/*
 * Copyright (c) 2013 Mellanox Technologies®. All rights reserved.
 *
 * This software is available to you under a choice of one of two licenses.
 * You may choose to be licensed under the terms of the GNU General Public
 * License (GPL) Version 2, available from the file COPYING in the main
 * directory of this source tree, or the Mellanox Technologies® BSD license
 * below:
 *
 *      - Redistribution and use in source and binary forms, with or without
 *        modification, are permitted provided that the following conditions
 *        are met:
 *
 *      - Redistributions of source code must retain the above copyright
 *        notice, this list of conditions and the following disclaimer.
 *
 *      - Redistributions in binary form must reproduce the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer in the documentation and/or other materials
 *        provided with the distribution.
 *
 *      - Neither the name of the Mellanox Technologies® nor the names of its
 *        contributors may be used to endorse or promote products derived from
 *        this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include <xio_predefs.h>
#include <xio_env.h>
#include <xio_os.h>
#include "libxio.h"
#include "xio_log.h"
#include "xio_common.h"
#include "xio_observer.h"
#include "xio_protocol.h"
#include "xio_mbuf.h"
#include "xio_task.h"
#include "xio_mempool.h"
#include "xio_sg_table.h"
#include "xio_transport.h"
#include "xio_usr_transport.h"
#include "xio_ev_data.h"
#include "xio_objpool.h"
#include "xio_workqueue.h"
#include "xio_context.h"
#include "xio_ucx_transport.h"
#include "xio_mem.h"
#include "xio_ucx_transport.h"

/* default option values */
#define XIO_OPTVAL_DEF_ENABLE_MEM_POOL			1
#define XIO_OPTVAL_DEF_ENABLE_MR_CHECK			0
#define XIO_OPTVAL_DEF_UCX_ENABLE_DMA_LATENCY		0
#define XIO_OPTVAL_DEF_UCX_MAX_IN_IOVSZ			XIO_IOVLEN
#define XIO_OPTVAL_DEF_UCX_MAX_OUT_IOVSZ		XIO_IOVLEN
#define XIO_OPTVAL_DEF_UCX_NO_DELAY			0
#define XIO_OPTVAL_DEF_UCX_SO_SNDBUF			4194304
#define XIO_OPTVAL_DEF_UCX_SO_RCVBUF			4194304
#define XIO_OPTVAL_DEF_UCX_DUAL_SOCK			1

/*---------------------------------------------------------------------------*/
/* globals								     */
/*---------------------------------------------------------------------------*/
static spinlock_t			mngmt_lock;
static thread_once_t			ctor_key_once = THREAD_ONCE_INIT;
static thread_once_t			dtor_key_once = THREAD_ONCE_INIT;
static struct xio_ucx_socket_ops	ucp;
extern struct xio_transport		xio_ucx_transport;
static int				cdl_fd = -1;

static ucp_context_h ucp_context = NULL;

/* ucx options */
struct xio_ucx_options			ucx_options = {
	XIO_OPTVAL_DEF_ENABLE_MEM_POOL,		/*enable_mem_pool*/
	XIO_OPTVAL_DEF_UCX_ENABLE_DMA_LATENCY,	/*enable_dma_latency*/
	XIO_OPTVAL_DEF_ENABLE_MR_CHECK,		/*enable_mr_check*/
	XIO_OPTVAL_DEF_UCX_MAX_IN_IOVSZ,	/*max_in_iovsz*/
	XIO_OPTVAL_DEF_UCX_MAX_OUT_IOVSZ,	/*max_out_iovsz*/
	XIO_OPTVAL_DEF_UCX_NO_DELAY,		/*ucx_no_delay*/
	XIO_OPTVAL_DEF_UCX_SO_SNDBUF,		/*ucx_so_sndbuf*/
	XIO_OPTVAL_DEF_UCX_SO_RCVBUF,		/*ucx_so_rcvbuf*/
	XIO_OPTVAL_DEF_UCX_DUAL_SOCK,		/*ucx_dual_sock*/
	0					/*pad*/
};

/*---------------------------------------------------------------------------*/
/* xio_ucx_get_max_header_size						     */
/*---------------------------------------------------------------------------*/
int xio_ucx_get_max_header_size(void)
{
	int req_hdr = XIO_TRANSPORT_OFFSET + sizeof(struct xio_ucx_req_hdr);
	int rsp_hdr = XIO_TRANSPORT_OFFSET + sizeof(struct xio_ucx_rsp_hdr);
	int iovsz = ucx_options.max_out_iovsz + ucx_options.max_in_iovsz;

	req_hdr += iovsz * sizeof(struct xio_sge);
	rsp_hdr += ucx_options.max_out_iovsz * sizeof(struct xio_sge);

	return max(req_hdr, rsp_hdr);
}

/*---------------------------------------------------------------------------*/
/* xio_ucx_get_inline_buffer_size					     */
/*---------------------------------------------------------------------------*/
int xio_ucx_get_inline_buffer_size(void)
{
	int inline_buf_sz = ALIGN(xio_ucx_get_max_header_size() +
				  g_options.max_inline_xio_hdr +
				  g_options.max_inline_xio_data, 1024);
	return inline_buf_sz;
}

/*---------------------------------------------------------------------------*/
/* xio_ucx_flush_all_tasks						     */
/*---------------------------------------------------------------------------*/
static int xio_ucx_flush_all_tasks(struct xio_ucx_transport *ucx_hndl)
{
	if (!list_empty(&ucx_hndl->in_flight_list)) {
		TRACE_LOG("in_flight_list not empty!\n");
		xio_transport_flush_task_list(&ucx_hndl->in_flight_list);
		/* for task that attached to senders with ref count = 2 */
		xio_transport_flush_task_list(&ucx_hndl->in_flight_list);
	}

	if (!list_empty(&ucx_hndl->tx_comp_list)) {
		TRACE_LOG("tx_comp_list not empty!\n");
		xio_transport_flush_task_list(&ucx_hndl->tx_comp_list);
	}
	if (!list_empty(&ucx_hndl->io_list)) {
		TRACE_LOG("io_list not empty!\n");
		xio_transport_flush_task_list(&ucx_hndl->io_list);
	}

	if (!list_empty(&ucx_hndl->tx_ready_list)) {
		TRACE_LOG("tx_ready_list not empty!\n");
		xio_transport_flush_task_list(&ucx_hndl->tx_ready_list);
		/* for task that attached to senders with ref count = 2 */
		xio_transport_flush_task_list(&ucx_hndl->tx_ready_list);
	}

	if (!list_empty(&ucx_hndl->rx_list)) {
		TRACE_LOG("rx_list not empty!\n");
		xio_transport_flush_task_list(&ucx_hndl->rx_list);
	}

	ucx_hndl->tx_ready_tasks_num = 0;

	return 0;
}

/*---------------------------------------------------------------------------*/
/* on_sock_close							     */
/*---------------------------------------------------------------------------*/
static void on_sock_close(struct xio_ucx_transport *ucx_hndl)
{
	TRACE_LOG("on_sock_close ucx_hndl:%p, state:%d\n\n",
		  ucx_hndl, ucx_hndl->state);

	xio_ucx_flush_all_tasks(ucx_hndl);

	xio_transport_notify_observer(&ucx_hndl->base,
				      XIO_TRANSPORT_EVENT_CLOSED,
				      NULL);

	ucx_hndl->state = XIO_TRANSPORT_STATE_DESTROYED;
}

/*---------------------------------------------------------------------------*/
/* xio_ucx_single_sock_del_ev_handlers		                             */
/*---------------------------------------------------------------------------*/
int xio_ucx_single_sock_del_ev_handlers(struct xio_ucx_transport *ucx_hndl)
{
	int retval;

        if (ucx_hndl->in_epoll[0])
                return 0;

	/* remove from epoll */
	retval = xio_context_del_ev_handler(ucx_hndl->base.ctx,
					    ucx_hndl->tcp_sock.cfd);

	if (retval) {
		ERROR_LOG("ucx_hndl:%p fd=%d del_ev_handler failed, %m\n",
			  ucx_hndl, ucx_hndl->tcp_sock.cfd);
	}

	return retval;
}

/*---------------------------------------------------------------------------*/
/* on_sock_disconnected							     */
/*---------------------------------------------------------------------------*/
static void on_sock_disconnected(struct xio_ucx_transport *ucx_hndl,
			  int passive_close)
{
	struct xio_ucx_pending_conn *pconn, *next_pconn;
	int retval;

	TRACE_LOG("on_sock_disconnected. ucx_hndl:%p, state:%d\n",
		  ucx_hndl, ucx_hndl->state);
	if (ucx_hndl->state == XIO_TRANSPORT_STATE_DISCONNECTED) {
		TRACE_LOG("call to close. ucx_hndl:%p\n",
			  ucx_hndl);
		ucx_hndl->state = XIO_TRANSPORT_STATE_CLOSED;

		xio_context_disable_event(&ucx_hndl->flush_tx_event);
		xio_context_disable_event(&ucx_hndl->ctl_rx_event);

		if (ucx_hndl->tcp_sock.ops.del_ev_handlers)
			ucx_hndl->tcp_sock.ops.del_ev_handlers(ucx_hndl);

		if (!passive_close && !ucx_hndl->is_listen) { /*active close*/
			ucx_hndl->tcp_sock.ops.shutdown(&ucx_hndl->tcp_sock);
		}
		ucx_hndl->tcp_sock.ops.close(&ucx_hndl->tcp_sock);

		list_for_each_entry_safe(pconn, next_pconn,
					 &ucx_hndl->pending_conns,
					 conns_list_entry) {
			retval = xio_context_del_ev_handler(ucx_hndl->base.ctx,
							    pconn->fd);
			if (retval) {
				ERROR_LOG(
				"removing conn handler failed.(errno=%d %m)\n",
				xio_get_last_socket_error());
			}
			list_del(&pconn->conns_list_entry);
			ufree(pconn);
		}

		if (passive_close) {
			xio_transport_notify_observer(
					&ucx_hndl->base,
					XIO_TRANSPORT_EVENT_DISCONNECTED,
					NULL);
		}
	}
}

/*---------------------------------------------------------------------------*/
/* xio_ucx_post_close							     */
/*---------------------------------------------------------------------------*/
static void xio_ucx_post_close(struct xio_ucx_transport *ucx_hndl)
{
	TRACE_LOG("ucx transport: [post close] handle:%p\n",
		  ucx_hndl);

	xio_context_disable_event(&ucx_hndl->disconnect_event);

	xio_observable_unreg_all_observers(&ucx_hndl->base.observable);

	if (ucx_hndl->tmp_rx_buf) {
		ufree(ucx_hndl->tmp_rx_buf);
		ucx_hndl->tmp_rx_buf = NULL;
	}

	ufree(ucx_hndl->base.portal_uri);

	XIO_OBSERVABLE_DESTROY(&ucx_hndl->base.observable);

	ufree(ucx_hndl);
}

