/*
 * Copyright (c) 2012-2014, FRiCKLE <info@frickle.com>
 * Copyright (c) 2012-2014, Piotr Sikora <piotr.sikora@frickle.com>
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
#include <ngx_event_zeromq.h>
#include <ngx_http.h>
#include <nginx.h>


#ifndef nginx_version
#error This module cannot be build against an unknown nginx version.
#endif


typedef struct {
    ngx_zeromq_endpoint_t    *send;
    ngx_zeromq_endpoint_t    *recv;
    ngx_flag_t                single;
} ngx_http_upstream_zeromq_srv_conf_t;


typedef struct {
    ngx_zeromq_connection_t   send;
    ngx_zeromq_connection_t   recv;

    ngx_http_request_t       *request;
    ngx_chain_t              *headers;
} ngx_http_upstream_zeromq_peer_data_t;


static ngx_int_t ngx_http_upstream_init_zeromq(ngx_conf_t *cf,
    ngx_http_upstream_srv_conf_t *us);

static ngx_int_t ngx_http_upstream_init_zeromq_peer(ngx_http_request_t *r,
    ngx_http_upstream_srv_conf_t *us);
static ngx_int_t ngx_http_upstream_get_zeromq_peer(ngx_peer_connection_t *pc,
    void *data);
static void ngx_http_upstream_free_zeromq_peer(ngx_peer_connection_t *pc,
    void *data, ngx_uint_t state);

static void *ngx_http_upstream_zeromq_create_conf(ngx_conf_t *cf);
static char *ngx_http_upstream_zeromq_endpoint(ngx_conf_t *cf,
    ngx_command_t *cmd, void *conf);


static ngx_command_t  ngx_http_upstream_zeromq_commands[] = {

    { ngx_string("zeromq_local"),
      NGX_HTTP_UPS_CONF|NGX_CONF_TAKE2,
      ngx_http_upstream_zeromq_endpoint,
      NGX_HTTP_SRV_CONF_OFFSET,
      1,
      NULL },

    { ngx_string("zeromq_remote"),
      NGX_HTTP_UPS_CONF|NGX_CONF_TAKE2,
      ngx_http_upstream_zeromq_endpoint,
      NGX_HTTP_SRV_CONF_OFFSET,
      0,
      NULL },

    { ngx_string("zeromq_single"),
      NGX_HTTP_UPS_CONF|NGX_CONF_FLAG,
      ngx_conf_set_flag_slot,
      NGX_HTTP_SRV_CONF_OFFSET,
      offsetof(ngx_http_upstream_zeromq_srv_conf_t, single),
      NULL },

      ngx_null_command
};


static ngx_http_module_t  ngx_http_upstream_zeromq_module_ctx = {
    NULL,                                  /* preconfiguration */
    NULL,                                  /* postconfiguration */

    NULL,                                  /* create main configuration */
    NULL,                                  /* init main configuration */

    ngx_http_upstream_zeromq_create_conf,  /* create server configuration */
    NULL,                                  /* merge server configuration */

    NULL,                                  /* create location configuration */
    NULL                                   /* merge location configuration */
};


ngx_module_t  ngx_http_upstream_zeromq_module = {
    NGX_MODULE_V1,
    &ngx_http_upstream_zeromq_module_ctx,  /* module context */
    ngx_http_upstream_zeromq_commands,     /* module directives */
    NGX_HTTP_MODULE,                       /* module type */
    NULL,                                  /* init master */
    NULL,                                  /* init module */
    NULL,                                  /* init process */
    NULL,                                  /* init thread */
    NULL,                                  /* exit thread */
    NULL,                                  /* exit process */
    NULL,                                  /* exit master */
    NGX_MODULE_V1_PADDING
};


static ngx_int_t
ngx_http_upstream_init_zeromq(ngx_conf_t *cf, ngx_http_upstream_srv_conf_t *us)
{
    ngx_http_upstream_zeromq_srv_conf_t  *zcf;

    zcf = ngx_http_conf_upstream_srv_conf(us, ngx_http_upstream_zeromq_module);

    if (zcf->send == NULL) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "missing sending endpoint in upstream \"%V\"",
                           &us->host);
        return NGX_ERROR;
    }

    if (zcf->recv == NULL) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "missing receiving endpoint in upstream \"%V\"",
                           &us->host);
        return NGX_ERROR;
    }

    if ((zcf->single != 1)
        && ((zcf->send->bind && !zcf->send->rand)
            || (zcf->recv->bind && !zcf->recv->rand)))
    {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "local endpoint must use random port numbers,"
                           " use \"tcp://A.B.C.D:*\" in upstream \"%V\"",
                           &us->host);
        return NGX_ERROR;
    }

    us->peer.init = ngx_http_upstream_init_zeromq_peer;

    return NGX_OK;
}


