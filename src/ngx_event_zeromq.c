/*
 * Copyright (c) 2012, FRiCKLE <info@frickle.com>
 * Copyright (c) 2012, Piotr Sikora <piotr.sikora@frickle.com>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * HOLDERS OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_event.h>
#include <ngx_event_connect.h>
#include <ngx_event_zeromq.h>
#include <ngx_http.h>
#include <zmq.h>


#if ZMQ_VERSION_MAJOR < 3
#define ZMQ_DONTWAIT  ZMQ_NOBLOCK
#define zmq_sendmsg   zmq_send
#define zmq_recvmsg   zmq_recv
#endif


typedef struct {
    ngx_int_t  threads;
} ngx_zeromq_conf_t;


static void *ngx_zeromq_get_socket(ngx_connection_t *c);
static void ngx_zeromq_log_error(ngx_log_t *log, const char *text);

static ngx_int_t ngx_zeromq_ready(void *zmq, ngx_event_t *ev, const char *what,
    uint32_t want);
static ssize_t ngx_zeromq_sendmsg(void *zmq, ngx_event_t *ev, zmq_msg_t *msg,
    int flags);
static ssize_t ngx_zeromq_recvmsg(void *zmq, ngx_event_t *ev, zmq_msg_t *msg);

static ssize_t ngx_zeromq_send_part(void *zmq, ngx_event_t *wev, u_char *buf,
    size_t size, int flags);
static ngx_chain_t *ngx_zeromq_send_chain(ngx_connection_t *c, ngx_chain_t *in,
    off_t limit);

static ssize_t ngx_zeromq_recv_part(void *zmq, ngx_event_t *rev, u_char *buf,
    size_t size);
static ssize_t ngx_zeromq_recv(ngx_connection_t *c, u_char *buf, size_t size);
static ssize_t ngx_zeromq_recv_chain(ngx_connection_t *c, ngx_chain_t *cl);

static void *ngx_zeromq_create_conf(ngx_cycle_t *cycle);
static char *ngx_zeromq_init_conf(ngx_cycle_t *cycle, void *conf);
static char *ngx_zeromq_threads(ngx_conf_t *cf, ngx_command_t *cmd, void *conf);

static ngx_int_t ngx_zeromq_module_init(ngx_cycle_t *cycle);
static ngx_int_t ngx_zeromq_process_init(ngx_cycle_t *cycle);
static void ngx_zeromq_process_exit(ngx_cycle_t *cycle);


ngx_zeromq_socket_t  ngx_zeromq_socket_types[] = {
    { ngx_string("REQ"),  ZMQ_REQ,  1, 1 },
    { ngx_string("PUSH"), ZMQ_PUSH, 1, 0 },
    { ngx_null_string, 0, 0, 0 }
};


static ngx_command_t  ngx_zeromq_commands[] = {

    { ngx_string("zeromq_threads"),
      NGX_MAIN_CONF|NGX_DIRECT_CONF|NGX_CONF_TAKE1,
      ngx_zeromq_threads,
      0,
      0,
      NULL },

      ngx_null_command
};


static ngx_core_module_t  ngx_zeromq_module_ctx = {
    ngx_string("zeromq"),
    ngx_zeromq_create_conf,
    ngx_zeromq_init_conf
};


ngx_module_t  ngx_zeromq_module = {
    NGX_MODULE_V1,
    &ngx_zeromq_module_ctx,                /* module context */
    ngx_zeromq_commands,                   /* module directives */
    NGX_CORE_MODULE,                       /* module type */
    NULL,                                  /* init master */
    ngx_zeromq_module_init,                /* init module */
    ngx_zeromq_process_init,               /* init process */
    NULL,                                  /* init thread */
    NULL,                                  /* exit thread */
    ngx_zeromq_process_exit,               /* exit process */
    NULL,                                  /* exit master */
    NGX_MODULE_V1_PADDING
};


void *zmq_context;
int   zmq_used;


static void *
ngx_zeromq_get_socket(ngx_connection_t *c)
{
    ngx_http_request_t  *r = c->data;
    return ngx_http_get_module_ctx(r, ngx_zeromq_module);
}