/*---------------------------------------------------------------------------*/
/* xio_ucx_close_cb		                                             */
/*---------------------------------------------------------------------------*/
static void xio_ucx_close_cb(struct kref *kref)
{
	struct xio_transport_base *transport = container_of(
					kref, struct xio_transport_base, kref);
	struct xio_ucx_transport *ucx_hndl =
		(struct xio_ucx_transport *)transport;

	/* now it is zero */
	TRACE_LOG("xio_ucx_close: [close] handle:%p, fd:%d\n",
		  ucx_hndl, ucx_hndl->tcp_sock.cfd);

	switch (ucx_hndl->state) {
	case XIO_TRANSPORT_STATE_LISTEN:
	case XIO_TRANSPORT_STATE_CONNECTED:
		ucx_hndl->state = XIO_TRANSPORT_STATE_DISCONNECTED;
		/*fallthrough*/
	case XIO_TRANSPORT_STATE_DISCONNECTED:
		on_sock_disconnected(ucx_hndl, 0);
		/*fallthrough*/
	case XIO_TRANSPORT_STATE_CLOSED:
		on_sock_close(ucx_hndl);
		break;
	default:
		xio_transport_notify_observer(
				&ucx_hndl->base,
				XIO_TRANSPORT_EVENT_CLOSED,
				NULL);
		ucx_hndl->state = XIO_TRANSPORT_STATE_DESTROYED;
		break;
	}

	if (ucx_hndl->state  == XIO_TRANSPORT_STATE_DESTROYED)
		xio_ucx_post_close(ucx_hndl);
}

/*---------------------------------------------------------------------------*/
/* xio_ucx_close		                                             */
/*---------------------------------------------------------------------------*/
static void xio_ucx_close(struct xio_transport_base *transport)
{
	int was = atomic_read(&transport->kref.refcount);

	/* this is only for debugging - please note that the combination of
	 * atomic_read and kref_put is not atomic - please remove if this
	 * error does not pop up. Otherwise contact me and report bug.
	 */

	/* was already 0 */
	if (!was) {
		ERROR_LOG("xio_ucx_close double close. handle:%p\n",
			  transport);
		return;
	}

	kref_put(&transport->kref, xio_ucx_close_cb);
}

/*---------------------------------------------------------------------------*/
/* xio_ucx_single_sock_shutdown		                                     */
/*---------------------------------------------------------------------------*/
int xio_ucx_single_sock_shutdown(struct xio_ucx_tcp_socket *sock)
{
	int retval;

	retval = shutdown(sock->cfd, SHUT_RDWR);
	if (retval) {
		xio_set_error(xio_get_last_socket_error());
		DEBUG_LOG("ucx shutdown failed. (errno=%d %m)\n",
			  xio_get_last_socket_error());
	}

	return retval;
}

/*---------------------------------------------------------------------------*/
/* xio_ucx_single_sock_close		                                     */
/*---------------------------------------------------------------------------*/
int xio_ucx_single_sock_close(struct xio_ucx_tcp_socket *sock)
{
	int retval;

	retval = xio_closesocket(sock->cfd);
	if (retval) {
		xio_set_error(xio_get_last_socket_error());
		DEBUG_LOG("ucx close failed. (errno=%d %m)\n",
			  xio_get_last_socket_error());
	}

	return retval;
}


/*---------------------------------------------------------------------------*/
/* xio_ucx_reject		                                             */
/*---------------------------------------------------------------------------*/
static int xio_ucx_reject(struct xio_transport_base *transport)
{
	struct xio_ucx_transport *ucx_hndl =
		(struct xio_ucx_transport *)transport;
	int				retval;

	ucx_hndl->tcp_sock.ops.shutdown(&ucx_hndl->tcp_sock);

	retval = ucx_hndl->tcp_sock.ops.close(&ucx_hndl->tcp_sock);
	if (retval)
		return -1;

	TRACE_LOG("ucx transport: [reject] handle:%p\n", ucx_hndl);

	return 0;
}

/*---------------------------------------------------------------------------*/
/* xio_ucx_context_shutdown						     */
/*---------------------------------------------------------------------------*/
static int xio_ucx_context_shutdown(struct xio_transport_base *trans_hndl,
				    struct xio_context *ctx)
{
	struct xio_ucx_transport *ucx_hndl =
			(struct xio_ucx_transport *)trans_hndl;

	TRACE_LOG("ucx transport context_shutdown handle:%p\n", ucx_hndl);

	switch (ucx_hndl->state) {
	case XIO_TRANSPORT_STATE_INIT:
		DEBUG_LOG("shutting context while ucx_hndl=%p state is INIT?\n",
			  ucx_hndl);
		/*fallthrough*/
	case XIO_TRANSPORT_STATE_LISTEN:
	case XIO_TRANSPORT_STATE_CONNECTING:
	case XIO_TRANSPORT_STATE_CONNECTED:
		ucx_hndl->state = XIO_TRANSPORT_STATE_DISCONNECTED;
		/*fallthrough*/
	case XIO_TRANSPORT_STATE_DISCONNECTED:
		on_sock_disconnected(ucx_hndl, 0);
		break;
	default:
		break;
	}

	ucx_hndl->state = XIO_TRANSPORT_STATE_DESTROYED;
	xio_ucx_flush_all_tasks(ucx_hndl);

	xio_transport_notify_observer(&ucx_hndl->base,
				      XIO_TRANSPORT_EVENT_CLOSED,
				      NULL);

	xio_ucx_post_close(ucx_hndl);

	return 0;
}

/*---------------------------------------------------------------------------*/
/* xio_ucx_disconnect_handler						     */
/*---------------------------------------------------------------------------*/
static void xio_ucx_disconnect_handler(void *xio_ucx_hndl)
{
	struct xio_ucx_transport *ucx_hndl = (struct xio_ucx_transport *)
						xio_ucx_hndl;
	on_sock_disconnected(ucx_hndl, 1);
}

/*---------------------------------------------------------------------------*/
/* xio_ucx_flush_tx_handler						     */
/*---------------------------------------------------------------------------*/
void xio_ucx_flush_tx_handler(void *xio_ucx_hndl)
{
	struct xio_ucx_transport *ucx_hndl = (struct xio_ucx_transport *)
						xio_ucx_hndl;
	xio_ucx_xmit(ucx_hndl);
}

/*---------------------------------------------------------------------------*/
/* xio_ucx_single_sock_rx_ctl_handler					     */
/*---------------------------------------------------------------------------*/
int xio_ucx_single_sock_rx_ctl_handler(struct xio_ucx_transport *ucx_hndl)
{
	return xio_ucx_rx_ctl_handler(ucx_hndl, 1);
}

/*---------------------------------------------------------------------------*/
/* xio_ucx_consume_ctl_rx						     */
/*---------------------------------------------------------------------------*/
void xio_ucx_consume_ctl_rx(void *xio_ucx_hndl)
{
	struct xio_ucx_transport *ucx_hndl = (struct xio_ucx_transport *)
						xio_ucx_hndl;
	int retval = 0, count = 0;

	xio_context_disable_event(&ucx_hndl->ctl_rx_event);

	do {
		retval = ucx_hndl->tcp_sock.ops.rx_ctl_handler(ucx_hndl);
		++count;
	} while (retval > 0 && count <  RX_POLL_NR_MAX);

	if (/*retval > 0 && */ ucx_hndl->tmp_rx_buf_len &&
	    ucx_hndl->state == XIO_TRANSPORT_STATE_CONNECTED) {
		xio_context_add_event(ucx_hndl->base.ctx,
				      &ucx_hndl->ctl_rx_event);
	}
}

/**
 * this function listens on the ucx worker fd and invoked from epoll
 * @param fd the fd that invoked the event
 * @param events type of event
 * @param user_context the transport handler
 * @note the transport handler is not the one that will send the response
 */
void xio_ucx_handler(int fd, int events, void *user_context)
{
	struct xio_ucx_transport	*ucx_hndl = (struct xio_ucx_transport *)
							user_context;
	if (ucx_hndl->state ==  XIO_TRANSPORT_STATE_CONNECTING) {
		xio_ucx_get_ucp_server_adrs(fd, events, user_context);
		return;
	}
	if (events & XIO_POLLIN)
		xio_ucx_consume_ctl_rx(ucx_hndl);

	if (events & (XIO_POLLHUP | XIO_POLLRDHUP | XIO_POLLERR)) {
		DEBUG_LOG("epoll returned with error events=%d for fd=%d\n",
			  events, fd);
		xio_ucx_disconnect_helper(ucx_hndl);
	}

	/* ORK todo add work instead of poll_nr? */
}

/*---------------------------------------------------------------------------*/
/* xio_ucx_data_ready_ev_handler					     */
/*---------------------------------------------------------------------------*/
void xio_ucx_data_ready_ev_handler(int fd, int events, void *user_context)
{
	struct xio_ucx_transport	*ucx_hndl = (struct xio_ucx_transport *)
							user_context;
	int retval = 0, count = 0;

	if (events & XIO_POLLOUT) {
		xio_context_modify_ev_handler(ucx_hndl->base.ctx, fd,
					      XIO_POLLIN | XIO_POLLRDHUP);
		xio_ucx_xmit(ucx_hndl);
	}

	if (events & XIO_POLLIN) {
		do {
			retval = ucx_hndl->tcp_sock.ops.rx_data_handler(
							ucx_hndl, RX_BATCH);
			++count;
		} while (retval > 0 && count <  RX_POLL_NR_MAX);
	}

	if (events & (XIO_POLLHUP | XIO_POLLRDHUP | XIO_POLLERR)) {
		DEBUG_LOG("epoll returned with error events=%d for fd=%d\n",
			  events, fd);
		xio_ucx_disconnect_helper(ucx_hndl);
	}
}

/*---------------------------------------------------------------------------*/
/* xio_ucx_single_sock_add_ev_handlers		                             */
/*---------------------------------------------------------------------------*/
int xio_ucx_single_sock_add_ev_handlers(struct xio_ucx_transport *ucx_hndl)
{
	/* add to epoll */
	struct xio_ucp_worker *worker = (struct xio_ucp_worker*)
			ucx_hndl->base.ctx->trans_data;
	int retval = xio_context_add_ev_handler(
			ucx_hndl->base.ctx,
			worker->fd,
			XIO_POLLIN | XIO_POLLRDHUP,
			xio_ucx_handler,
			ucx_hndl);

	if (retval) {
		ERROR_LOG("setting connection handler failed. (errno=%d %m)\n",
			  xio_get_last_socket_error());
	}
        ucx_hndl->in_epoll[0] = 1;

	return retval;
}

/*---------------------------------------------------------------------------*/
/* xio_ucx_accept		                                             */
/*---------------------------------------------------------------------------*/
static int xio_ucx_accept(struct xio_transport_base *transport)
{
	struct xio_ucx_transport *ucx_hndl =
			(struct xio_ucx_transport *)transport;

	if (ucx_hndl->tcp_sock.ops.add_ev_handlers(ucx_hndl)) {
		xio_transport_notify_observer_error(&ucx_hndl->base,
						    XIO_E_UNSUCCESSFUL);
	}

	TRACE_LOG("ucx transport: [accept] handle:%p\n", ucx_hndl);

	xio_transport_notify_observer(
			&ucx_hndl->base,
			XIO_TRANSPORT_EVENT_ESTABLISHED,
			NULL);

	return 0;
}