static ngx_int_t
ngx_http_upstream_init_zeromq_peer(ngx_http_request_t *r,
    ngx_http_upstream_srv_conf_t *us)
{
    ngx_http_upstream_zeromq_peer_data_t  *zp;
    ngx_http_upstream_zeromq_srv_conf_t   *zcf;
    ngx_http_upstream_t                   *u;

    zp = ngx_pcalloc(r->pool, sizeof(ngx_http_upstream_zeromq_peer_data_t));
    if (zp == NULL) {
        return NGX_ERROR;
    }

    zcf = ngx_http_conf_upstream_srv_conf(us, ngx_http_upstream_zeromq_module);

    if (zcf->send->rand) {
        zp->send.endpoint = ngx_zeromq_randomized_endpoint(zcf->send, r->pool);
        if (zp->send.endpoint == NULL) {
            return NGX_ERROR;
        }

    } else {
        zp->send.endpoint = zcf->send;
    }

    if (zcf->recv != zcf->send) {
        if (zcf->recv->rand) {
            zp->recv.endpoint = ngx_zeromq_randomized_endpoint(zcf->recv,
                                                               r->pool);
            if (zp->recv.endpoint == NULL) {
                return NGX_ERROR;
            }

        } else {
            zp->recv.endpoint = zcf->recv;
        }

    } else {
        zp->recv.endpoint = zp->send.endpoint;
    }

    zp->request = r;

    u = r->upstream;

    u->peer.data = zp;
    u->peer.name = &zp->send.endpoint->addr;

    u->peer.get = ngx_http_upstream_get_zeromq_peer;
    u->peer.free = ngx_http_upstream_free_zeromq_peer;

    if (zp->recv.endpoint->type != &ngx_zeromq_socket_types[NGX_ZEROMQ_REQ]) {
        if (u->conf->module.len == sizeof("proxy") - 1
            && ngx_strncmp(u->conf->module.data, "proxy",
                           sizeof("proxy") - 1) == 0)
        {
            zp->headers = ngx_zeromq_headers_add_http(u->request_bufs,
                                                      zp->recv.endpoint,
                                                      r->pool);
            if (zp->headers == NGX_CHAIN_ERROR) {
                return NGX_ERROR;
            }
        }
    }

    return NGX_OK;
}


static ngx_int_t
ngx_http_upstream_get_zeromq_peer(ngx_peer_connection_t *pc, void *data)
{
    ngx_http_upstream_zeromq_peer_data_t  *zp = data;
    ngx_int_t                              rc;

    if (zp->recv.endpoint != zp->send.endpoint) {
        pc->data = &zp->recv;
        rc = ngx_zeromq_connect(pc);
        pc->data = data;

        if (rc != NGX_OK) {
            return rc;
        }

        zp->recv.connection.data = zp->request;
    }

    pc->data = &zp->send;
    rc = ngx_zeromq_connect(pc);
    pc->data = data;

    if (rc != NGX_OK) {
        return rc;
    }

    zp->send.connection.data = zp->request;

    if (zp->recv.endpoint != zp->send.endpoint) {
        zp->send.recv = zp->recv.recv;
        zp->recv.send = zp->send.send;
    }

    if (zp->recv.endpoint->rand && zp->headers) {
        ngx_zeromq_headers_set_http(zp->headers->buf, zp->recv.endpoint);
    }

    if (pc->connection->pool == NULL) {
        pc->connection->pool = ngx_create_pool(128, pc->log);
        if (pc->connection->pool == NULL) {
            return NGX_ERROR;
        }
    }

    pc->sockaddr = ngx_pcalloc(pc->connection->pool, sizeof(struct sockaddr));
    if (pc->sockaddr == NULL) {
        return NGX_ERROR;
    }

    pc->sockaddr->sa_family = AF_UNSPEC;
    pc->socklen = sizeof(struct sockaddr);

    return NGX_DONE;
}


