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
#include <ngx_event_zeromq.h>
#include <ngx_http.h>
#include <nginx.h>


typedef struct {
    ngx_http_upstream_rr_peer_data_t   rrp;
    void                              *zmq;
    ngx_http_request_t                *request;
} ngx_http_upstream_zeromq_peer_data_t;


static ngx_int_t ngx_http_upstream_init_zeromq_peer(ngx_http_request_t *r,
    ngx_http_upstream_srv_conf_t *us);
static ngx_int_t ngx_http_upstream_get_zeromq_peer(ngx_peer_connection_t *pc,
    void *data);
static void ngx_http_upstream_free_zeromq_peer(ngx_peer_connection_t *pc,
    void *data, ngx_uint_t state);

static char *ngx_http_upstream_zeromq(ngx_conf_t *cf, ngx_command_t *cmd,
    void *conf);


static ngx_command_t  ngx_http_upstream_zeromq_commands[] = {

    { ngx_string("zeromq"),
      NGX_HTTP_UPS_CONF|NGX_CONF_NOARGS,
      ngx_http_upstream_zeromq,
      0,
      0,
      NULL },

      ngx_null_command
};


static ngx_http_module_t  ngx_http_upstream_zeromq_module_ctx = {
    NULL,                                  /* preconfiguration */
    NULL,                                  /* postconfiguration */

    NULL,                                  /* create main configuration */
    NULL,                                  /* init main configuration */

    NULL,                                  /* create server configuration */
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


ngx_int_t
ngx_http_upstream_init_zeromq(ngx_conf_t *cf, ngx_http_upstream_srv_conf_t *us)
{
    ngx_http_upstream_rr_peers_t  *peers;
    ngx_str_t                      name;
    ngx_uint_t                     i;

    if (ngx_http_upstream_init_round_robin(cf, us) != NGX_OK) {
        return NGX_ERROR;
    }

    if (us->servers) {
        peers = us->peer.data;

        for (i = 0; i < peers->number; i++) {
            name.len = sizeof("tcp://") - 1 + peers->peer[i].name.len;
            name.data = ngx_pnalloc(cf->pool, name.len + 1);

            (void) ngx_snprintf(name.data, name.len, "tcp://%V",
                                &peers->peer[i].name);
            name.data[name.len] = '\0';

            peers->peer[i].name = name;
        }
    }

    us->peer.init = ngx_http_upstream_init_zeromq_peer;

    return NGX_OK;
}


static ngx_int_t
ngx_http_upstream_init_zeromq_peer(ngx_http_request_t *r,
    ngx_http_upstream_srv_conf_t *us)
{
    ngx_http_upstream_zeromq_peer_data_t  *zp;

    zp = ngx_palloc(r->pool, sizeof(ngx_http_upstream_zeromq_peer_data_t));
    if (zp == NULL) {
        return NGX_ERROR;
    }

    zp ->request = r;
    zp ->zmq = NULL;
    ngx_http_set_ctx(r, NULL, ngx_zeromq_module);

    r->upstream->peer.data = &zp->rrp;

    if (ngx_http_upstream_init_round_robin_peer(r, us) != NGX_OK) {
        return NGX_ERROR;
    }

    r->upstream->peer.get = ngx_http_upstream_get_zeromq_peer;
    r->upstream->peer.free = ngx_http_upstream_free_zeromq_peer;

    return NGX_OK;
}


static ngx_int_t
ngx_http_upstream_get_zeromq_peer(ngx_peer_connection_t *pc, void *data)
{
    ngx_http_upstream_zeromq_peer_data_t  *zp = data;
    ngx_int_t                              rc;

    rc = ngx_http_upstream_get_round_robin_peer(pc, data);
    if (rc != NGX_OK) {
        return rc;
    }

    rc = ngx_zeromq_connect(pc);
    if (rc != NGX_OK) {
        return rc;
    }

    zp->zmq = pc->connection->data;
    ngx_http_set_ctx(zp->request, zp->zmq, ngx_zeromq_module);

    return NGX_DONE;
}


void
ngx_http_upstream_free_zeromq_peer(ngx_peer_connection_t *pc, void *data,
    ngx_uint_t state)
{
    ngx_http_upstream_zeromq_peer_data_t  *zp = data;

    ngx_http_upstream_free_round_robin_peer(pc, data, state);

    if (pc->connection) {
#if defined(nginx_version) && (nginx_version >= 1001004)
        if (pc->connection->pool) {
            ngx_destroy_pool(pc->connection->pool);
        }
#endif

        pc->connection->data = zp->zmq;
        ngx_zeromq_close(pc->connection);
        pc->connection = NULL;
    }
}


static char *
ngx_http_upstream_zeromq(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    ngx_http_upstream_srv_conf_t  *uscf;

    uscf = ngx_http_conf_get_module_srv_conf(cf, ngx_http_upstream_module);

    uscf->peer.init_upstream = ngx_http_upstream_init_zeromq;

    uscf->flags = NGX_HTTP_UPSTREAM_CREATE
                  |NGX_HTTP_UPSTREAM_MAX_FAILS
                  |NGX_HTTP_UPSTREAM_FAIL_TIMEOUT
                  |NGX_HTTP_UPSTREAM_DOWN;

    zmq_used = 1;

    return NGX_CONF_OK;
}