/*---------------------------------------------------------------------------*/
/* xio_ucx_socket_create		                                     */
/*---------------------------------------------------------------------------*/
int xio_ucx_socket_create(void)
{
	int sock_fd, retval, optval = 1;

	sock_fd = xio_socket_non_blocking(AF_INET, SOCK_STREAM, 0);
	if (sock_fd < 0) {
		xio_set_error(xio_get_last_socket_error());
		ERROR_LOG("create socket failed. (errno=%d %m)\n",
			  xio_get_last_socket_error());
		return sock_fd;
	}

	retval = setsockopt(sock_fd,
			    SOL_SOCKET,
			    SO_REUSEADDR,
			    (char *)&optval,
			    sizeof(optval));
	if (retval) {
		xio_set_error(xio_get_last_socket_error());
		ERROR_LOG("setsockopt failed. (errno=%d %m)\n",
			  xio_get_last_socket_error());
		goto cleanup;
	}

	if (ucx_options.ucx_no_delay) {
		retval = setsockopt(sock_fd,
				    IPPROTO_TCP,
				    TCP_NODELAY,
				    (char *)&optval,
				    sizeof(int));
		if (retval) {
			xio_set_error(xio_get_last_socket_error());
			ERROR_LOG("setsockopt failed. (errno=%d %m)\n",
				  xio_get_last_socket_error());
			goto cleanup;
		}
	}

	optval = ucx_options.ucx_so_sndbuf;
	retval = setsockopt(sock_fd, SOL_SOCKET, SO_SNDBUF,
			    (char *)&optval, sizeof(optval));
	if (retval) {
		xio_set_error(xio_get_last_socket_error());
		ERROR_LOG("setsockopt failed. (errno=%d %m)\n",
			  xio_get_last_socket_error());
		goto cleanup;
	}
	optval = ucx_options.ucx_so_rcvbuf;
	retval = setsockopt(sock_fd, SOL_SOCKET, SO_RCVBUF,
			    (char *)&optval, sizeof(optval));
	if (retval) {
		xio_set_error(xio_get_last_socket_error());
		ERROR_LOG("setsockopt failed. (errno=%d %m)\n",
			  xio_get_last_socket_error());
		goto cleanup;
	}

	return sock_fd;

cleanup:
	xio_closesocket(sock_fd);
	return -1;
}

/*---------------------------------------------------------------------------*/
/* xio_ucx_single_sock_create		                                     */
/*---------------------------------------------------------------------------*/
int xio_ucx_single_sock_create(struct xio_ucx_tcp_socket *sock)
{
	sock->cfd = xio_ucx_socket_create();
	if (sock->cfd < 0)
		return -1;

	return 0;
}


/*---------------------------------------------------------------------------*/
/* xio_ucx_transport_create		                                     */
/*---------------------------------------------------------------------------*/
struct xio_ucx_transport *xio_ucx_tcp_create(
		struct xio_transport	*transport,
		struct xio_context	*ctx,
		struct xio_observer	*observer,
		int			create_tcp_socket)
{
	struct xio_ucx_transport	*ucx_hndl;

	/*allocate ucx handl */
	ucx_hndl = (struct xio_ucx_transport *)
			ucalloc(1, sizeof(struct xio_ucx_transport));
	if (!ucx_hndl) {
		xio_set_error(ENOMEM);
		ERROR_LOG("ucalloc failed. %m\n");
		return NULL;
	}

	XIO_OBSERVABLE_INIT(&ucx_hndl->base.observable, ucx_hndl);

	if (ucx_options.enable_mem_pool) {
		ucx_hndl->ucx_mempool =
			xio_transport_mempool_get(ctx, 0);
		if (!ucx_hndl->ucx_mempool) {
			xio_set_error(ENOMEM);
			ERROR_LOG("allocating ucx mempool failed. %m\n");
			goto cleanup;
		}
	}
	ucx_hndl->ucp_ep		= NULL;
	ucx_hndl->base.portal_uri	= NULL;
	ucx_hndl->base.proto		= XIO_PROTO_UCX;
	kref_init(&ucx_hndl->base.kref);
	ucx_hndl->transport		= transport;
	ucx_hndl->base.ctx		= ctx;
	ucx_hndl->is_listen		= 0;

	ucx_hndl->tmp_rx_buf		= NULL;
	ucx_hndl->tmp_rx_buf_cur	= NULL;
	ucx_hndl->tmp_rx_buf_len	= 0;

	ucx_hndl->tx_ready_tasks_num = 0;
	ucx_hndl->tx_comp_cnt = 0;

	memset(&ucx_hndl->tmp_work, 0, sizeof(struct xio_ucx_work_req));
	ucx_hndl->tmp_work.msg_iov = ucx_hndl->tmp_iovec;

	/* create ucx socket */
	if (create_tcp_socket) {
		memcpy(&ucx_hndl->tcp_sock.ops, &ucp,
				sizeof(ucx_hndl->tcp_sock.ops));
		if (ucx_hndl->tcp_sock.ops.open(&ucx_hndl->tcp_sock))
			goto cleanup;
	}
	/* from now on don't allow changes */
	ucx_hndl->max_inline_buf_sz	= xio_ucx_get_inline_buffer_size();
	ucx_hndl->membuf_sz		= ucx_hndl->max_inline_buf_sz;

	if (observer)
		xio_observable_reg_observer(&ucx_hndl->base.observable,
					    observer);

	INIT_LIST_HEAD(&ucx_hndl->in_flight_list);
	INIT_LIST_HEAD(&ucx_hndl->tx_ready_list);
	INIT_LIST_HEAD(&ucx_hndl->tx_comp_list);
	INIT_LIST_HEAD(&ucx_hndl->rx_list);
	INIT_LIST_HEAD(&ucx_hndl->io_list);

	INIT_LIST_HEAD(&ucx_hndl->pending_conns);

	memset(&ucx_hndl->flush_tx_event, 0, sizeof(struct xio_ev_data));
	ucx_hndl->flush_tx_event.handler	= xio_ucx_flush_tx_handler;
	ucx_hndl->flush_tx_event.data		= ucx_hndl;

	memset(&ucx_hndl->ctl_rx_event, 0, sizeof(struct xio_ev_data));
	ucx_hndl->ctl_rx_event.handler		= xio_ucx_consume_ctl_rx;
	ucx_hndl->ctl_rx_event.data		= ucx_hndl;

	memset(&ucx_hndl->disconnect_event, 0, sizeof(struct xio_ev_data));
	ucx_hndl->disconnect_event.handler	= xio_ucx_disconnect_handler;
	ucx_hndl->disconnect_event.data		= ucx_hndl;

	TRACE_LOG("xio_ucx_open: [new] handle:%p\n", ucx_hndl);

	return ucx_hndl;

cleanup:
	ufree(ucx_hndl);

	return NULL;
}

/**
 * a function that handles a pending connection in the server
 * @param fd the fd to read the connection data from
 * @param ucx_hndl the transport handler
 * @param error errors from the epoll
 */
void xio_ucx_handle_pending_conn(int fd,
				 struct xio_ucx_transport *ucx_hndl,
				 int error)
{
	int retval;
	struct xio_ucx_pending_conn *pconn, *next_pconn;
	struct xio_ucx_pending_conn *pending_conn = NULL, *ctl_conn = NULL;
	void *buf;
	struct xio_ucp_worker *worker =
			(struct xio_ucp_worker*)ucx_hndl->base.ctx->trans_data;
	ucs_status_t status;
	ucs_status_ptr_t h_status;
	ucp_address_t *ucp_addr;
	struct xio_ucp_callback_data *handle;
	struct xio_ucx_connect_msg addr;
	union xio_transport_event_data ev_data;

	list_for_each_entry_safe(pconn, next_pconn,
			&ucx_hndl->pending_conns,
			conns_list_entry)
	{
		if (pconn->fd == fd) {
			pending_conn = pconn;
			break;
		}
	}

	if (!pending_conn) {
		ERROR_LOG("could not find pending fd [%d] on the list\n", fd);
		goto cleanup2;
	}

	if (error) {
		DEBUG_LOG("epoll returned with error=%d for fd=%d\n", error,
				fd);
		goto cleanup1;
	}
	memset(&pending_conn->msg, 0, sizeof(pending_conn->msg));
	buf = &pending_conn->msg;
	inc_ptr(buf,
		sizeof(struct xio_ucx_connect_msg) - pending_conn->waiting_for_bytes);
	while (pending_conn->waiting_for_bytes) {
		retval = recv(fd, (char *)buf, pending_conn->waiting_for_bytes,
				0);
		if (retval > 0) {
			pending_conn->waiting_for_bytes -= retval;
			inc_ptr(buf, retval);
		} else if (retval == 0) {
			ERROR_LOG("got EOF while establishing connection\n");
			goto cleanup1;
		} else {
			if (xio_get_last_socket_error() != XIO_EAGAIN) {
				ERROR_LOG("recv return with errno=%d\n",
						xio_get_last_socket_error());
				goto cleanup1;
			}
			return;
		}
	}

	UNPACK_LVAL(&pending_conn->msg, &pending_conn->msg, length);

	ctl_conn = pending_conn;

	list_del(&ctl_conn->conns_list_entry);
	retval = xio_context_del_ev_handler(ucx_hndl->base.ctx, ctl_conn->fd);
	if (retval) {
		ERROR_LOG("removing connection handler failed.(errno=%d %m)\n",
				xio_get_last_socket_error());
	}

	ucp_addr = (ucp_address_t*)pending_conn->msg.data;
	status = ucp_ep_create(worker->worker, ucp_addr, &ucx_hndl->ucp_ep);
	if (status != UCS_OK) {
		xio_set_error(xio_get_last_socket_error());
		ERROR_LOG("ucx getsockname failed. (errno=%d %m)\n",
				xio_get_last_socket_error());
		goto cleanup2;
	}
	addr.length = worker->addr_len;
	memcpy(addr.data, worker->addr, worker->addr_len);

	/* send server worker address using ucp */
	h_status = ucp_tag_send_nb(ucx_hndl->ucp_ep, &addr, sizeof(addr),
				   XIO_CONTIG, XIO_UCP_TAG,
				   xio_ucx_general_send_cb);
	if (UCS_PTR_IS_ERR(h_status)) {
		ERROR_LOG("UCS PTR returned ERR\n");
		goto cleanup2;
	} else if (UCS_PTR_STATUS(h_status) != UCS_OK) {
		handle = (struct xio_ucp_callback_data *)h_status;
		handle->transport = ucx_hndl;
		/*RAFI : need to cancel while loop */
		while (handle->completed == 0) {
			ucp_worker_progress(worker->worker);
		}
	}
	handle->transport->state = XIO_TRANSPORT_STATE_CONNECTING;
	ev_data.new_connection.child_trans_hndl =
			(struct xio_transport_base*)handle->transport;

	xio_transport_notify_observer(
			(struct xio_transport_base *)handle->transport,
			XIO_TRANSPORT_EVENT_NEW_CONNECTION,
			&ev_data);
	return;

	cleanup1:
	list_del(&pending_conn->conns_list_entry);
	ufree(pending_conn);
	cleanup2:
	/*remove from epoll*/
	retval = xio_context_del_ev_handler(ucx_hndl->base.ctx, fd);
	if (retval) {
		ERROR_LOG("removing connection handler failed.(errno=%d %m)\n",
				xio_get_last_socket_error());
	}
}

