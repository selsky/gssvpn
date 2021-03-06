/*
 * Copyright 2011 Jonathan Reams
 * 
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 * 
 * http://www.apache.org/licenses/LICENSE-2.0
 * 
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "config.h"
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <netdb.h>
#include <pwd.h>
#include <fcntl.h>
#include <stdio.h>
#include <gssapi/gssapi.h>
#include <unistd.h>
#include <net/if.h>
#include <net/ethernet.h>
#include <arpa/inet.h>
#if defined(HAVE_IF_TUN)
#include <linux/if_tun.h>
#endif
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <syslog.h>
#include "libev/ev.h"
#define GSSVPN_SERVER
#include "gssvpn.h"

#define NETINIT_BUFLEN 4096

struct conn * clients_ip[255];
struct conn * clients_ether[255];
gss_cred_id_t srvcreds = GSS_C_NO_CREDENTIAL;
int verbose = 0;
int daemonize = 0;
int killontimeout = 0;
char * netinit_util = NULL;

int tapfd = -1, netfd = -1;
const uint64_t ether_broadcast = 0xffffffffffffffff;
const uint64_t ether_empty = 0x0000000000000000;

int get_server_creds(gss_cred_id_t * sco, char * service_name) {
	gss_buffer_desc name_buff;
	gss_name_t server_name;
	OM_uint32 maj_stat, min_stat;

	name_buff.value = service_name;
	name_buff.length = strlen(service_name);
	maj_stat = gss_import_name(&min_stat, &name_buff,
					(gss_OID) GSS_C_NT_HOSTBASED_SERVICE,
				   	&server_name);

	maj_stat = gss_acquire_cred(&min_stat, server_name, 0,
					GSS_C_NO_OID_SET, GSS_C_ACCEPT,
					sco, NULL, NULL);

	gss_release_name(&min_stat, &server_name);
	if(maj_stat != GSS_S_COMPLETE) {
		logit(1, "Error acquiring server credentials.");
		display_gss_err(maj_stat, min_stat);
		return -1;
	} else if(verbose)
		logit(-1, "Acquired credentials for %s", service_name);
	return 0;
}

void handle_shutdown(struct conn * client) {
	OM_uint32 min;

	logit(0, "Shutting down client %s:%d (%s)",
		client->ipstr, client->addr.sin_port, client->princname);

	unlink_conn(client, CLIENT_ALL);
	if(client->context != GSS_C_NO_CONTEXT)
		gss_delete_sec_context(&min, &client->context, NULL);

	if(client->ni.value != NULL) {
		if(ev_is_active(&client->nipipe))
			ev_io_stop(client->loop, &client->nipipe);
		if(ev_is_active(&client->nichild))
			ev_child_stop(client->loop, &client->nichild);
		free(client->ni.value);
	}

	if(ev_is_active(&client->conntimeout))
		ev_timer_stop(client->loop, &client->conntimeout);
	if(client->princname)
		free(client->princname);

	free(client);
}

void netinit_read_cb(struct ev_loop * loop, ev_io * ios, int revents) {
	struct conn * c = (struct conn*)ios->data;
	ssize_t r, tocopy = NETINIT_BUFLEN;

	if(!c->ni.value) {
		logit(1, "Called netinit read for a null pointer!!");
		return;
	}

	do {
		if(c->ni.length)
			tocopy = NETINIT_BUFLEN - c->ni.length;

		r = read(ios->fd, c->ni.value + c->ni.length, tocopy);
		if(r > 0)
			c->ni.length += r;
	} while(r > 0 && c->ni.length < NETINIT_BUFLEN);
	if(c->ni.length == NETINIT_BUFLEN)
		ev_io_stop(loop, ios);
}

void conn_timeout_cb(struct ev_loop * loop, ev_timer * iot, int revents) {
	struct conn * c = (struct conn*)iot->data;
	OM_uint32 maj, min, timeout;

	maj = gss_context_time(&min, c->context, &timeout);
	if(maj == GSS_S_COMPLETE && timeout > 0) {
		iot->repeat = timeout;
		ev_timer_again(loop, iot);
		return;
	}

	if(maj == GSS_S_CONTEXT_EXPIRED ||
		maj == GSS_S_CREDENTIALS_EXPIRED) {
		if(killontimeout) {
			logit(0, "Connection %s (%s:%d) has timed out. Shutting down.",
				c->princname, c->ipstr, c->addr.sin_port);
			send_packet(netfd, NULL, &c->addr, PAC_SHUTDOWN, c->sid);
			handle_shutdown(c);
		} else {
			logit(0, "Connection %s (%s:%d) has timed out. Requesting GSSINIT.",
				c->princname, c->ipstr, c->addr.sin_port);
			send_packet(netfd, NULL, &c->addr, PAC_GSSINIT, c->sid);
		}
	}
	ev_timer_stop(loop, iot);
}

void netinit_child_cb(struct ev_loop * loop, ev_child * ioc, int revents) {
	struct conn * c = (struct conn*)ioc->data;

	ev_child_stop(loop, ioc);
	if(ev_is_active(&c->nipipe)) {
		netinit_read_cb(loop, &c->nipipe, EV_READ);
		if(ev_is_active(&c->nipipe))	
			ev_io_stop(loop, &c->nipipe);
	}

	if(ioc->rstatus != 0) {
		logit(0, "Rejecting client %s:%d (%s)", c->ipstr,
			c->addr.sin_port, c->princname);
		send_packet(netfd, NULL, &c->addr, PAC_SHUTDOWN, c->sid);
		handle_shutdown(c);
		return;
	}

	send_packet(netfd, c->ni.length > 0 ? &c->ni : NULL,
		&c->addr, PAC_NETINIT, c->sid);
	free(c->ni.value);
	c->ni.length = 0;
	c->ni.value = NULL;
	logit(0, "Client %s:%d (%s) is starting normal operation",
		c->ipstr, c->addr.sin_port, c->princname);
}

void tapfd_read_cb(struct ev_loop * loop, ev_io * ios, int revents) {
	uint8_t framebuf[1550], dstmac[6];
	ssize_t size = read(ios->fd, framebuf, sizeof(framebuf));
	gss_buffer_desc plaintext = { size, framebuf }; 
	int rc;
	OM_uint32 lmin;

	if(size < 0 && errno == EAGAIN)
		return;

	memcpy(dstmac, framebuf, sizeof(dstmac));
	if(memcmp(dstmac, &ether_broadcast, sizeof(dstmac)) == 0) {
		uint8_t i;
		for(i = 0; i < 255; i++) {
			struct conn * cur = clients_ether[i];
			while(cur) {
				if(cur->context == GSS_C_NO_CONTEXT ||
					cur->gssstate == GSS_S_CONTINUE_NEEDED) {
					logit(-1, "Dropping packet for tap");
					cur = cur->ethernext;
					continue;
				}
				rc = send_packet(netfd, &plaintext, &cur->addr,
					PAC_DATA, cur->sid);
				if(rc == -2) {
					logit(1, "Reinitializing GSSAPI context");
					if(cur->context != GSS_C_NO_CONTEXT) {
						gss_delete_sec_context(&lmin, &cur->context, NULL);
						cur->context = GSS_C_NO_CONTEXT;
					}
					send_packet(netfd, NULL, &cur->addr,
						PAC_GSSINIT, cur->sid);
				}
				cur = cur->ethernext;
			}
		}
		return;
	}
	uint8_t eh = hash(dstmac, sizeof(dstmac));
	struct conn * client = clients_ether[eh];
	while(client && memcmp(client->mac, dstmac, sizeof(dstmac)) != 0)
		client = client->ethernext;
	if(!client) {
		logit(-1, "Received packet for unknown client");
		return;
	}

	rc = send_packet(netfd, &plaintext, &client->addr,
		PAC_DATA, client->sid);
	if(rc == -2) {
		logit(1, "Reinitializing GSSAPI context");
		if(client->context != GSS_C_NO_CONTEXT) {
			gss_delete_sec_context(&lmin, &client->context, NULL);
			client->context = GSS_C_NO_CONTEXT;
		}
		send_packet(netfd, NULL, &client->addr,
			PAC_GSSINIT, client->sid);
	}
}

void handle_netinit(struct ev_loop * loop, struct conn * client,
	gss_buffer_desc * macbuf) {
	pid_t pid;
	int fds[2];

	if(ev_is_active(&client->nichild))
		return;

	if(macbuf->length < sizeof(client->mac))
		return;

	if(memcmp(macbuf->value, client->mac, sizeof(client->mac)) != 0) {
		uint8_t eh;
		memcpy(client->mac, macbuf->value, sizeof(client->mac));
		eh = hash(client->mac, sizeof(client->mac));
		unlink_conn(client, CLIENT_ETHERNET);
		client->ethernext = clients_ether[eh];
		clients_ether[eh] = client;
	}

	if(!netinit_util) {
		send_packet(netfd, NULL, &client->addr, PAC_NETINIT, client->sid);
		return;
	}

	if(pipe(fds) < 0) {
		logit(1, "Error creating pipe during netinit %s", strerror(errno));
		send_packet(netfd, NULL, &client->addr, PAC_SHUTDOWN, client->sid);
		handle_shutdown(client);
		return;
	}

	if(fcntl(fds[0], F_SETFL, O_NONBLOCK) < 0) {
		logit(1, "Error setting pipe to non-blocking during netinit %s",
			strerror(errno));
		send_packet(netfd, NULL, &client->addr, PAC_SHUTDOWN, client->sid);
		handle_shutdown(client);
		return;
	}

	client->ni.value = malloc(NETINIT_BUFLEN);
	client->ni.length = 0;
	client->loop = loop;

	ev_io_init(&client->nipipe, netinit_read_cb, fds[0], EV_READ);
	client->nipipe.data = client;
	ev_io_start(loop, &client->nipipe);

	pid = fork();
	if(pid == 0) {
		char portstr[6];
		char * filename = netinit_util + (strlen(netinit_util) - 1);

		close(netfd);
		close(tapfd);
		close(fds[0]);
		while(*filename != '/' && filename != netinit_util)
			filename--;
		if(*filename == '/')
			filename++;

		sprintf(portstr, "%d", client->addr.sin_port);

		dup2(fds[1], fileno(stdout));
		if(execl(netinit_util, filename, client->princname,
			client->ipstr, portstr, NULL) < 0)
			exit(-1);
	}
	
	logit(-1, "Waiting for netinit util to finish for %s:%d (%s)",
		client->ipstr, client->addr.sin_port, client->princname);
	ev_child_init(&client->nichild, netinit_child_cb, pid, 0);
	client->nichild.data = client;
	ev_child_start(loop, &client->nichild);
}

void handle_gssinit(struct ev_loop * loop, struct conn * client,
	gss_buffer_desc * intoken) {
	gss_name_t client_name;
	gss_buffer_desc output, nameout;
	OM_uint32 flags, lmin, maj, min, timeout;

	if(client->gssstate == GSS_S_COMPLETE && 
		client->context != GSS_C_NO_CONTEXT) {
		gss_delete_sec_context(&lmin, &client->context, NULL);
		client->context = GSS_C_NO_CONTEXT;
	}

	if(ev_is_active(&client->conntimeout))
		ev_timer_stop(loop, &client->conntimeout);

	maj = gss_accept_sec_context(&min, &client->context, srvcreds, intoken,
					NULL, &client_name, NULL, &output, &flags, &timeout, NULL);
	if(maj != GSS_S_COMPLETE && maj != GSS_S_CONTINUE_NEEDED) {
		logit(1, "Error accepting security context from %s", client->ipstr);
		display_gss_err(maj, min);
		return;
	}
	client->gssstate = maj;
	if(output.length > 0) {
		send_packet(netfd, &output, &client->addr,
			PAC_GSSINIT, client->sid);
		gss_release_buffer(&lmin, &output);
	}

	if(maj == GSS_S_CONTINUE_NEEDED) {
		logit(0, "Continue needed for GSSAPI auth");
		return;
	}

	gss_display_name(&lmin, client_name, &nameout, NULL);
	logit(0, "Accepted connection for %s from %s",
		nameout.value, client->ipstr);
	client->princname = strdup(nameout.value);
	gss_release_buffer(&lmin, &nameout);
	gss_release_name(&lmin, &client_name);

	if(timeout != GSS_C_INDEFINITE) {
		if(ev_is_active(&client->conntimeout))
			ev_timer_stop(loop, &client->conntimeout);
		else
			ev_init(&client->conntimeout, conn_timeout_cb);
		client->conntimeout.data = client;
		conn_timeout_cb(loop, &client->conntimeout, EV_TIMER);
	}
	if(memcmp(client->mac, &ether_empty, sizeof(client->mac)) == 0)
		send_packet(netfd, NULL, &client->addr,
			PAC_NETSTART, client->sid);
}

void netfd_read_cb(struct ev_loop * loop, ev_io * ios, int revents) {
	gss_buffer_desc packet = GSS_C_EMPTY_BUFFER;
	char pac;
	struct sockaddr_in peer;
	struct conn * client;
	OM_uint32 min;
	uint16_t sid;
	int rc = recv_packet(netfd, &packet, &pac, &peer, &sid);
	if(rc == -2) {
		client = get_conn(&peer, sid);
		logit(1, "Reinitializing GSSAPI context");
		if(client->context != GSS_C_NO_CONTEXT) {
			gss_delete_sec_context(&min, &client->context, NULL);
			client->context = GSS_C_NO_CONTEXT;
		}
		send_packet(netfd, NULL, &client->addr, PAC_GSSINIT, sid);
		return;
	} else if(rc < 0)
		return;
	else if((client = get_conn(&peer, sid)) == NULL)
		return;

	if(memcmp(&client->addr, &peer, sizeof(client->addr)) != 0) {
		memcpy(&client->addr, &peer, sizeof(client->addr));
		inet_ntop(peer.sin_family, &peer.sin_addr,
			client->ipstr, sizeof(client->ipstr));
	}

	if((client->gssstate == GSS_S_CONTINUE_NEEDED ||
		client->context == GSS_C_NO_CONTEXT) && pac != PAC_GSSINIT) {
		send_packet(netfd, NULL, &client->addr, PAC_GSSINIT, sid);
		if(packet.length)
			gss_release_buffer(&min, &packet);
		return;
	}

	if(pac == PAC_DATA &&
		memcmp(client->mac, &ether_empty, sizeof(client->mac)) == 0) {
		logit(-1, "Received data packet for uninitialized client %s (%s:%d)",
			client->princname, client->ipstr, client->addr.sin_port);
		if(packet.length > 0)
			free(packet.value);
		return;
	}

	if(pac == PAC_DATA && packet.length > 0) {
		logit(-1, "Writing %d bytes to tap", packet.length);
		size_t s = write(tapfd, packet.value, packet.length);
		if(s < 0)
			logit(1, "Error writing to tap: %s", strerror(errno));
		gss_release_buffer(&min, &packet);
	}
	else if(pac == PAC_GSSINIT)
		handle_gssinit(loop, client, &packet);
	else if(pac == PAC_NETINIT)
		handle_netinit(loop, client, &packet);
	else if(pac == PAC_SHUTDOWN)
		handle_shutdown(client);
	else if(pac == PAC_ECHO)
		send_packet(netfd, NULL, &client->addr, PAC_ECHO, sid);

	if(packet.value)
		gss_release_buffer(&min, &packet);
}

void term_cb(struct ev_loop * l, ev_signal * w, int r) {
	uint8_t i;
	
	for(i = 0; i < 255; i++) {
		struct conn * c = clients_ip[i];
		while(c) {
			struct conn * save = c->ipnext;
			send_packet(netfd, NULL, &c->addr, PAC_SHUTDOWN, c->sid);
			handle_shutdown(c);
			c = save;
		}
	}

	close(tapfd);
	close(netfd);

	ev_break(l, EVBREAK_ALL);
}

int main(int argc, char ** argv) {
	int rc;
	ev_io tapio, netio;
	ev_signal term;
	struct ev_loop * loop;
	openlog("gssvpnd", 0, LOG_DAEMON);
	char ch, *tapdev = NULL;
	short port = 2106;
	uid_t dropto = 0;

	while((ch = getopt(argc, argv, "ds:p:i:va:u:t")) != -1) {
		switch(ch) {
			case 'v':
				verbose = 1;
				break;
			case 'p':
				port = atoi(optarg);
				break;
			case 'i':
				tapdev = strdup(optarg);
				break;
			case 's':
				rc = get_server_creds(&srvcreds, optarg);
				if(rc != 0)
					return -1;
				break;
			case 'a': {
				if(access(optarg, R_OK|X_OK) < 0) {
					logit(1, "Unable to access %s for read/execute: %s",
						optarg, strerror(errno));
					return -1;
				}
				netinit_util = strdup(optarg);
				break;
			}
			case 'u': {
				struct passwd * u = getpwnam(optarg);
				if(!u) {
					logit(1, "Error doing user lookup for %s: (%s)",
						optarg, strerror(errno));
					return -1;
				}
				dropto = u->pw_uid;
			}
			case 't':
				killontimeout = 1;				
			case 'd':
				daemonize = 1;
		}
	}

	if(srvcreds == GSS_C_NO_CREDENTIAL) {
		rc = get_server_creds(&srvcreds, "gssvpn");
		if(rc != 0)
			return -1;
	}

	if((netfd = open_net(port)) < 0)
		return -1;

	if((tapfd = open_tap(&tapdev)) < 0)
		logit(1, "No tap device defined");

	if(dropto)
		setuid(dropto);
	
	if(daemonize)
		daemon(0, 0);
	
	memset(clients_ip, 0, sizeof(struct conn*) * 255);
	memset(clients_ether, 0, sizeof(struct conn*) * 255);

	loop = ev_default_loop(0);
	ev_io_init(&netio, netfd_read_cb, netfd, EV_READ);
	ev_io_start(loop, &netio);
	ev_io_init(&tapio, tapfd_read_cb, tapfd, EV_READ);
	ev_io_start(loop, &tapio);
	ev_signal_init(&term, term_cb, SIGTERM | SIGQUIT);
	ev_signal_start(loop, &term);
	ev_run(loop, 0);

	return 0;
}