static void
ngx_zeromq_log_error(ngx_log_t *log, const char *text)
{
    ngx_log_error(NGX_LOG_ALERT, log, 0, "%s failed (%d: %s)",
                  text, ngx_errno, zmq_strerror(ngx_errno));
}


ngx_int_t
ngx_zeromq_connect(ngx_peer_connection_t *pc)
{
    ngx_zeromq_connection_t  *zc = pc->data;
    ngx_zeromq_endpoint_t    *zep;
    ngx_connection_t         *c;
    ngx_event_t              *rev, *wev, *ev;
    void                     *zmq;
    int                       fd, zero;
    size_t                    fdsize;

    if (zc == NULL || zc->endpoint == NULL) {
        return NGX_ERROR;
    }

    zep = zc->endpoint;

    zmq = zmq_socket(zmq_context, zep->type->value);
    if (zmq == NULL) {
        ngx_log_error(NGX_LOG_ALERT, pc->log, 0,
                      "zmq_socket(%V) failed (%d: %s)",
                      &zep->type->name, ngx_errno, zmq_strerror(ngx_errno));
        return NGX_ERROR;
    }

    fdsize = sizeof(int);

    if (zmq_getsockopt(zmq, ZMQ_FD, &fd, &fdsize) == -1) {
        ngx_zeromq_log_error(pc->log, "zmq_getsockopt(ZMQ_FD)");
        goto failed_zmq;
    }

    zero = 0;

    if (zmq_setsockopt(zmq, ZMQ_LINGER, &zero, sizeof(int)) == -1) {
        ngx_zeromq_log_error(pc->log, "zmq_setsockopt(ZMQ_LINGER)");
        goto failed_zmq;
    }

    c = ngx_get_connection(fd, pc->log);
    if (c == NULL) {
        goto failed_zmq;
    }

    c->recv = ngx_zeromq_recv;
    c->send = NULL;
    c->recv_chain = ngx_zeromq_recv_chain;
    c->send_chain = ngx_zeromq_send_chain;

    /* This won't fly with ZeroMQ */
    c->sendfile = 0;
    c->tcp_nopush = NGX_TCP_NOPUSH_DISABLED;
    c->tcp_nodelay = NGX_TCP_NODELAY_DISABLED;

    c->log_error = pc->log_error;

    rev = c->read;
    wev = c->write;

    rev->data = zc;
    wev->data = zc;

    rev->log = pc->log;
    wev->log = pc->log;

    pc->connection = c;
    zc->connection = *c;

    zc->socket = zmq;

    c->number = ngx_atomic_fetch_add(ngx_connection_counter, 1);

    if (pc->local) {
        ngx_log_error(NGX_LOG_WARN, pc->log, 0,
                      "zmq_connect: binding to local address is not supported");
    }

    if (zep->bind) {
        if (zmq_bind(zmq, (const char *) zep->addr->data) == -1) {
            ngx_zeromq_log_error(pc->log, "zmq_bind()");
            goto failed;
        }

        ev = wev;

    } else {
        if (zmq_connect(zmq, (const char *) zep->addr->data) == -1) {
            ngx_zeromq_log_error(pc->log, "zmq_connect()");
            goto failed;
        }

        ev = rev;
    }

    ngx_log_debug6(NGX_LOG_DEBUG_EVENT, pc->log, 0,
                   "zmq_connect: lazily %s to %V (%V), zmq:%p fd:%d #%d",
                   zep->bind ? "bound" : "connected",
                   zep->addr, &zep->type->name, zmq, fd, c->number);

    if (ngx_add_conn) {
        /* rtsig */
        if (ngx_add_conn(c) == NGX_ERROR) {
            goto failed;
        }

    } else {
        if (ngx_event_flags & NGX_USE_CLEAR_EVENT) {
            /* kqueue, epoll */
            if (ngx_add_event(ev, NGX_READ_EVENT, NGX_CLEAR_EVENT) != NGX_OK) {
                goto failed;
            }

        } else {
            /* select, poll, /dev/poll */
            if (ngx_add_event(ev, NGX_READ_EVENT, NGX_LEVEL_EVENT) != NGX_OK) {
                goto failed;
            }
        }
    }

    /*
     * ZeroMQ assumes that new socket is read-ready (but it really isn't)
     * and it won't notify us about any new events if we don't fail to read
     * from it first. Sigh.
     */

    rev->ready = zep->type->can_recv;
    wev->ready = zep->type->can_send;

    return NGX_OK;

failed:

    ngx_free_connection(c);

    c->fd = (ngx_socket_t) -1;

failed_zmq:

    if (zmq_close(zmq) == -1) {
        ngx_zeromq_log_error(pc->log, "zmq_close()");
    }

    return NGX_ERROR;
}