/**
 * bridge function to handle pending connections to the server
 * @param fd
 * @param events
 * @param user_context
 */
void xio_ucx_pending_conn_ev_handler(int fd, int events, void *user_context)
{
	struct xio_ucx_transport *ucx_hndl =
			(struct xio_ucx_transport *)user_context;

	xio_ucx_handle_pending_conn(
			fd, ucx_hndl,
			events & (XIO_POLLHUP | XIO_POLLERR));
}

/**
 * this is a general function used for signaling the message was sent
 * @param user_context
 * @param status
 */
void xio_ucx_general_send_cb(void *user_context, ucs_status_t status)
{
	struct xio_ucp_callback_data *handle =
			(struct xio_ucp_callback_data*)user_context;
	if (likely((status == UCS_OK)))
		handle->completed = 1;
	else
		ERROR_LOG("Got error %d when receiving data from UCX\n", status);

}

/**
 * this is a general function used for signaling the message was received
 * @param request
 * @param status
 * @param info
 */
void xio_ucx_general_recv_cb(void *request, ucs_status_t status,
			     ucp_tag_recv_info_t *info)
{
	struct xio_ucp_callback_data *handle =
			(struct xio_ucp_callback_data*)request;
	if (likely((status == UCS_OK)))
		handle->completed = 1;
	else
		ERROR_LOG("Go error %d when reciveing data from UCX\n", status);

}
/**
 * this function handles new connection flow.
 * @param parent_hndl
 */
void xio_ucx_new_connection(struct xio_ucx_transport *ucx_hndl)
{
	int retval;
	socklen_t len = sizeof(struct sockaddr_storage);
	struct xio_ucx_pending_conn *pending_conn;

	/*allocate pending fd struct */
	pending_conn = (struct xio_ucx_pending_conn *)ucalloc(
			1, sizeof(struct xio_ucx_pending_conn));
	if (!pending_conn) {
		xio_set_error(ENOMEM);
		ERROR_LOG("ucalloc failed. %m\n");
		xio_transport_notify_observer_error(&ucx_hndl->base,
							xio_errno());
		return;
	}

	pending_conn->waiting_for_bytes = sizeof(struct xio_ucx_connect_msg);

	/* "accept" the connection */
	retval = xio_accept_non_blocking(
			ucx_hndl->tcp_sock.cfd,
			(struct sockaddr * )&pending_conn->sa.sa_stor,
			&len);
	if (retval < 0) {
		xio_set_error(xio_get_last_socket_error());
		ERROR_LOG("ucx accept failed. (errno=%d %m)\n",
				xio_get_last_socket_error());
		ufree(pending_conn);
		return;
	}
	pending_conn->fd = retval;

	list_add_tail(&pending_conn->conns_list_entry,
			&ucx_hndl->pending_conns);

	/* add to epoll */
	retval = xio_context_add_ev_handler(ucx_hndl->base.ctx,
						pending_conn->fd,
						XIO_POLLIN | XIO_POLLRDHUP,
						xio_ucx_pending_conn_ev_handler,
						ucx_hndl);
	if (retval)
		ERROR_LOG("adding pending_conn_ev_handler failed\n");
}

/**
 * called by the server when a new connection sequence starts
 * @param fd- the file descriptor with the data
 * @param events - epoll event to listen to
 * @param user_context - contectx passwd to function
 */
void xio_ucx_listener_ev_handler(int fd, int events, void *user_context)
{
	struct xio_ucx_transport *ucx_hndl =
			(struct xio_ucx_transport *)user_context;
	/* server only */
	if (events & XIO_POLLIN)
		xio_ucx_new_connection(ucx_hndl);

	if ((events & (XIO_POLLHUP | XIO_POLLERR))) {
		DEBUG_LOG("epoll returned with error events=%d for fd=%d\n",
				events, fd);
		xio_ucx_disconnect_helper(ucx_hndl);
	}
}

/**
 * server listens to incoming connections
 * @param transport - transport to listen to
 * @param portal_uri - url to listen on
 * @param src_port - server port
 * @param backlog - maximum length of pending connections
 * @return
 */
static int xio_ucx_listen(struct xio_transport_base *transport,
			  const char *portal_uri,
			  uint16_t *src_port,
			  int backlog)
{
	struct xio_ucx_transport *ucx_hndl =
			(struct xio_ucx_transport *)transport;
	struct xio_ucp_worker *worker = (struct xio_ucp_worker*)
				ucx_hndl->base.ctx->trans_data;
	union xio_sockaddr sa;
	int sa_len;
	int retval = 0;
	uint16_t sport;

	/* resolve the portal_uri */
	sa_len = xio_uri_to_ss(portal_uri, &sa.sa_stor);
	if (sa_len == -1) {
		xio_set_error(XIO_E_ADDR_ERROR);
		ERROR_LOG("address [%s] resolving failed\n", portal_uri);
		goto exit1;
	}
	ucx_hndl->base.is_client = 0;

	/* bind */
	retval = bind(ucx_hndl->tcp_sock.cfd, (struct sockaddr *)&sa.sa_stor,
			sa_len);
	if (retval) {
		xio_set_error(xio_get_last_socket_error());
		ERROR_LOG("ucx bind failed. (errno=%d %m)\n",
				xio_get_last_socket_error());
		goto exit1;
	}

	ucx_hndl->is_listen = 1;

	retval = listen(ucx_hndl->tcp_sock.cfd,
			backlog > 0 ? backlog : MAX_BACKLOG);
	if (retval) {
		xio_set_error(xio_get_last_socket_error());
		ERROR_LOG("ucx listen failed. (errno=%d %m)\n",
				xio_get_last_socket_error());
		goto exit1;
	}

	/* add tcp fd to epoll */
	retval = xio_context_add_ev_handler(ucx_hndl->base.ctx,
					    ucx_hndl->tcp_sock.cfd,
					    XIO_POLLIN,
					    xio_ucx_listener_ev_handler,
					    ucx_hndl);
	if (retval) {
		ERROR_LOG("xio_context_add_ev_handler failed.\n");
		goto exit1;
	}
	/* add ucx fd to epoll */
	ucp_worker_arm(worker->worker);
	retval = xio_context_add_ev_handler(ucx_hndl->base.ctx,
					    worker->fd,
					    XIO_POLLIN,
					    xio_ucx_handler,
					    ucx_hndl);
	ucx_hndl->in_epoll[0] = 1;

	retval = getsockname(ucx_hndl->tcp_sock.cfd, (struct sockaddr *)&sa.sa_stor,
			     (socklen_t *)&sa_len);
	if (retval) {
		xio_set_error(xio_get_last_socket_error());
		ERROR_LOG("getsockname failed. (errno=%d %m)\n",
				xio_get_last_socket_error());
		goto exit;
	}

	switch (sa.sa_stor.ss_family) {
	case AF_INET:
		sport = ntohs(sa.sa_in.sin_port);
		break;
	case AF_INET6:
		sport = ntohs(sa.sa_in6.sin6_port);
		break;
	default:
		xio_set_error(XIO_E_ADDR_ERROR);
		ERROR_LOG("invalid family type %d.\n", sa.sa_stor.ss_family);
		goto exit;
	}

	if (src_port)
		*src_port = sport;

	ucx_hndl->state = XIO_TRANSPORT_STATE_LISTEN;
	DEBUG_LOG("listen on [%s] src_port:%d\n", portal_uri, sport);

	return 0;

	exit1: ucx_hndl->tcp_sock.ops.del_ev_handlers = NULL;
	exit: return -1;
}

/**
 * this function is used by the client to send the connection message to the
 * server.
 * @param fd
 * @param ucx_hndl
 * @param msg
 * @param error
 */
void xio_ucx_conn_established_helper(int fd, struct xio_ucx_transport *ucx_hndl,
				     struct xio_ucx_connect_msg *msg,
				     int error)
{
	int retval = 0;
	int so_error = 0;
	socklen_t len = sizeof(so_error);
	struct xio_ucp_worker *worker =
			(struct xio_ucp_worker*)ucx_hndl->base.ctx->trans_data;

	retval = getsockopt(fd, SOL_SOCKET, SO_ERROR, (char *)&so_error, &len);
	if (retval) {
		ERROR_LOG("getsockopt failed. (errno=%d %m)\n",
				xio_get_last_socket_error());
		so_error = xio_get_last_socket_error();
	}
	if (so_error || error) {
		DEBUG_LOG("fd=%d connection establishment failed\n",
				ucx_hndl->tcp_sock.cfd);
		DEBUG_LOG("so_error=%d, epoll_error=%d\n", so_error, error);
		ucx_hndl->tcp_sock.ops.del_ev_handlers = NULL;
		goto cleanup;
	}

	len = sizeof(ucx_hndl->base.peer_addr);
	retval = getpeername(fd, (struct sockaddr *)&ucx_hndl->base.peer_addr,
				&len);
	if (retval) {
		xio_set_error(xio_get_last_socket_error());
		ERROR_LOG("ucx getpeername failed. (errno=%d %m)\n",
				xio_get_last_socket_error());
		so_error = xio_get_last_socket_error();
		goto cleanup;
	}
	ucx_hndl->state = XIO_TRANSPORT_STATE_CONNECTING;

	ucp_worker_arm(worker->worker);
	if (retval) {
		ERROR_LOG("setting connection handler failed. "
				"(errno=%d %m)\n",
				xio_get_last_socket_error());
		goto cleanup;
	}
	retval = xio_context_del_ev_handler(ucx_hndl->base.ctx, fd);

	if (retval)
		goto cleanup;
	retval = xio_ucx_send_connect_msg(ucx_hndl->tcp_sock.cfd, msg);
	if (retval) {
		ERROR_LOG("setting connection handler failed. (errno=%d %m)\n",
			  xio_get_last_socket_error());
		goto cleanup;
	}
	retval = ucx_hndl->tcp_sock.ops.close(&ucx_hndl->tcp_sock);
	if (retval) {
		ERROR_LOG("failed closing TCP socket. (errno=%d %m)\n",
			  xio_get_last_socket_error());
		goto cleanup;
	}
	return;

	cleanup:
	if (so_error == XIO_ECONNREFUSED)
		xio_transport_notify_observer(&ucx_hndl->base,
						XIO_TRANSPORT_EVENT_REFUSED,
						NULL);
	else
		xio_transport_notify_observer_error(
				&ucx_hndl->base,
				so_error ? so_error : XIO_E_CONNECT_ERROR);
}

/**
 * this function is used to get the ucp address from the server
 * @param fd
 * @param events
 * @param user_context
 */