void
ngx_http_upstream_free_zeromq_peer(ngx_peer_connection_t *pc, void *data,
    ngx_uint_t state)
{
    ngx_http_upstream_zeromq_peer_data_t  *zp = data;

    if (pc->connection) {
#if (nginx_version >= 1001004)
        if (pc->connection->pool) {
            ngx_destroy_pool(pc->connection->pool);
        }
#endif

        pc->connection = NULL;
    }

    if (zp->recv.endpoint != zp->send.endpoint) {
        if (zp->recv.socket) {
            ngx_zeromq_close(&zp->recv);
        }
    }

    if (zp->send.socket) {
        ngx_zeromq_close(&zp->send);
    }
}


static void *
ngx_http_upstream_zeromq_create_conf(ngx_conf_t *cf)
{
    ngx_http_upstream_zeromq_srv_conf_t  *conf;

    conf = ngx_pcalloc(cf->pool, sizeof(ngx_http_upstream_zeromq_srv_conf_t));
    if (conf == NULL) {
        return NULL;
    }

    /*
     * set by ngx_pcalloc():
     *
     *     conf->send = NULL;
     *     conf->recv = NULL;
     */

    conf->single = NGX_CONF_UNSET;

    return conf;
}


static char *
ngx_http_upstream_zeromq_endpoint(ngx_conf_t *cf, ngx_command_t *cmd,
    void *conf)
{
    ngx_str_t                            *value = cf->args->elts;
    ngx_http_upstream_zeromq_srv_conf_t  *zcf = conf;
    ngx_http_upstream_srv_conf_t         *uscf;
    ngx_zeromq_socket_t                  *type;
    ngx_zeromq_endpoint_t                *zep;
    ngx_uint_t                            i;

    uscf = ngx_http_conf_get_module_srv_conf(cf, ngx_http_upstream_module);

    type = ngx_zeromq_socket_types;
    for (i = 0; type[i].value; i++) {
        if (ngx_strcmp(value[1].data, type[i].name.data) == 0) {
            break;
        }
    }

    if (type[i].value == 0) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "invalid socket type \"%V\" in upstream \"%V\"",
                           &value[1], &uscf->host);
        return NGX_CONF_ERROR;
    }

    if (type[i].can_send) {
        if (zcf->send) {
            ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                               "sending endpoint already set to"
                               " \"%V\" (%V) in upstream \"%V\"",
                               &zcf->send->addr, &zcf->send->type->name,
                               &uscf->host);
            return NGX_CONF_ERROR;
        }
    }

    if (type[i].can_recv) {
        if (zcf->recv) {
            ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                               "receivng endpoint already set to"
                               " \"%V\" (%V) in upstream \"%V\"",
                               &zcf->recv->addr, &zcf->recv->type->name,
                               &uscf->host);
            return NGX_CONF_ERROR;
        }
    }

    zep = ngx_pcalloc(cf->pool, sizeof(ngx_zeromq_endpoint_t));
    if (zep == NULL) {
        return NGX_CONF_ERROR;
    }

    zep->type = &type[i];
    zep->addr = value[2];
    zep->bind = cmd->offset;

    if ((ngx_strncmp(zep->addr.data, "tcp://", sizeof("tcp://") - 1) == 0)
        && (ngx_strncmp(zep->addr.data + zep->addr.len - (sizeof(":*") - 1),
                        ":*", sizeof(":*") - 1) == 0))
    {
        zep->rand = 1;
        zep->addr.len -=  sizeof("*") - 1;
    }

    if (zep->rand && !zep->bind) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "random port numbers don't make sense for remote"
                           " endpoint in upstream \"%V\"", &uscf->host);
        return NGX_CONF_ERROR;
    }

    if (type[i].can_send) {
        zcf->send = zep;
    }

    if (type[i].can_recv) {
        zcf->recv = zep;
    }

    if (uscf->servers == NULL) {
        uscf->servers = ngx_pcalloc(cf->pool, sizeof(ngx_array_t));
        if (uscf->servers == NULL) {
            return NGX_CONF_ERROR;
        }
    }

    if (uscf->servers->nelts == 0) {
        uscf->servers->nelts = 1;
    }

    uscf->peer.init_upstream = ngx_http_upstream_init_zeromq;

    ngx_zeromq_used = 1;

    return NGX_CONF_OK;
}
