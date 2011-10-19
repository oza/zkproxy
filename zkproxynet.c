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
#include <errno.h>

#include "util.h"
#include "net.h"
#include "event.h"
#include "work.h"
#include "logger.h"
#include "coroutine.h"
#include "zookeeper.h"
#include "zookeeper.jute.h"
#include "zk_adaptor.h"
#include "recordio.h"
#include "accord.h"


#define ntohll(x) (((uint64_t)(ntohl((int)((x << 32) >> 32))) << 32) | (unsigned int)ntohl(((int)(x >> 32)))) 
#define htonll(x) ntohll(x)

static int acrdport = 9090;
static char *hostname = "localhost";

extern int keepidle, keepintvl, keepcnt;
static int min_timeout = 2000;
static int max_timeout = 100000;
static int max_msg_size = 1024*1024;

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
	struct list_head tx_reqs;

	struct acrd_handle *ah;

	int fd;
	enum client_status status;
	unsigned int events;
	int nr_outstanding_reqs;
	uint64_t rx_len;
	uint64_t tx_len;
	uint64_t zxid;
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

struct response {
	struct oarchive *oa;
	struct list_head siblings;
	void (*callback)(struct client_info *);
};

static LIST_HEAD(client_info_list);



/* the size of connect request */
#define HANDSHAKE_REQ_SIZE 44

static uint64_t local_nodeid;

void leave_cb(struct acrd_handle *ah, const uint64_t *member_list,
	size_t member_list_entries, uint64_t nodeid, void* arg)
{

}

void join_cb(struct acrd_handle *ah, const uint64_t *member_list,
	size_t member_list_entries, uint64_t nodeid, void* arg)
{
	if (local_nodeid == 0)
		local_nodeid =  nodeid;

	printf("local_nodeid %ld\n", local_nodeid);
}

static uint64_t get_zxid(struct client_info *ci)
{
	return ++ci->zxid;
}

static void destroy_client(struct client_info *ci)
{
	printf("*** %s refcnt %d ***\n", __FUNCTION__, ci->refcnt);
	if (ci->ah)
		acrd_close(ci->ah);

	close(ci->fd);
	free(ci);
}

static void client_incref(struct client_info *ci)
{
	if (ci) {
		__sync_add_and_fetch(&ci->refcnt, 1);
		//printf("%s refcnt %d\n", __FUNCTION__, ci->refcnt);
	}
}