void xio_ucx_get_ucp_server_adrs(int fd, int events, void *user_context)
{
	struct xio_ucx_connect_msg msg;
	struct xio_ucx_transport *ucx_hndl =
			(struct xio_ucx_transport *)user_context;
	struct xio_ucp_worker *worker =
			(struct xio_ucp_worker*)ucx_hndl->base.ctx->trans_data;
	ucp_tag_recv_info_t tag_info;
	ucp_tag_message_h tag_msg;
	ucs_status_ptr_t status;

	tag_msg = ucp_tag_probe_nb(worker->worker, XIO_UCP_TAG, XIO_TAG_MASK,
				   XIO_UCX_REMOVE, &tag_info);
	/* if there is no message rearm and return to epoll */
	if (tag_msg == NULL)
		goto back_to_epoll;
	/* we got something were not expecting */
	if (tag_info.length != sizeof(msg)) {
		ERROR_LOG("Got messages not expecting\n");
		goto back_to_epoll;
	}
	status = ucp_tag_msg_recv_nb(worker->worker, &msg, sizeof(msg),
				     XIO_CONTIG,tag_msg, xio_ucx_general_recv_cb);
	if (UCS_PTR_IS_ERR(status)) {
		ERROR_LOG("Got err while sending ucx address %d\n", status);
		goto back_to_epoll;
	} else {
		while (((struct xio_ucp_callback_data *)status)->completed == 0) {
			ucp_worker_progress(worker->worker);
		}
	}
	/* remove and set new handlers */
	ucp_ep_create(worker->worker, (ucp_address_t*)msg.data,
			&ucx_hndl->ucp_ep);

	xio_transport_notify_observer(&ucx_hndl->base,
					XIO_TRANSPORT_EVENT_ESTABLISHED,
					NULL);
	back_to_epoll:
	ucp_worker_arm(worker->worker);
	return;
}

/**
 * heper function to connect, triggered from epoll
 * @param fd fd to read from
 * @param events events from epoll
 * @param user_context - transport
 */
void xio_ucx_single_conn_established_ev_handler(int fd, int events,
						void *user_context)
{
	struct xio_ucx_transport *ucx_hndl =
			(struct xio_ucx_transport *)user_context;
	struct xio_ucp_worker *worker =
			(struct xio_ucp_worker*)ucx_hndl->base.ctx->trans_data;
	struct xio_ucx_connect_msg msg;

	memcpy(msg.data, worker->addr, worker->addr_len);
	msg.length = (uint16_t)worker->addr_len;
	xio_ucx_conn_established_helper(
			fd, ucx_hndl, &msg,
			events & (XIO_POLLERR | XIO_POLLHUP | XIO_POLLRDHUP));
}

/*---------------------------------------------------------------------------*/
/* xio_ucx_connect_helper	                                             */
/*---------------------------------------------------------------------------*/
static int xio_ucx_connect_helper(int fd, struct sockaddr *sa, socklen_t sa_len,
				  uint16_t *bound_port,
				  struct sockaddr_storage *lss)
{
	int retval;
	union xio_sockaddr *lsa = (union xio_sockaddr *)lss;
	struct sockaddr_storage sa_stor;
	socklen_t lsa_len = sizeof(struct sockaddr_storage);

	retval = connect(fd, sa, sa_len);
	if (retval) {
		if (xio_get_last_socket_error() == XIO_EINPROGRESS) {
			/*set iomux for write event*/
		} else {
			xio_set_error(xio_get_last_socket_error());
			ERROR_LOG("ucx connect failed. (errno=%d %m)\n",
					xio_get_last_socket_error());
			return retval;
		}
	} else {
		/*handle in ev_handler*/
	}

	if (!lss)
		lsa = (union xio_sockaddr *)&sa_stor;

	retval = getsockname(fd, &lsa->sa, &lsa_len);
	if (retval) {
		xio_set_error(xio_get_last_socket_error());
		ERROR_LOG("ucx getsockname failed. (errno=%d %m)\n",
				xio_get_last_socket_error());
		return retval;
	}

	if (lsa->sa.sa_family == AF_INET) {
		*bound_port = ntohs(lsa->sa_in.sin_port);
	} else if (lsa->sa.sa_family == AF_INET6) {
		*bound_port = ntohs(lsa->sa_in6.sin6_port);
	} else {
		ERROR_LOG("getsockname unknown family = %d\n",
				lsa->sa.sa_family);
		return -1;
	}

	return 0;
}

/**
 * called by the client to connect using tcp socket
 * @param ucx_hndl - transport handler
 * @param sa - socket address to use
 * @param sa_len - socket address length
 * @return
 */
int xio_ucx_single_sock_connect(struct xio_ucx_transport *ucx_hndl,
				struct sockaddr *sa,
				socklen_t sa_len)
{
	int retval;
	struct xio_ucp_worker *worker = (struct xio_ucp_worker *)
			ucx_hndl->base.ctx->trans_data;
	retval = xio_ucx_connect_helper(ucx_hndl->tcp_sock.cfd, sa, sa_len,
					&ucx_hndl->tcp_sock.port_cfd,
					&ucx_hndl->base.local_addr);
	if (retval)
		return retval;

	retval = xio_context_add_ev_handler(
			ucx_hndl->base.ctx,
			ucx_hndl->tcp_sock.cfd,
			XIO_POLLOUT | XIO_POLLRDHUP | XIO_ONESHOT,
			xio_ucx_single_conn_established_ev_handler,
			ucx_hndl);
	ucp_worker_arm(worker->worker);
	retval = xio_context_add_ev_handler(
			ucx_hndl->base.ctx,
			worker->fd,
			XIO_POLLIN | XIO_POLLRDHUP,
			xio_ucx_handler,
			ucx_hndl);
	if (retval) {
		ERROR_LOG("setting connection handler failed. (errno=%d %m)\n",
				xio_get_last_socket_error());
		return retval;
	}

	return 0;
}

/**
 * function to connect to a server
 * @param transport transport to use
 * @param portal_uri uri to conect to
 * @param out_if_addr bounded outgoing interface address and/or port
 * @return
 */
static int xio_ucx_connect(struct xio_transport_base *transport,
			   const char *portal_uri,
			   const char *out_if_addr)
{
	struct xio_ucx_transport *ucx_hndl =
			(struct xio_ucx_transport *)transport;
	union xio_sockaddr rsa;
	socklen_t rsa_len = 0;
	int retval = 0;

	/* resolve the portal_uri */
	rsa_len = xio_uri_to_ss(portal_uri, &rsa.sa_stor);
	if (rsa_len == (socklen_t)-1) {
		xio_set_error(XIO_E_ADDR_ERROR);
		ERROR_LOG("address [%s] resolving failed\n", portal_uri);
		goto exit1;
	}
	/* allocate memory for portal_uri */
	ucx_hndl->base.portal_uri = strdup(portal_uri);
	if (!ucx_hndl->base.portal_uri) {
		xio_set_error(ENOMEM);
		ERROR_LOG("strdup failed. %m\n");
		goto exit1;
	}
	ucx_hndl->base.is_client = 1;

	if (out_if_addr) {
		union xio_sockaddr if_sa;
		int sa_len;

		sa_len = xio_host_port_to_ss(out_if_addr, &if_sa.sa_stor);
		if (sa_len == -1) {
			xio_set_error(XIO_E_ADDR_ERROR);
			ERROR_LOG("outgoing interface [%s] resolving failed\n",
					out_if_addr);
			goto exit;
		}
		retval = bind(ucx_hndl->tcp_sock.cfd,
				(struct sockaddr *)&if_sa.sa_stor,
				sa_len);
		if (retval) {
			xio_set_error(xio_get_last_socket_error());
			ERROR_LOG("ucx bind failed. (errno=%d %m)\n",
					xio_get_last_socket_error());
			goto exit;
		}
	}

	/* connect */
	retval = ucx_hndl->tcp_sock.ops.connect(ucx_hndl,
						(struct sockaddr *)&rsa.sa_stor,
						rsa_len);
	if (retval)
		goto exit;

	return 0;

	exit: ufree(ucx_hndl->base.portal_uri);
	exit1: ucx_hndl->tcp_sock.ops.del_ev_handlers = NULL;
	return -1;
}

static void xio_ucx_request_init_cb(void *req)
{
	struct xio_ucp_callback_data *data = (struct xio_ucp_callback_data *)req;
	data->completed = 0;
	data->transport = NULL;
}

static int xio_ucx_transport_open(struct xio_ucx_transport *ucx_hndl)
{
	ucs_status_t status;
	struct xio_ucp_worker *worker;
	/* UCP temporary vars */
	ucp_params_t ucp_params;
	ucp_config_t *config;

	if (ucx_hndl->base.ctx->trans_data)
		return 0;
	ucx_hndl->base.ctx->trans_data = ucalloc(
			1, sizeof(struct xio_ucp_worker));
	worker = (struct xio_ucp_worker*)ucx_hndl->base.ctx->trans_data;

	/* UCP initialization */
	status = ucp_config_read(NULL, NULL, &config);
	if (status != UCS_OK) {
		ERROR_LOG("failed reading ucp config %d\n", status);
		return 1;
	}

	ucp_params.features = UCP_FEATURE_TAG | UCP_FEATURE_WAKEUP;
	ucp_params.request_size = sizeof(struct xio_ucp_callback_data);
	ucp_params.request_init = xio_ucx_request_init_cb;
	ucp_params.request_cleanup = NULL;
	status = ucp_init(&ucp_params, config, &ucp_context);

	ucp_config_print(config, stdout, NULL, UCS_CONFIG_PRINT_CONFIG);

	ucp_config_release(config);
	if (status != UCS_OK) {
		ERROR_LOG("failed reading ucp config %d\n", status);
		return 1;
	}
	status = ucp_worker_create(ucp_context, UCS_THREAD_MODE_SINGLE,
					&(worker->worker));
	if (status != UCS_OK) {
		goto err_cleanup;
	}

	status = ucp_worker_get_address(worker->worker,
					&worker->addr,
					&worker->addr_len);
	if (status != UCS_OK) {
		goto err_worker;
	}

	status = ucp_worker_get_efd(worker->worker, &worker->fd);
	if (status) {
		ERROR_LOG("failed getting ucp epoll fd %d\n",status);
		goto err_worker;
	}
	return 0;

	err_worker:
	ucp_worker_release_address(worker->worker, worker->addr);
	ucp_worker_destroy(worker->worker);
	ufree(worker);
	err_cleanup:
	ucp_cleanup(ucp_context);
	return 1;
}

/*---------------------------------------------------------------------------*/
/* xio_ucx_open								     */
/*---------------------------------------------------------------------------*/
static struct xio_transport_base *xio_ucx_open(
		struct xio_transport	*transport,
		struct xio_context	*ctx,
		struct xio_observer	*observer,
		uint32_t		trans_attr_mask,
		struct xio_transport_init_attr *attr)
{
	struct xio_ucx_transport	*ucx_hndl;
	int status;
	ucx_hndl = xio_ucx_tcp_create(transport, ctx, observer, 1);
	if (!ucx_hndl) {
		ERROR_LOG("failed. to create ucx transport%m\n");
		return NULL;
	}
	if (attr && trans_attr_mask) {
		memcpy(&ucx_hndl->trans_attr, attr, sizeof(*attr));
		ucx_hndl->trans_attr_mask = trans_attr_mask;
	}
	status = xio_ucx_transport_open(ucx_hndl);
	if (status != 0) {
		ufree(ucx_hndl);
		return NULL;
	}
	return (struct xio_transport_base *)ucx_hndl;
}

/*
 * To dynamically control C-states, open the file /dev/cpu_dma_latency and
 * write the maximum allowable latency to it. This will prevent C-states with
 * transition latencies higher than the specified value from being used, as
 * long as the file /dev/cpu_dma_latency is kept open.
 * Writing a maximum allowable latency of 0 will keep the processors in C0
 * (like using kernel parameter ―idle=poll), and writing 1 should force
 * the processors to C1 when idle. Higher values could also be written to
 * restrict the use of C-states with latency greater than the value written.
 *
 * http://en.community.dell.com/techcenter/extras/m/white_papers/20227764/download.aspx
 */