void
ngx_zeromq_close(ngx_connection_t *c)
{
    void  *zmq;

    if (c->fd == -1) {
        return;
    }

    zmq = c->data;

    ngx_log_debug3(NGX_LOG_DEBUG_EVENT, c->log, 0,
                   "zmq_close: zmq:%p fd:%d #%d", zmq, c->fd, c->number);

    if (c->read->timer_set) {
        ngx_del_timer(c->read);
    }

    if (c->write->timer_set) {
        ngx_del_timer(c->write);
    }

    if (ngx_del_conn) {
        ngx_del_conn(c, NGX_CLOSE_EVENT);

    } else {
        if (c->read->active || c->read->disabled) {
            ngx_del_event(c->read, NGX_READ_EVENT, NGX_CLOSE_EVENT);
        }

        if (c->write->active || c->write->disabled) {
            ngx_del_event(c->write, NGX_WRITE_EVENT, NGX_CLOSE_EVENT);
        }
    }

    if (c->read->prev) {
        ngx_delete_posted_event(c->read);
    }

    if (c->write->prev) {
        ngx_delete_posted_event(c->write);
    }

    c->read->closed = 1;
    c->write->closed = 1;

    ngx_reusable_connection(c, 0);

    ngx_free_connection(c);

    c->fd = (ngx_socket_t) -1;

    if (zmq_close(zmq) == -1) {
        ngx_zeromq_log_error(ngx_cycle->log, "zmq_close()");
    }
}


static ngx_int_t
ngx_zeromq_ready(void *zmq, ngx_event_t *ev, const char *what, uint32_t want)
{
    uint32_t  flags;
    size_t    fsize;

    fsize = sizeof(uint32_t);

    if (zmq_getsockopt(zmq, ZMQ_EVENTS, &flags, &fsize) == -1) {
        ngx_zeromq_log_error(ev->log, "zmq_getsockopt(ZMQ_EVENTS)");

        ev->error = 1;
        return NGX_ERROR;
    }

    if (!(flags & want)) {
        ngx_log_debug1(NGX_LOG_DEBUG_EVENT, ev->log, 0, "%s: not ready", what);

        ev->ready = 0;
        return NGX_AGAIN;
    }

    return NGX_OK;
}


static ssize_t
ngx_zeromq_sendmsg(void *zmq, ngx_event_t *ev, zmq_msg_t *msg, int flags)
{
    size_t  size;

    size = zmq_msg_size(msg);

    for (;;) {
        if (zmq_sendmsg(zmq, msg, ZMQ_DONTWAIT|flags) == -1) {

            if (ngx_errno == NGX_EINTR) {
                ngx_log_debug0(NGX_LOG_DEBUG_EVENT, ev->log, 0,
                               "zmq_send: interrupted");
                ev->ready = 0;
                continue;
            }

            ngx_zeromq_log_error(ev->log, "zmq_sendmsg()");

            ev->error = 1;
            return NGX_ERROR;
        }

        break;
    }

    ngx_log_debug2(NGX_LOG_DEBUG_EVENT, ev->log, 0,
                   "zmq_send: %uz eom:%d", size, flags != ZMQ_SNDMORE);

    return size;
}


static ssize_t
ngx_zeromq_recvmsg(void *zmq, ngx_event_t *ev, zmq_msg_t *msg)
{
    int64_t  more;
    size_t   msize;

    for (;;) {
        if (zmq_recvmsg(zmq, msg, ZMQ_DONTWAIT) == -1) {

            if (ngx_errno == NGX_EINTR) {
                ngx_log_debug0(NGX_LOG_DEBUG_EVENT, ev->log, 0,
                               "zmq_recv: interrupted");
                ev->ready = 0;
                continue;
            }

            ngx_zeromq_log_error(ev->log, "zmq_recvmsg()");

            ev->error = 1;
            return NGX_ERROR;
        }

        break;
    }

    msize = sizeof(int64_t);

    if (zmq_getsockopt(zmq, ZMQ_RCVMORE, &more, &msize) == -1) {
        ngx_zeromq_log_error(ev->log, "zmq_getsockopt(ZMQ_RCVMORE)");

        ev->error = 1;
        return NGX_ERROR;
    }

    ev->eof = more ? 0 : 1;

    ngx_log_debug2(NGX_LOG_DEBUG_EVENT, ev->log, 0,
                   "zmq_recv: %uz eom:%d", zmq_msg_size(msg), ev->eof);

    return zmq_msg_size(msg);
}


