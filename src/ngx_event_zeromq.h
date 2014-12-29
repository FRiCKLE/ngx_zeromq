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

#ifndef _NGX_EVENT_ZEROMQ_H_INCLUDED_
#define _NGX_EVENT_ZEROMQ_H_INCLUDED_


#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_event_connect.h>


#define NGX_ZEROMQ_REQ   0
#define NGX_ZEROMQ_PUSH  1
#define NGX_ZEROMQ_PULL  2


typedef struct ngx_zeromq_connection_s  ngx_zeromq_connection_t;

typedef struct {
    ngx_str_t                 name;
    int                       value;
    unsigned                  can_send:1;
    unsigned                  can_recv:1;
} ngx_zeromq_socket_t;


typedef struct {
    ngx_zeromq_socket_t      *type;
    ngx_str_t                 addr;
    unsigned                  bind:1;
    unsigned                  rand:1;
} ngx_zeromq_endpoint_t;


struct ngx_zeromq_connection_s {
    ngx_connection_t          connection;
    ngx_connection_t         *connection_ptr;

    ngx_zeromq_endpoint_t    *endpoint;
    void                     *socket;

    ngx_event_handler_pt      handler;

    ngx_zeromq_connection_t  *send;
    ngx_zeromq_connection_t  *recv;

    unsigned                  request_sent:1;
};


ngx_zeromq_endpoint_t *ngx_zeromq_randomized_endpoint(
    ngx_zeromq_endpoint_t *zep, ngx_pool_t *pool);

ngx_chain_t *ngx_zeromq_headers_add_http(ngx_chain_t *in,
    ngx_zeromq_endpoint_t *zep, ngx_pool_t *pool);
void ngx_zeromq_headers_set_http(ngx_buf_t *b, ngx_zeromq_endpoint_t *zep);

ngx_int_t ngx_zeromq_connect(ngx_peer_connection_t *pc);
void ngx_zeromq_close(ngx_zeromq_connection_t *zc);


extern ngx_zeromq_socket_t  ngx_zeromq_socket_types[];
extern ngx_int_t            ngx_zeromq_used;


#endif /* _NGX_EVENT_ZEROMQ_H_INCLUDED_ */