/*---------------------------------------------------------------------------*/
/* xio_set_cpu_latency							     */
/*---------------------------------------------------------------------------*/
static int xio_set_cpu_latency(int *fd)
{
	int32_t latency = 0;

	if (!ucx_options.enable_dma_latency)
		return 0;

	DEBUG_LOG("setting latency to %d us\n", latency);
	*fd = open("/dev/cpu_dma_latency", O_WRONLY);
	if (*fd < 0) {
		ERROR_LOG(
		 "open /dev/cpu_dma_latency %m - need root permissions\n");
		return -1;
	}
	if (write(*fd, &latency, sizeof(latency)) != sizeof(latency)) {
		ERROR_LOG(
		 "write to /dev/cpu_dma_latency %m - need root permissions\n");
		close(*fd);
		*fd = -1;
		return -1;
	}
	return 0;
}

/*---------------------------------------------------------------------------*/
/* xio_ucx_init							     */
/*---------------------------------------------------------------------------*/
static void xio_ucx_init(void)
{
	spin_lock_init(&mngmt_lock);

	/* set cpu latency until process is down */
	xio_set_cpu_latency(&cdl_fd);
}

/*---------------------------------------------------------------------------*/
/* xio_ucx_transport_init						     */
/*---------------------------------------------------------------------------*/
static int xio_ucx_transport_init(struct xio_transport *transport)
{
	thread_once(&ctor_key_once, xio_ucx_init);
	return 0;
}

/*---------------------------------------------------------------------------*/
/* xio_ucx_release							     */
/*---------------------------------------------------------------------------*/
static void xio_ucx_release(void)
{
	if (cdl_fd >= 0)
		xio_closesocket(cdl_fd);

	/*ORK todo close everything? see xio_cq_release*/
}

/*---------------------------------------------------------------------------*/
/* xio_ucx_release_msvc							     */
/* This function is required for MSVC compilation under Windows */
/*---------------------------------------------------------------------------*/
int CALLBACK xio_ucx_release_msvc(thread_once_t *a, void *b, void **c)
{
	xio_ucx_release();
	return 0;
}

/*---------------------------------------------------------------------------*/
/* xio_ucx_transport_constructor					     */
/*---------------------------------------------------------------------------*/
void xio_ucx_transport_constructor(void)
{
}

/*---------------------------------------------------------------------------*/
/* xio_ucx_transport_destructor					     */
/*---------------------------------------------------------------------------*/
void xio_ucx_transport_destructor(void)
{
	reset_thread_once_t(&ctor_key_once);
	reset_thread_once_t(&dtor_key_once);
}

/*---------------------------------------------------------------------------*/
/* xio_ucx_transport_release		                                     */
/*---------------------------------------------------------------------------*/
static void xio_ucx_transport_release(struct xio_transport *transport)
{
	if (is_reset_thread_once_t(&ctor_key_once))
		return;

	thread_once(&dtor_key_once, xio_ucx_release);
}

/*---------------------------------------------------------------------------*/
/* xio_ucx_rxd_init							     */
/*---------------------------------------------------------------------------*/
static void xio_ucx_rxd_init(struct xio_ucx_work_req *rxd,
			     void *buf, unsigned size)
{
	rxd->msg_iov[0].iov_base = buf;
	rxd->msg_iov[0].iov_len	= sizeof(struct xio_tlv);
	rxd->msg_iov[1].iov_base = sum_to_ptr(rxd->msg_iov[0].iov_base,
					      rxd->msg_iov[0].iov_len);
	rxd->msg_iov[1].iov_len	= size - sizeof(struct xio_tlv);
	rxd->msg_len = 2;

	rxd->tot_iov_byte_len = 0;

	rxd->stage = XIO_UCX_RX_START;
	rxd->msg.msg_control = NULL;
	rxd->msg.msg_controllen = 0;
	rxd->msg.msg_flags = 0;
	rxd->msg.msg_name = NULL;
	rxd->msg.msg_namelen = 0;
	rxd->msg.msg_iov = NULL;
	rxd->msg.msg_iovlen = 0;
}

/*---------------------------------------------------------------------------*/
/* xio_ucx_txd_init							     */
/*---------------------------------------------------------------------------*/
static void xio_ucx_txd_init(struct xio_ucx_work_req *txd,
			     void *buf, unsigned size)
{
	txd->ctl_msg = buf;
	txd->ctl_msg_len = 0;
	txd->msg_iov[0].iov_base = buf;
	txd->msg_iov[0].iov_len	= size;
	txd->msg_len = 1;
	txd->tot_iov_byte_len = 0;

	txd->stage = XIO_UCX_TX_BEFORE;
	txd->msg.msg_control = NULL;
	txd->msg.msg_controllen = 0;
	txd->msg.msg_flags = 0;
	txd->msg.msg_name = NULL;
	txd->msg.msg_namelen = 0;
	txd->msg.msg_iov = NULL;
	txd->msg.msg_iovlen = 0;
}

/*---------------------------------------------------------------------------*/
/* xio_ucx_task_init							     */
/*---------------------------------------------------------------------------*/
static void xio_ucx_task_init(struct xio_task *task,
			      struct xio_ucx_transport *ucx_hndl,
			      void *buf,
			      unsigned long size)
{
	XIO_TO_UCX_TASK(task, ucx_task);

	xio_ucx_rxd_init(&ucx_task->rxd, buf, size);
	xio_ucx_txd_init(&ucx_task->txd, buf, size);

	/* initialize the mbuf */
	xio_mbuf_init(&task->mbuf, buf, size, 0);
}

/* task pools management */
/*---------------------------------------------------------------------------*/
/* xio_ucx_initial_pool_slab_pre_create					     */
/*---------------------------------------------------------------------------*/
static int xio_ucx_initial_pool_slab_pre_create(
		struct xio_transport_base *transport_hndl,
		int alloc_nr,
		void *pool_dd_data, void *slab_dd_data)
{
	struct xio_ucx_tasks_slab *ucx_slab =
		(struct xio_ucx_tasks_slab *)slab_dd_data;
	uint32_t pool_size;

	ucx_slab->buf_size = CONN_SETUP_BUF_SIZE;
	pool_size = ucx_slab->buf_size * alloc_nr;

	ucx_slab->data_pool = ucalloc(pool_size * alloc_nr, sizeof(uint8_t));
	if (!ucx_slab->data_pool) {
		xio_set_error(ENOMEM);
		ERROR_LOG("ucalloc conn_setup_data_pool sz: %u failed\n",
			  pool_size);
		return -1;
	}

	return 0;
}

/*---------------------------------------------------------------------------*/
/* xio_ucx_initial_task_alloc						     */
/*---------------------------------------------------------------------------*/
static inline struct xio_task *xio_ucx_initial_task_alloc(
					struct xio_ucx_transport *ucx_hndl)
{
	if (ucx_hndl->initial_pool_cls.task_get) {
		struct xio_task *task = ucx_hndl->initial_pool_cls.task_get(
					ucx_hndl->initial_pool_cls.pool,
					ucx_hndl);
		return task;
	}
	return NULL;
}

/*---------------------------------------------------------------------------*/
/* xio_ucx_primary_task_alloc						     */
/*---------------------------------------------------------------------------*/
struct xio_task *xio_ucx_primary_task_alloc(
					struct xio_ucx_transport *ucx_hndl)
{
	if (ucx_hndl->primary_pool_cls.task_get) {
		struct xio_task *task = ucx_hndl->primary_pool_cls.task_get(
					ucx_hndl->primary_pool_cls.pool,
					ucx_hndl);
		return task;
	}
	return NULL;
}

/*---------------------------------------------------------------------------*/
/* xio_ucx_primary_task_lookup						     */
/*---------------------------------------------------------------------------*/
struct xio_task *xio_ucx_primary_task_lookup(
					struct xio_ucx_transport *ucx_hndl,
					int tid)
{
	if (ucx_hndl->primary_pool_cls.task_lookup)
		return ucx_hndl->primary_pool_cls.task_lookup(
					ucx_hndl->primary_pool_cls.pool, tid);
	return NULL;
}

/*---------------------------------------------------------------------------*/
/* xio_ucx_task_free							     */
/*---------------------------------------------------------------------------*/
inline void xio_ucx_task_free(struct xio_ucx_transport *ucx_hndl,
			       struct xio_task *task)
{
	if (ucx_hndl->primary_pool_cls.task_put)
		return ucx_hndl->primary_pool_cls.task_put(task);
}

/*---------------------------------------------------------------------------*/
/* xio_ucx_initial_pool_post_create					     */
/*---------------------------------------------------------------------------*/
static int xio_ucx_initial_pool_post_create(
		struct xio_transport_base *transport_hndl,
		void *pool, void *pool_dd_data)
{
	struct xio_task *task;
	struct xio_ucx_task *ucx_task;
	struct xio_ucx_transport *ucx_hndl =
		(struct xio_ucx_transport *)transport_hndl;

	if (!ucx_hndl)
		return 0;

	ucx_hndl->initial_pool_cls.pool = pool;

	task = xio_ucx_initial_task_alloc(ucx_hndl);
	if (!task) {
		ERROR_LOG("failed to get task\n");
	} else {
		list_add_tail(&task->tasks_list_entry, &ucx_hndl->rx_list);
		ucx_task = (struct xio_ucx_task *)task->dd_data;
		ucx_task->out_ucx_op = XIO_UCX_RECV;
	}

	return 0;
}

/*---------------------------------------------------------------------------*/
/* xio_ucx_initial_pool_slab_destroy					     */
/*---------------------------------------------------------------------------*/
static int xio_ucx_initial_pool_slab_destroy(
		struct xio_transport_base *transport_hndl,
		void *pool_dd_data, void *slab_dd_data)
{
	struct xio_ucx_tasks_slab *ucx_slab =
		(struct xio_ucx_tasks_slab *)slab_dd_data;

	ufree(ucx_slab->data_pool);

	return 0;
}

/*---------------------------------------------------------------------------*/
/* xio_ucx_initial_pool_slab_init_task					     */
/*---------------------------------------------------------------------------*/
static int xio_ucx_initial_pool_slab_init_task(
		struct xio_transport_base *transport_hndl,
		void *pool_dd_data, void *slab_dd_data,
		int tid, struct xio_task *task)
{
	struct xio_ucx_transport *ucx_hndl =
		(struct xio_ucx_transport *)transport_hndl;
	struct xio_ucx_tasks_slab *ucx_slab =
		(struct xio_ucx_tasks_slab *)slab_dd_data;
	void *buf = sum_to_ptr(ucx_slab->data_pool,
			       tid * ALIGN(ucx_slab->buf_size, PAGE_SIZE));
	char *ptr;

	XIO_TO_UCX_TASK(task, ucx_task);

	/* fill xio_ucx_task */
	ptr = (char *)ucx_task;
	ptr += sizeof(struct xio_ucx_task);

	/* fill xio_ucx_work_req */
	ucx_task->txd.msg_iov = (struct iovec *)ptr;
	ptr += sizeof(struct iovec);

	ucx_task->rxd.msg_iov = (struct iovec *)ptr;
	ptr += 2 * sizeof(struct iovec);
	/*****************************************/

	xio_ucx_task_init(
			task,
			ucx_hndl,
			buf,
			ucx_slab->buf_size);

	return 0;
}

