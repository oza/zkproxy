/*
 * zkproxy
 *
 * Copyright (C) 2011 OZAWA Tsuyoshi
 *
 *    Licensed under the Apache License, Version 2.0 (the "License");
 *    you may not use this file except in compliance with the License.
 *    You may obtain a copy of the License at
 *
 *        http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing, software
 *    distributed under the License is distributed on an "AS IS" BASIS,
 *    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *    See the License for the specific language governing permissions and
 *    limitations under the License.
 */

#include <unistd.h>
#include <arpa/inet.h>
#include <sys/epoll.h>
#include <assert.h>

#include "util.h"
#include "net.h"
#include "event.h"
#include "work.h"
#include "logger.h"
#include "coroutine.h"
#include "zookeeper.h"
#include "zookeeper.jute.h"
#include "recordio.h"

extern int keepidle, keepintvl, keepcnt;

struct client_id {
	union {
		struct {
			uint32_t nodeid;
			uint32_t seq_no;
		};
		uint64_t id;
	};
};

enum client_status {
	CLIENT_STATUS_CONNECTED,
	CLIENT_STATUS_JOINED,
	CLIENT_STATUS_DEAD,
	CLIENT_STATUS_CONNECTING,
};

struct client_info {
	struct client_id cid;
	struct list_head siblings;

	int fd;
	enum client_status status;
	unsigned int events;
	int nr_outstanding_reqs;
	uint64_t rx_len;
	uint64_t tx_len;
	struct co_buffer rx_buf;

	struct coroutine *rx_co;
	struct coroutine *tx_co;
	int tx_on; /* if true, send_response() is sending response through
		    * this connection */

	int tx_failed;
	int stop; /* if true, the connection is not ready for read
		   * operations because of too many requests */

	int refcnt;

};

static void destroy_client(struct client_info *ci)
{
	close(ci->fd);
	free(ci);
}

static void client_incref(struct client_info *ci)
{
	if (ci)
		__sync_add_and_fetch(&ci->refcnt, 1);
}

static void client_decref(struct client_info *ci)
{
	if (ci && __sync_sub_and_fetch(&ci->refcnt, 1) == 0) {
		destroy_client(ci);
		return;
	}
}

static void client_rx_on(struct client_info *ci)
{
	ci->events |= EPOLLIN;
	modify_event(ci->fd, ci->events);
}

static void client_rx_off(struct client_info *ci)
{
	ci->events &= ~EPOLLIN;
	modify_event(ci->fd, ci->events);
}

static void client_tx_on(struct client_info *ci)
{
	ci->events |= EPOLLOUT;
	modify_event(ci->fd, ci->events);
}

static void client_tx_off(struct client_info *ci)
{
	ci->events &= ~EPOLLOUT;
	modify_event(ci->fd, ci->events);
}

#if 0
static void check_cmd()
{
	/**
	 * conf
	 * cons
	 * crst
	 * dump
	 * gtmk
	 * ruok
	 * stmk
	 * srvr
	 * srst
	 * wchc
	 * wchp
	 * wchs
	 */
}
#endif


static void establish_connection(struct client_info *ci)
{
	int fd = ci->fd;
	/* read header */
	struct ConnectRequest cr;
	int len;
	read(fd, &len, sizeof(int));
	len = ntohl(len);
	printf("len %d\n", len);

	/* read payload */
	char buf[len];
	read(fd, buf, len);
	struct iarchive *ia = create_buffer_iarchive(buf, len);
	deserialize_ConnectRequest(ia, "hdr", &cr);
	printf("protocolVersion %d, lastZxidSeen %lu, timeout %u, \
			sessionId %lu, buffer.size %d\n", cr.protocolVersion, cr.lastZxidSeen,
			cr.timeOut, cr.sessionId, cr.passwd.len);
	close_buffer_iarchive(&ia);

	/* try to handshake */
	struct oarchive *oa = create_buffer_oarchive();
	struct ConnectResponse res;
	res.protocolVersion = cr.protocolVersion;
	res.timeOut = cr.timeOut;
	res.sessionId = 1;
	res.passwd.len = len;
	serialize_ConnectResponse(oa, "rsp", &res);
	len = get_buffer_len(oa);
	write(fd, &len, sizeof(int));
	write(fd, get_buffer(oa), len);
	close_buffer_oarchive(&oa, 0);

	ci->status = CLIENT_STATUS_CONNECTED;
}