static ssize_t
ngx_zeromq_send_part(void *zmq, ngx_event_t *wev, u_char *buf, size_t size,
    int flags)
{
    zmq_msg_t  zmq_msg;
    ssize_t    n;

    if (zmq_msg_init_size(&zmq_msg, size) == -1) {
        ngx_log_error(NGX_LOG_ALERT, wev->log, 0,
                      "zmq_msg_init_size(%uz) failed (%d: %s)",
                      size, ngx_errno, zmq_strerror(ngx_errno));
        return NGX_ERROR;
    }

    ngx_memcpy(zmq_msg_data(&zmq_msg), buf, size);

    n = ngx_zeromq_sendmsg(zmq, wev, &zmq_msg, flags);

    if (zmq_msg_close(&zmq_msg) == -1) {
        ngx_zeromq_log_error(wev->log, "zmq_msg_close()");
        return NGX_ERROR;
    }

    return n;
}


static ngx_chain_t *
ngx_zeromq_send_chain(ngx_connection_t *c, ngx_chain_t *in, off_t limit)
{
    ngx_event_t  *wev;
    ngx_chain_t  *cl;
    ngx_buf_t    *b;
    ngx_int_t     rc;
    void         *zmq;
    ssize_t       n;

    wev = c->write;

    zmq = ngx_zeromq_get_socket(c);

    rc = ngx_zeromq_ready(zmq, wev, "zmq_send", ZMQ_POLLOUT);

    if (rc == NGX_ERROR) {
        return NGX_CHAIN_ERROR;

    } else if (rc == NGX_AGAIN) {
        return in;
    }

    for (cl = in; cl; cl = cl->next) {

        b = cl->buf;

        if (ngx_buf_special(cl->buf)) {
            continue;
        }

        n = ngx_zeromq_send_part(zmq, wev, b->pos, b->last - b->pos,
                                 cl->next ? ZMQ_SNDMORE : 0);
        if (n < 0) {
            return (ngx_chain_t *) n;
        }

        b->pos = b->last;

        c->sent += n;
    }

    return NULL;
}


static ssize_t
ngx_zeromq_recv_part(void *zmq, ngx_event_t *rev, u_char *buf, size_t size)
{
    zmq_msg_t  zmq_msg;
    ssize_t    n;

    if (zmq_msg_init(&zmq_msg) == -1) {
        ngx_zeromq_log_error(rev->log, "zmq_msg_init()");
        return NGX_ERROR;
    }

    n = ngx_zeromq_recvmsg(zmq, rev, &zmq_msg);
    if (n < 0) {
        goto done;
    }

    if ((size_t) n > size) {
        ngx_log_error(NGX_LOG_ALERT, rev->log, 0,
                      "zmq_recv: ZeroMQ message part too big (%uz) to fit"
                      " into buffer (%uz)", n, size);

        n = NGX_ERROR;
        goto done;
    }

    ngx_memcpy(buf, zmq_msg_data(&zmq_msg), n);

done:

    if (zmq_msg_close(&zmq_msg) == -1) {
        ngx_zeromq_log_error(rev->log, "zmq_msg_close()");
        return NGX_ERROR;
    }

    return n;
}