/*---------------------------------------------------------------------------*/
/* xio_ucx_initial_pool_get_params					     */
/*---------------------------------------------------------------------------*/
static void xio_ucx_initial_pool_get_params(
		struct xio_transport_base *transport_hndl,
		int *start_nr, int *max_nr, int *alloc_nr,
		int *pool_dd_sz, int *slab_dd_sz, int *task_dd_sz)
{

	*start_nr = 10 * NUM_CONN_SETUP_TASKS;
	*alloc_nr = 10 * NUM_CONN_SETUP_TASKS;
	*max_nr = 10 * NUM_CONN_SETUP_TASKS;

	*pool_dd_sz = 0;
	*slab_dd_sz = sizeof(struct xio_ucx_tasks_slab);
	*task_dd_sz = sizeof(struct xio_ucx_task) + 3 * sizeof(struct iovec);
}

/*---------------------------------------------------------------------------*/
/* xio_ucx_task_pre_put							     */
/*---------------------------------------------------------------------------*/
static int xio_ucx_task_pre_put(
		struct xio_transport_base *trans_hndl,
		struct xio_task *task)
{
	XIO_TO_UCX_TASK(task, ucx_task);
	XIO_TO_UCX_HNDL(task, ucx_hndl);
	unsigned int	i;

	/* recycle UCX  buffers back to pool */

	/* put buffers back to pool */

	for (i = 0; i < ucx_task->read_num_reg_mem; i++) {
		if (ucx_task->read_reg_mem[i].priv) {
			xio_mempool_free(&ucx_task->read_reg_mem[i]);
			ucx_task->read_reg_mem[i].priv = NULL;
		}
	}
	ucx_task->read_num_reg_mem = 0;

	for (i = 0; i < ucx_task->write_num_reg_mem; i++) {
		if (ucx_task->write_reg_mem[i].priv) {
			xio_mempool_free(&ucx_task->write_reg_mem[i]);
			ucx_task->write_reg_mem[i].priv = NULL;
		}
	}
	ucx_task->write_num_reg_mem	= 0;
	ucx_task->req_in_num_sge	= 0;
	ucx_task->req_out_num_sge	= 0;
	ucx_task->rsp_out_num_sge	= 0;
	ucx_task->sn			= 0;

	ucx_task->out_ucx_op		= XIO_UCX_NULL;

	xio_ucx_rxd_init(&ucx_task->rxd,
			 task->mbuf.buf.head,
			 task->mbuf.buf.buflen);
	xio_ucx_txd_init(&ucx_task->txd,
			 task->mbuf.buf.head,
			 task->mbuf.buf.buflen);

	xio_ctx_del_work(ucx_hndl->base.ctx, &ucx_task->comp_work);

	return 0;
}

static struct xio_tasks_pool_ops initial_tasks_pool_ops;
/*---------------------------------------------------------------------------*/
static void init_initial_tasks_pool_ops(void)
{
	initial_tasks_pool_ops.pool_get_params =
		xio_ucx_initial_pool_get_params;
	initial_tasks_pool_ops.slab_pre_create =
		xio_ucx_initial_pool_slab_pre_create;
	initial_tasks_pool_ops.slab_destroy =
		xio_ucx_initial_pool_slab_destroy;
	initial_tasks_pool_ops.slab_init_task =
		xio_ucx_initial_pool_slab_init_task;
	initial_tasks_pool_ops.pool_post_create =
		xio_ucx_initial_pool_post_create;
	initial_tasks_pool_ops.task_pre_put =
		xio_ucx_task_pre_put;
};

/*---------------------------------------------------------------------------*/
/* xio_ucx_primary_pool_slab_pre_create					     */
/*---------------------------------------------------------------------------*/
static int xio_ucx_primary_pool_slab_pre_create(
		struct xio_transport_base *transport_hndl,
		int alloc_nr, void *pool_dd_data, void *slab_dd_data)
{
	struct xio_ucx_tasks_slab *ucx_slab =
		(struct xio_ucx_tasks_slab *)slab_dd_data;
	size_t inline_buf_sz = xio_ucx_get_inline_buffer_size();
	size_t	alloc_sz = alloc_nr * ALIGN(inline_buf_sz, PAGE_SIZE);
	int	retval;

	ucx_slab->buf_size = inline_buf_sz;

	if (disable_huge_pages) {
		retval = xio_mem_alloc(alloc_sz, &ucx_slab->reg_mem);
		if (retval) {
			xio_set_error(ENOMEM);
			ERROR_LOG("xio_alloc ucx pool sz:%zu failed\n",
				  alloc_sz);
			return -1;
		}
		ucx_slab->data_pool = ucx_slab->reg_mem.addr;
	} else {
		/* maybe allocation of with unuma_alloc can provide better
		 * performance?
		 */
		ucx_slab->data_pool = umalloc_huge_pages(alloc_sz);
		if (!ucx_slab->data_pool) {
			xio_set_error(ENOMEM);
			ERROR_LOG("malloc ucx pool sz:%zu failed\n",
				  alloc_sz);
			return -1;
		}
	}

	DEBUG_LOG("pool buf:%p\n", ucx_slab->data_pool);

	return 0;
}

/*---------------------------------------------------------------------------*/
/* xio_ucx_primary_pool_post_create					     */
/*---------------------------------------------------------------------------*/
static int xio_ucx_primary_pool_post_create(
		struct xio_transport_base *transport_hndl,
		void *pool, void *pool_dd_data)
{
	struct xio_task		*task = NULL;
	struct xio_ucx_task	*ucx_task = NULL;
	int			i;
	struct xio_ucx_transport *ucx_hndl =
		(struct xio_ucx_transport *)transport_hndl;

	if (!ucx_hndl)
		return 0;

	ucx_hndl->primary_pool_cls.pool = pool;

	for (i = 0; i < RX_LIST_POST_NR; i++) {
		/* get ready to receive message */
		task = xio_ucx_primary_task_alloc(ucx_hndl);
		if (task == 0) {
			ERROR_LOG("primary task pool is empty\n");
			return -1;
		}
		ucx_task = (struct xio_ucx_task *)task->dd_data;
		ucx_task->out_ucx_op = XIO_UCX_RECV;
		list_add_tail(&task->tasks_list_entry, &ucx_hndl->rx_list);
	}

	return 0;
}

/*---------------------------------------------------------------------------*/
/* xio_ucx_primary_pool_slab_destroy					     */
/*---------------------------------------------------------------------------*/
static int xio_ucx_primary_pool_slab_destroy(
		struct xio_transport_base *transport_hndl,
		void *pool_dd_data, void *slab_dd_data)
{
	struct xio_ucx_tasks_slab *ucx_slab =
		(struct xio_ucx_tasks_slab *)slab_dd_data;

	if (ucx_slab->reg_mem.addr)
		xio_mem_free(&ucx_slab->reg_mem);
	else
		ufree_huge_pages(ucx_slab->data_pool);

	return 0;
}

/*---------------------------------------------------------------------------*/
/* xio_ucx_primary_pool_slab_init_task					     */
/*---------------------------------------------------------------------------*/
static int xio_ucx_primary_pool_slab_init_task(
		struct xio_transport_base *transport_hndl,
		void *pool_dd_data,
		void *slab_dd_data, int tid, struct xio_task *task)
{
	struct xio_ucx_transport *ucx_hndl =
		(struct xio_ucx_transport *)transport_hndl;
	struct xio_ucx_tasks_slab *ucx_slab =
		(struct xio_ucx_tasks_slab *)slab_dd_data;
	void *buf = sum_to_ptr(ucx_slab->data_pool, tid * ucx_slab->buf_size);
	int  max_iovsz = max(ucx_options.max_out_iovsz,
				     ucx_options.max_in_iovsz) + 1;
	char *ptr;

	XIO_TO_UCX_TASK(task, ucx_task);

	/* fill xio_tco_task */
	ptr = (char *)ucx_task;
	ptr += sizeof(struct xio_ucx_task);

	/* fill xio_ucx_work_req */
	ucx_task->txd.msg_iov = (struct iovec *)ptr;
	ptr += (max_iovsz + 1) * sizeof(struct iovec);
	ucx_task->rxd.msg_iov = (struct iovec *)ptr;
	ptr += (max_iovsz + 1) * sizeof(struct iovec);

	ucx_task->read_reg_mem = (struct xio_reg_mem *)ptr;
	ptr += max_iovsz * sizeof(struct xio_reg_mem);
	ucx_task->write_reg_mem = (struct xio_reg_mem *)ptr;
	ptr += max_iovsz * sizeof(struct xio_reg_mem);

	ucx_task->req_in_sge = (struct xio_sge *)ptr;
	ptr += max_iovsz * sizeof(struct xio_sge);
	ucx_task->req_out_sge = (struct xio_sge *)ptr;
	ptr += max_iovsz * sizeof(struct xio_sge);
	ucx_task->rsp_out_sge = (struct xio_sge *)ptr;
	ptr += max_iovsz * sizeof(struct xio_sge);
	/*****************************************/

	ucx_task->out_ucx_op = (enum xio_ucx_op_code)0x200;
	xio_ucx_task_init(
			task,
			ucx_hndl,
			buf,
			ucx_slab->buf_size);

	return 0;
}

/*---------------------------------------------------------------------------*/
/* xio_ucx_primary_pool_get_params					     */
/*---------------------------------------------------------------------------*/
static void xio_ucx_primary_pool_get_params(
		struct xio_transport_base *transport_hndl,
		int *start_nr, int *max_nr, int *alloc_nr,
		int *pool_dd_sz, int *slab_dd_sz, int *task_dd_sz)
{
	int  max_iovsz = max(ucx_options.max_out_iovsz,
				    ucx_options.max_in_iovsz) + 1;

	/* per transport */
	*start_nr = NUM_START_PRIMARY_POOL_TASKS;
	*alloc_nr = NUM_ALLOC_PRIMARY_POOL_TASKS;
	*max_nr = max((g_options.snd_queue_depth_msgs +
		       g_options.rcv_queue_depth_msgs), *start_nr);

	*pool_dd_sz = 0;
	*slab_dd_sz = sizeof(struct xio_ucx_tasks_slab);
	*task_dd_sz = sizeof(struct xio_ucx_task) +
			(2 * (max_iovsz + 1)) * sizeof(struct iovec) +
			 2 * max_iovsz * sizeof(struct xio_reg_mem) +
			 3 * max_iovsz * sizeof(struct xio_sge);
}

static struct xio_tasks_pool_ops   primary_tasks_pool_ops;
/*---------------------------------------------------------------------------*/
static void init_primary_tasks_pool_ops(void)
{
	primary_tasks_pool_ops.pool_get_params =
		xio_ucx_primary_pool_get_params;
	primary_tasks_pool_ops.slab_pre_create =
		xio_ucx_primary_pool_slab_pre_create;
	primary_tasks_pool_ops.slab_destroy =
		xio_ucx_primary_pool_slab_destroy;
	primary_tasks_pool_ops.slab_init_task =
		xio_ucx_primary_pool_slab_init_task;
	primary_tasks_pool_ops.pool_post_create =
		xio_ucx_primary_pool_post_create;
	primary_tasks_pool_ops.task_pre_put = xio_ucx_task_pre_put;
};