static void client_decref(struct client_info *ci)
{
	if (ci && __sync_sub_and_fetch(&ci->refcnt, 1) == 0)
		destroy_client(ci);

	//printf("%s refcnt %d\n", __FUNCTION__, ci->refcnt);
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

static int recv_buffer(int fd, void *buf, size_t len)
{
	int ret = 0, offset = 0;

	while (len) {
		ret = do_read(fd, buf + offset, len);

		if (ret < 0) {
			if (errno != EAGAIN)
				return -1;

			/* FIXME: use coroutine */
			//coroutine_yield();
			continue;
		}
		offset += ret;
		len -= ret;
	}

	return 0;
}

static int write_buffer(int fd, void *buf, size_t len)
{
        int ret = 0, offset = 0;

        while (len) {
                ret = do_write(fd, buf + offset, len);

                if (ret < 0) {
                        if (errno != EAGAIN)
                                return -1;

                        //coroutine_yield();
                        continue;
                }
                offset += ret;
                len -= ret;
        }

        return 0;
}

static inline int is_handshake_size(int len)
{
	return len == HANDSHAKE_REQ_SIZE;
}

static void queue_response(struct client_info *ci, struct response *res)
{
	list_add_tail(&res->siblings, &ci->tx_reqs);
}

static void set_status_connected(struct client_info *ci)
{
	printf("%s", __FUNCTION__);
	ci->status = CLIENT_STATUS_CONNECTED;
}

static void establish_connection(struct client_info *ci)
{
	int len, ret, fd = ci->fd;
	char *buf = NULL;
	struct response *r = NULL;
	struct ConnectRequest req;
	struct ConnectResponse res;
	//struct connect_res res;
	struct iarchive *ia;
	struct oarchive *oa;

	/* read header */
	ret = recv_buffer(fd, &len, sizeof(int));
	len = ntohl(len);
	if (ret < 0 || !is_handshake_size(len)) {
		printf("illegal msg size %d\n", len);
		ci->status = CLIENT_STATUS_DEAD;
		return;
	}

	buf = zalloc(len);
	if (buf == NULL) {
		printf("oom\n");
		return;
	}

	/* try to read connect header */
	ret = recv_buffer(fd, buf, len);
	if (ret < 0) {
		perror("unknown err\n");
		ci->status = CLIENT_STATUS_DEAD;
		return;
	}

	ia = create_buffer_iarchive(buf, len);
	deserialize_ConnectRequest(ia, "hdr", &req);
	printf("protocolVersion %d, lastZxidSeen %lu, timeout %u,"
			"sessionId %lu, buffer.size %d\n",
			req.protocolVersion, req.lastZxidSeen,
			req.timeOut,
			req.sessionId,
			req.passwd.len);
	close_buffer_iarchive(&ia);
	free(buf);

	/* start to send request */
	ci->ah = acrd_init("localhost", 9090, join_cb, leave_cb, ci);
	if (ci->ah == NULL) {
		ci->status = CLIENT_STATUS_DEAD;
		return;
	}

	/* try to handshake */
	res.passwd.buff = zalloc(16);
	r = zalloc(sizeof(struct response));
	/* FIXME: err handling */

        /* try to handshake */
        oa = create_buffer_oarchive();
        if (!oa)
                return;
        res.protocolVersion = req.protocolVersion;
        res.sessionId = 1;
	if (len > 16)
        	res.passwd.len = 16;
	else
        	res.passwd.len = req.passwd.len;
        memcpy(res.passwd.buff, req.passwd.buff, 16);
        if (req.timeOut < min_timeout)
                res.timeOut = min_timeout;
        else if (req.timeOut > max_timeout)
                res.timeOut = max_timeout;
        else
                res.timeOut = req.timeOut;

        serialize_ConnectResponse(oa, "rsp", &res);
	free(res.passwd.buff);

#if 0
        serialize_ConnectResponse(oa, "rsp", &res);
	len = get_buffer_len(oa);
	ret = write_buffer(fd, &len, sizeof(int));
	if (ret < 0) {
		printf("unknown err");
	}

	ret = write_buffer(fd, get_buffer(oa), len);
	if (ret < 0) {
		printf("unknown err");
	}
	ci->status = CLIENT_STATUS_CONNECTED;
	client_tx_on(ci);
#else
	r->oa = oa;
	r->callback = set_status_connected;
	queue_response(ci, r);
	client_tx_on(ci);
#endif

}

int is_valid_size(int len)
{
 	return (len > max_msg_size || len < 0);
}

void delegate_request(struct client_info *ci)
{
	int fd = ci->fd, len, nlen, ret, xid, type;
	char *buf =NULL;
	struct iarchive *ia;
	struct oarchive *oa;
	//struct response *r;
	struct RequestHeader rh;
	struct ReplyHeader h;

	ret = recv_buffer(fd, &len, sizeof(int));
	len = ntohl(len);
	printf("len %d\n", len);
	if (ret < 0 || is_valid_size(len) ) {
		printf("illegal msg size %d(%u)\n", len, len);
		ci->status = CLIENT_STATUS_DEAD;
		return;
	}

	/* read payload */
	buf = zalloc(len);
	ret = recv_buffer(fd, buf, len);
	if (ret < 0) {
		printf("illegal msg %d, ret %d\n", len, ret);
		ci->status = CLIENT_STATUS_DEAD;
		return;
	}

	ia = create_buffer_iarchive(buf, len);
	deserialize_RequestHeader(ia, "hdr", &rh);
	printf("xid %d, type %d --> ", rh.xid, rh.type);
	xid = rh.xid;
	type = rh.type;

	switch(type) {
	case CREATE_OP:
	{
		struct CreateRequest crereq;
		struct CreateResponse creres;

		deserialize_CreateRequest(ia, "req", &crereq);
		printf("create. path %s data %s\n", crereq.path, crereq.data.buff);
		ret = acrd_write(ci->ah, crereq.path, crereq.data.buff,
			crereq.data.len, 0, ACRD_FLAG_CREATE);

		if (ret == ACRD_SUCCESS) {
			h.err = ZOK;
		} else if (ret == ACRD_ERR_EXIST){
			h.err = ZNODEEXISTS;
		} else {
			/* FIXME : err handling correctly. */
			h.err = ZAPIERROR;
		}
		h.xid = rh.xid;
		h.zxid = get_zxid(ci);
		creres.path = crereq.path;

		oa = create_buffer_oarchive();
		serialize_ReplyHeader(oa, "rsp", &h);
		serialize_CreateResponse(oa, "rsp", &creres);

		len = get_buffer_len(oa);
		nlen = htonl(len);
		ret = write_buffer(fd, &nlen, sizeof(nlen));
		if (ret < 0) {
			ci->status = CLIENT_STATUS_DEAD;
			return;
		}
		ret = write_buffer(fd, get_buffer(oa), len);
		if (ret < 0) {
			ci->status = CLIENT_STATUS_DEAD;
			return;
		}

		break;
	}
	case DELETE_OP:
	{
		printf("delete. not yet implemented.\n");
		break;
	}
	case EXISTS_OP:
	{
		printf("exists. not yet implemented.\n");
		break;
	}
	case GETDATA_OP:
	{
		struct GetDataRequest getreq;
		struct GetDataResponse getres;
		uint32_t cnt = max_msg_size;

		deserialize_GetDataRequest(ia, "req", &getreq);
		printf("contents of getreq: path %s, watch %d\n",
			getreq.path, getreq.watch);

		/* FIXME: This approach is too naive, but safe. */
		buf = zalloc(max_msg_size);
		ret = acrd_read(ci->ah, getreq.path, buf,
			&cnt, 0, 0);

		if (ret == ACRD_SUCCESS) {
			h.err = ZOK;
			printf("SUCCESS\n");
		} else if (ret == ACRD_ERR_NOTFOUND){
			h.err = ZNONODE;
		} else {
			/* FIXME : err handling correctly. */
			h.err = ZAPIERROR;
		}
		h.xid = rh.xid;
		h.zxid = get_zxid(ci);

		getres.data.buff = buf;
		getres.data.len = cnt;

		printf("buf %s len %d ret %d\n", buf, cnt, ret);

		oa = create_buffer_oarchive();
		serialize_ReplyHeader(oa, "rsp", &h);
		serialize_GetDataResponse(oa, "rsp", &getres);

		len = get_buffer_len(oa);
		nlen = htonl(len);
		ret = write_buffer(fd, &nlen, sizeof(nlen));
		if (ret < 0) {
			ci->status = CLIENT_STATUS_DEAD;
			return;
		}
		ret = write_buffer(fd, get_buffer(oa), len);
		if (ret < 0) {
			ci->status = CLIENT_STATUS_DEAD;
			return;
		}

		break;
	}
	case SETDATA_OP:
	{
		struct SetDataRequest setreq;
		struct SetDataResponse setres;

		printf("set. Note that watch request is igonored current version.\n");

		deserialize_SetDataRequest(ia, "req", &setreq);
		printf("contents of setreq: path %s, setreq %s version %d\n", 
			setreq.path, setreq.data.buff, setreq.version);

		ret = acrd_write(ci->ah, setreq.path, setreq.data.buff,
			setreq.data.len, 0, 0);
		if (ret == ACRD_SUCCESS) {
			h.err = ZOK;
		} else if (ret == ACRD_ERR_NOTFOUND){
			h.err = ZNONODE;
		} else {
			/* FIXME : err handling correctly. */
			h.err = ZAPIERROR;
		}

		h.xid = rh.xid;
		h.zxid = get_zxid(ci);
		memset(&setres, 0, sizeof(struct Stat));

		oa = create_buffer_oarchive();
		serialize_ReplyHeader(oa, "rsp", &h);
		serialize_SetDataResponse(oa, "rsp", &setres);

		len = get_buffer_len(oa);
		nlen = htonl(len);
		ret = write_buffer(fd, &nlen, sizeof(nlen));
		if (ret < 0) {
			ci->status = CLIENT_STATUS_DEAD;
			return;
		}
		ret = write_buffer(fd, get_buffer(oa), len);
		if (ret < 0) {
			ci->status = CLIENT_STATUS_DEAD;
			return;
		}
	}
		break;
	case SYNC_OP:
		printf("sync. not yet implemented.\n");
		break;
	case PING_OP:
		if (rh.xid != PING_XID) {
			ci->status = CLIENT_STATUS_DEAD;
			return;
		}

		h.xid = -2;
		h.zxid = get_zxid(ci); //rh.xzid;
		h.err = 0;
		oa = create_buffer_oarchive();
		serialize_ReplyHeader(oa, "rsp", &h);

#if 1
		len = get_buffer_len(oa);
		nlen = htonl(len);
		ret = write_buffer(fd, &nlen, sizeof(int));
		if (ret < 0) {
			ci->status = CLIENT_STATUS_DEAD;
			return;
		}

		ret = write_buffer(fd, get_buffer(oa), len);
		if (ret < 0) {
			ci->status = CLIENT_STATUS_DEAD;
			return;
		}
		printf("ping. len %d\n", len);
		close_buffer_oarchive(&oa, 0);
#else
		r = zalloc(sizeof(struct response));
		r->oa = oa;
		r->callback = NULL;
		queue_response(ci, r);
#endif
		break;
	case SETAUTH_OP:
		if (rh.xid != AUTH_XID) {
			ci->status = CLIENT_STATUS_DEAD;
			return;
		}
		printf("setauth xid %d, type %d\n", rh.xid, rh.type);
		h.xid = AUTH_XID;
		h.zxid = 3;
		h.err = ZOK;
		oa = create_buffer_oarchive();
		serialize_ReplyHeader(oa, "rsp", &h);

		len = get_buffer_len(oa);
		write_buffer(fd, &len, sizeof(int));
		write_buffer(fd, get_buffer(oa), len);
		break;
	case SETWATCHES_OP:
		if (rh.xid != SET_WATCHES_XID) {
			ci->status = CLIENT_STATUS_DEAD;
			return;
		}
		printf("set watch xid %d, type %d\n", rh.xid, rh.type);
		h.xid = SET_WATCHES_XID;
		h.zxid = 3;
		h.err = ZOK;
		oa = create_buffer_oarchive();
		serialize_ReplyHeader(oa, "rsp", &h);

		len = get_buffer_len(oa);
		write_buffer(fd, &len, sizeof(int));
		write_buffer(fd, get_buffer(oa), len);
		break;
	default:
		printf("not yet implemented.\n");
		abort();
	}

	close_buffer_iarchive(&ia);
	free(buf);
	client_tx_on(ci);
#if 0
	int len, ret, fd = ci->fd;
	char *buf = NULL;
	struct response *r = NULL;
	struct ConnectRequest req;
	struct ConnectResponse res;
	//struct connect_res res;
	struct iarchive *ia;
	struct oarchive *oa;

printf("hoge");
	/* read header */
	ret = recv_buffer(fd, &len, sizeof(int));
	len = ntohl(len);
	if (ret < 0 || !is_handshake_size(len)) {
		printf("illegal msg size %d\n", len);
		return;
	}

	buf = zalloc(len);
	if (buf == NULL) {
		printf("oom\n");
		return;
	}

	/* try to read connect header */
	ret = recv_buffer(fd, buf, len);
	if (ret < 0) {
		perror("unknown err\n");
		abort();
	}
#endif

}

static void client_rx_handler(void *opaque)
{
	struct client_info *ci = opaque;
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
	struct client_info *ci = opaque;
	struct response *res, *tmpres;
	struct oarchive *oa;
	int len = 0, ret = 0, fd = ci->fd;
	printf("handle request, status %d\n", ci->status);

	list_for_each_entry_safe(res, tmpres, &ci->tx_reqs, siblings) {
		oa = res->oa;
		len = get_buffer_len(oa);
		ret = write_buffer(fd, &len, sizeof(int));
		if (ret < 0) {
			printf("unknown err\n");
		}

		ret = write_buffer(fd, get_buffer(oa), len);
		if (ret < 0) {
			printf("unknown err\n");
		}

		if (res->callback)
			res->callback(ci);
		close_buffer_oarchive(&oa, 0);
		list_del(&res->siblings);
		free(res);
	}
}

static void client_handler(int fd, int events, void *data)
{
	struct client_info *ci = (struct client_info *)data;

	if (events & EPOLLIN) {
		//printf("read, %d\n", ci->fd);
		assert(ci->rx_list.next == NULL);

		client_incref(ci);
		client_rx_off(ci);
		client_rx_handler(ci);
		//coroutine_enter(&ci->rx_co, ci);
		client_rx_on(ci);
		client_decref(ci);
	}

	if (events & EPOLLOUT) {
		//printf("write, %d\n", fd);
		client_incref(ci);
		client_tx_off(ci);
		client_tx_handler(ci);
		client_decref(ci);
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

static struct client_info *create_client(int fd)
{
	struct client_info *ci;

	ci = zalloc(sizeof(*ci));
	if (!ci)
		return NULL;

	ci->rx_co = coroutine_create(client_rx_handler);
	ci->tx_co = coroutine_create(client_tx_handler);

	ci->fd = fd;
	ci->refcnt = 1;
	ci->status = CLIENT_STATUS_CONNECTING;
	ci->zxid = 0;

	list_add_tail(&ci->siblings, &client_info_list);
	INIT_LIST_HEAD(&ci->tx_reqs);

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