static ssize_t
ngx_zeromq_recv(ngx_connection_t *c, u_char *buf, size_t size)
{
    ngx_http_request_t  *r;
    ngx_event_t         *rev;
    ngx_int_t            rc;
    void                *zmq;
    ssize_t              n;

    rev = c->read;

    if (rev->eof) {
        ngx_log_debug0(NGX_LOG_DEBUG_EVENT, c->log, 0, "zmq_recv: - eom:1");
        return 0;
    }

    zmq = ngx_zeromq_get_socket(c);

    rc = ngx_zeromq_ready(zmq, rev, "zmq_recv", ZMQ_POLLIN);
    if (rc < 0) {
        return rc;
    }

    n = ngx_zeromq_recv_part(zmq, rev, buf, size);
    if (n < 0) {
        return n;
    }

    /*
     * This *really* shouldn't be here, but we need to cheat nginx into
     * thinking that the whole buffer space was used, otherwise it will
     * try to read into remaining part of this buffer, which could lead
     * to message part not fitting in.
     */

    r = c->data;
    if (buf == r->upstream->buffer.start) {
        r->upstream->buffer.end += size;
    }

    return n;
}


static ssize_t
ngx_zeromq_recv_chain(ngx_connection_t *c, ngx_chain_t *cl)
{
    ngx_event_t  *rev;
    ngx_buf_t    *b;
    ngx_int_t     rc;
    void         *zmq;
    ssize_t       n, size;

    rev = c->read;

    if (rev->eof) {
        ngx_log_debug0(NGX_LOG_DEBUG_EVENT, c->log, 0, "zmq_recv: - eom:1");
        return 0;
    }

    zmq = ngx_zeromq_get_socket(c);

    rc = ngx_zeromq_ready(zmq, rev, "zmq_recv", ZMQ_POLLIN);
    if (rc < 0) {
        return rc;
    }

    size = 0;

    for (; cl; cl = cl->next) {

        b = cl->buf;

        n = ngx_zeromq_recv_part(zmq, rev, b->last, b->end - b->last);
        if (n < 0) {
            return n;
        }

        b->last += n;
        b->end = b->last;

        size += n;

        if (rev->eof) {
            break;
        }
    }

    return size;
}


static void *
ngx_zeromq_create_conf(ngx_cycle_t *cycle)
{
    ngx_zeromq_conf_t  *zcf;

    zmq_used = 0;

    zcf = ngx_pcalloc(cycle->pool, sizeof(ngx_zeromq_conf_t));
    if (zcf == NULL) {
        return NULL;
    }

    zcf->threads = NGX_CONF_UNSET;

    return zcf;
}


static char *
ngx_zeromq_init_conf(ngx_cycle_t *cycle, void *conf)
{
    ngx_zeromq_conf_t  *zcf = conf;

    ngx_conf_init_value(zcf->threads, 1);

    return NGX_CONF_OK;
}


static char *
ngx_zeromq_threads(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    ngx_zeromq_conf_t  *zcf = conf;
    ngx_str_t          *value;
    ngx_int_t           number;

    if (zcf->threads != NGX_CONF_UNSET) {
        return "is duplicate";
    }

    value = cf->args->elts;

    number = ngx_atoi(value[1].data, value[1].len);
    if (number <= 0) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "invalid number of threads \"%V\""
                           " in \"%V\" directive", &value[1], &cmd->name);
        return NGX_CONF_ERROR;
    }

    zcf->threads = number;

    return NGX_CONF_OK;
}


static ngx_int_t
ngx_zeromq_module_init(ngx_cycle_t *cycle)
{
    int  a, b, c;

    if (zmq_used && !ngx_test_config && ngx_process <= NGX_PROCESS_MASTER) {
        zmq_version(&a, &b, &c);
        ngx_log_error(NGX_LOG_NOTICE, cycle->log, 0,
                      "using ZeroMQ/%d.%d.%d", a, b, c);
    }

    return NGX_OK;
}


static ngx_int_t
ngx_zeromq_process_init(ngx_cycle_t *cycle)
{
    ngx_zeromq_conf_t  *zcf;

    zcf = (ngx_zeromq_conf_t *) ngx_get_conf(cycle->conf_ctx,
                                             ngx_zeromq_module);

    if (zmq_used) {
        zmq_context = zmq_init(zcf->threads);

        if (zmq_context == NULL) {
            ngx_zeromq_log_error(cycle->log, "zmq_init()");
            return NGX_ERROR;
        }
    }

    return NGX_OK;
}


static void
ngx_zeromq_process_exit(ngx_cycle_t *cycle)
{
    if (zmq_context != NULL && zmq_term(zmq_context) == -1) {
        ngx_zeromq_log_error(cycle->log, "zmq_term()");
    }
}