/*---------------------------------------------------------------------------*/
/* xio_ucx_get_pools_ops						     */
/*---------------------------------------------------------------------------*/
static void xio_ucx_get_pools_ops(struct xio_transport_base *trans_hndl,
				  struct xio_tasks_pool_ops **initial_pool_ops,
				  struct xio_tasks_pool_ops **primary_pool_ops)
{
	*initial_pool_ops = &initial_tasks_pool_ops;
	*primary_pool_ops = &primary_tasks_pool_ops;
}

/*---------------------------------------------------------------------------*/
/* xio_ucx_set_pools_cls						     */
/*---------------------------------------------------------------------------*/
static void xio_ucx_set_pools_cls(struct xio_transport_base *trans_hndl,
				  struct xio_tasks_pool_cls *initial_pool_cls,
				  struct xio_tasks_pool_cls *primary_pool_cls)
{
	struct xio_ucx_transport *ucx_hndl =
		(struct xio_ucx_transport *)trans_hndl;

	if (initial_pool_cls)
		ucx_hndl->initial_pool_cls = *initial_pool_cls;
	if (primary_pool_cls)
		ucx_hndl->primary_pool_cls = *primary_pool_cls;
}

/*---------------------------------------------------------------------------*/
/* xio_ucx_set_opt                                                           */
/*---------------------------------------------------------------------------*/
static int xio_ucx_set_opt(void *xio_obj,
			   int optname, const void *optval, int optlen)
{
	switch (optname) {
	case XIO_OPTNAME_ENABLE_MEM_POOL:
		VALIDATE_SZ(sizeof(int));
		ucx_options.enable_mem_pool = *((int *)optval);
		return 0;
	case XIO_OPTNAME_ENABLE_DMA_LATENCY:
		VALIDATE_SZ(sizeof(int));
		ucx_options.enable_dma_latency = *((int *)optval);
		return 0;
	case XIO_OPTNAME_MAX_IN_IOVLEN:
		VALIDATE_SZ(sizeof(int));
		ucx_options.max_in_iovsz = *((int *)optval);
		return 0;
	case XIO_OPTNAME_MAX_OUT_IOVLEN:
		VALIDATE_SZ(sizeof(int));
		ucx_options.max_out_iovsz = *((int *)optval);
		return 0;
	default:
		break;
	}
	xio_set_error(XIO_E_NOT_SUPPORTED);
	return -1;
}

/*---------------------------------------------------------------------------*/
/* xio_ucx_get_opt                                                           */
/*---------------------------------------------------------------------------*/
static int xio_ucx_get_opt(void  *xio_obj,
			   int optname, void *optval, int *optlen)
{
	switch (optname) {
	case XIO_OPTNAME_ENABLE_MEM_POOL:
		*((int *)optval) = ucx_options.enable_mem_pool;
		*optlen = sizeof(int);
		return 0;
	case XIO_OPTNAME_ENABLE_DMA_LATENCY:
		*((int *)optval) = ucx_options.enable_dma_latency;
		*optlen = sizeof(int);
		return 0;
	case XIO_OPTNAME_MAX_IN_IOVLEN:
		*((int *)optval) = ucx_options.max_in_iovsz;
		*optlen = sizeof(int);
		return 0;
	case XIO_OPTNAME_MAX_OUT_IOVLEN:
		*((int *)optval) = ucx_options.max_out_iovsz;
		*optlen = sizeof(int);
		return 0;
	default:
		break;
	}
	xio_set_error(XIO_E_NOT_SUPPORTED);
	return -1;
}

/*---------------------------------------------------------------------------*/
/* xio_is_valid_in_req							     */
/*---------------------------------------------------------------------------*/
static int xio_ucx_is_valid_in_req(struct xio_msg *msg)
{
	unsigned int		i;
	unsigned int		mr_found = 0;
	struct xio_vmsg *vmsg = &msg->in;
	struct xio_sg_table_ops	*sgtbl_ops;
	void			*sgtbl;
	void			*sge;
	unsigned long		nents, max_nents;

	sgtbl		= xio_sg_table_get(&msg->in);
	sgtbl_ops	= (struct xio_sg_table_ops *)
				xio_sg_table_ops_get(msg->in.sgl_type);
	nents		= tbl_nents(sgtbl_ops, sgtbl);
	max_nents	= tbl_max_nents(sgtbl_ops, sgtbl);

	if ((nents > (unsigned long)ucx_options.max_in_iovsz) ||
	    (nents > max_nents) ||
	    (max_nents > (unsigned long)ucx_options.max_in_iovsz)) {
		return 0;
	}

	if (vmsg->sgl_type == XIO_SGL_TYPE_IOV && nents > XIO_IOVLEN)
		return 0;

	if (vmsg->header.iov_base &&
	    (vmsg->header.iov_len == 0))
		return 0;

	for_each_sge(sgtbl, sgtbl_ops, sge, i) {
		if (sge_mr(sgtbl_ops, sge))
			mr_found++;
		if (!sge_addr(sgtbl_ops, sge)) {
			if (sge_mr(sgtbl_ops, sge))
				return 0;
		} else {
			if (sge_length(sgtbl_ops, sge)  == 0)
				return 0;
		}
	}
	if (ucx_options.enable_mr_check &&
	    (mr_found != nents) && mr_found)
		return 0;

	return 1;
}

/*---------------------------------------------------------------------------*/
/* xio_ucx_is_valid_out_msg						     */
/*---------------------------------------------------------------------------*/
static int xio_ucx_is_valid_out_msg(struct xio_msg *msg)
{
	unsigned int		i;
	unsigned int		mr_found = 0;
	struct xio_vmsg		*vmsg = &msg->out;
	struct xio_sg_table_ops	*sgtbl_ops;
	void			*sgtbl;
	void			*sge;
	unsigned long		nents, max_nents;

	sgtbl		= xio_sg_table_get(&msg->out);
	sgtbl_ops	= (struct xio_sg_table_ops *)
				xio_sg_table_ops_get(msg->out.sgl_type);
	nents		= tbl_nents(sgtbl_ops, sgtbl);
	max_nents	= tbl_max_nents(sgtbl_ops, sgtbl);

	if ((nents > (unsigned long)ucx_options.max_out_iovsz) ||
	    (nents > max_nents) ||
	    (max_nents > (unsigned long)ucx_options.max_out_iovsz))
		return 0;

	if (vmsg->sgl_type == XIO_SGL_TYPE_IOV && nents > XIO_IOVLEN)
		return 0;

	if ((vmsg->header.iov_base  &&
	     (vmsg->header.iov_len == 0)) ||
	    (!vmsg->header.iov_base  &&
	     (vmsg->header.iov_len != 0)))
			return 0;

	if (vmsg->header.iov_len > (size_t)g_options.max_inline_xio_hdr)
		return 0;

	for_each_sge(sgtbl, sgtbl_ops, sge, i) {
		if (sge_mr(sgtbl_ops, sge))
			mr_found++;
		if (!sge_addr(sgtbl_ops, sge) ||
		    (sge_length(sgtbl_ops, sge) == 0))
			return 0;
	}

	if (ucx_options.enable_mr_check &&
	    (mr_found != nents) && mr_found)
		return 0;

	return 1;
}

/*---------------------------------------------------------------------------*/
/* xio_ucx_dup2			                                             */
/* makes new_trans_hndl be the copy of old_trans_hndl, closes new_trans_hndl */
/* Note old and new are in dup2 terminology opposite to reconnect terms	     */
/* --------------------------------------------------------------------------*/
static int xio_ucx_dup2(struct xio_transport_base *old_trans_hndl,
			struct xio_transport_base **new_trans_hndl)
{
	xio_ucx_close(*new_trans_hndl);

	/* conn layer will call close which will only decrement */
	/*kref_get(&old_trans_hndl->kref);*/
	*new_trans_hndl = old_trans_hndl;

	return 0;
}

struct xio_transport xio_ucx_transport;
/*---------------------------------------------------------------------------*/
static void init_xio_ucx_transport(void)
{
	xio_ucx_transport.name = "ucx";
	xio_ucx_transport.ctor = xio_ucx_transport_constructor;
	xio_ucx_transport.dtor = xio_ucx_transport_destructor;
	xio_ucx_transport.init = xio_ucx_transport_init;
	xio_ucx_transport.release = xio_ucx_transport_release;
	xio_ucx_transport.context_shutdown = xio_ucx_context_shutdown;
	xio_ucx_transport.open = xio_ucx_open;
	xio_ucx_transport.connect = xio_ucx_connect;
	xio_ucx_transport.listen = xio_ucx_listen;
	xio_ucx_transport.accept = xio_ucx_accept;
	xio_ucx_transport.reject = xio_ucx_reject;
	xio_ucx_transport.close = xio_ucx_close;
	xio_ucx_transport.dup2 = xio_ucx_dup2;
	/*	.update_task		= xio_ucx_update_task;*/
	xio_ucx_transport.send = xio_ucx_send;
	xio_ucx_transport.poll = xio_ucx_poll;
	xio_ucx_transport.set_opt = xio_ucx_set_opt;
	xio_ucx_transport.get_opt = xio_ucx_get_opt;
	xio_ucx_transport.cancel_req = xio_ucx_cancel_req;
	xio_ucx_transport.cancel_rsp = xio_ucx_cancel_rsp;
	xio_ucx_transport.get_pools_setup_ops = xio_ucx_get_pools_ops;
	xio_ucx_transport.set_pools_cls = xio_ucx_set_pools_cls;

	xio_ucx_transport.validators_cls.is_valid_in_req =
						xio_ucx_is_valid_in_req;
	xio_ucx_transport.validators_cls.is_valid_out_msg =
						xio_ucx_is_valid_out_msg;
	ucp.open = xio_ucx_single_sock_create;
	ucp.add_ev_handlers = xio_ucx_single_sock_add_ev_handlers;
	ucp.del_ev_handlers = xio_ucx_single_sock_del_ev_handlers;
	ucp.connect = xio_ucx_single_sock_connect;
	ucp.set_txd = xio_ucx_single_sock_set_txd;
	ucp.set_rxd = xio_ucx_single_sock_set_rxd;
	ucp.rx_ctl_work = xio_ucx_recvmsg_work;
	ucp.rx_ctl_handler = xio_ucx_single_sock_rx_ctl_handler;
	ucp.rx_data_handler = xio_ucx_rx_data_handler;
	ucp.shutdown = xio_ucx_single_sock_shutdown;
	ucp.close = xio_ucx_single_sock_close;
}

/*---------------------------------------------------------------------------*/
static void init_static_structs(void)
{
	init_initial_tasks_pool_ops();
	init_primary_tasks_pool_ops();
	init_xio_ucx_transport();
}

/*---------------------------------------------------------------------------*/
/* xio_ucx_get_transport_func_list					     */
/*---------------------------------------------------------------------------*/
struct xio_transport *xio_ucx_get_transport_func_list(void)
{
	init_static_structs();
	return &xio_ucx_transport;
}