void delegate_request(struct client_info *ci)
{
	/*
	int fd = ci->fd;
	int len;
	read(fd, &len, sizeof(int));
	len = ntohl(len);
	printf("len %d\n", len);
	*/

	/* read payload */
	/*
	char buf[len];
	read(fd, buf, len);
	struct iarchive *ia = create_buffer_iarchive(buf, len);
	struct RequestHeader rh;
	deserialize_RequestHeader(ia, "hdr", &rh);
	printf("xid %d, type %d", rh.xid, rh.type);
	close_buffer_iarchive(&ia);
	*/
	while(1);
}

#if 0
static void handle_request(struct client_info *ci)
{
	printf("handle request, status %d\n", ci->status);
	switch(ci->status) {
	case CLIENT_STATUS_CONNECTING:
		printf("connecting. start to establish connection\n");
		establish_connection(ci);
		break;
	case CLIENT_STATUS_CONNECTED:
		printf("start to delegate connection\n");
		delegate_request(ci);
		break;
	default:
		printf("not yet implemented!");

	}
}
#endif

static void client_handler(int fd, int events, void *data)
{
	struct client_info *ci = (struct client_info *)data;

	if (events & EPOLLIN) {
		printf("read, %d\n", fd);
		printf("read, %d\n", ci->fd);
		assert(ci->rx_list.next == NULL);


		client_incref(ci);
		client_rx_off(ci);
		coroutine_enter(ci->rx_co, ci);
		client_rx_on(ci);
		//handle_request(ci);
	}

	if (events & EPOLLOUT) {
		printf("write, %d\n", fd);
		ci->tx_on = 1;
		//client_tx_off(ci);

		client_incref(ci);
	}

	if (ci->status == CLIENT_STATUS_DEAD) {
		printf("closed a connection, %d\n", fd);
		unregister_event(fd);

		//remove_all_watch(ci);

		list_del(&ci->siblings);
		//notify_node_event(ci->cid.id, ACRD_EVENT_LEFT);

		client_decref(ci);
	}
}

static void client_rx_handler(void *opaque)
{
	struct client_info *ci = opaque;
	printf("handle request, status %d\n", ci->status);
	switch(ci->status) {
	case CLIENT_STATUS_CONNECTING:
		printf("connecting. start to establish connection\n");
		establish_connection(ci);
		break;
	case CLIENT_STATUS_CONNECTED:
		printf("start to delegate connection\n");
		delegate_request(ci);
		break;
	default:
		printf("not yet implemented!");

	}
}

static void client_tx_handler(void *opaque)
{
}


static struct client_info *create_client(int fd)
{
	struct client_info *ci;

	ci = zalloc(sizeof(*ci));
	if (!ci)
		return NULL;

	ci->rx_co = coroutine_create(client_rx_handler);
	ci->tx_co = coroutine_create(client_tx_handler);

	ci->fd = fd;
	ci->status = CLIENT_STATUS_CONNECTING;

	dprintf("ci %p", ci);

	return ci;
}

static void listen_handler(int listen_fd, int events, void *data)
{
	struct sockaddr_storage from;
	socklen_t namesize;
	int fd, ret;
	struct client_info *ci;

	namesize = sizeof(from);
	fd = accept(listen_fd, (struct sockaddr *)&from, &namesize);
	if (fd < 0) {
		eprintf("can't accept a new connection, %m\n");
		return;
	}

	ret = set_nodelay(fd);
	if (ret) {
		close(fd);
		return;
	}

	ret = set_nonblocking(fd);
	if (ret) {
		close(fd);
		return;
	}

	ret = set_keepalive(fd, keepidle, keepintvl, keepcnt);
	if (ret) {
		close(fd);
		return;
	}

	ci = create_client(fd);
	if (!ci) {
		close(fd);
		return;
	}

	printf("connected\n");

	ret = register_event(fd, client_handler, ci);
	if (ret) {
		destroy_client(ci);
		return;
	}

	printf("accepted a new connection, %d ci->fd %d ci %p\n",
		fd, ci->fd, ci);
}

static int create_listen_port_fn(int fd, void *data)
{
	return register_event(fd, listen_handler, data);
}

int create_listen_port(int port, void *data)
{
	return create_listen_ports(port, create_listen_port_fn, data);
}

int init_acrd_work_queue(int in_memory)
{
	/*
	int i;
	for (i = 0; i < NR_RECV_THREAD; i++) {
		recv_queue[i] = init_work_queue(recv_request, RECV_INTERVAL);
		if (!recv_queue[i])
			return -1;
	}
	*/


	return 0;
}
