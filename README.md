About
=====
`ngx_zeromq` is a transport module which allows `nginx` to use ZeroMQ
message-oriented transport layer when communicating with upstream servers.

It's level 7 protocol agnostic, which means that it can be used with any
well-behaving upstream modules (`proxy`, `fastcgi`, `uwsgi`, `scgi`, etc.).


Status
======
This is experimental module.


Caveats
=======
At this time, `ngx_zeromq` support only REQ/REP ZeroMQ sockets, which means
that upstream response must be delivered in a single ZeroMQ message (although
it can be multipart message).


Each message part must fit into upstream buffer (as defined by
`proxy_buffer_size`, `proxy_buffers` and other directives).


ZeroMQ is hungry for file descriptors, it will crash and bring worker process
down with it once it runs out of them. In order to guarantee that it doesn't
happen, worker connections limit (see: `worker_connections`) should be set
few times lower than the file descriptors limit (see: `worker_rlimit_nofile`).


SSL is another transport module, which means that it cannot be combined with
`ngx_zeromq` on the same connection (but this matters only for communication
with upstream servers, clients can still use SSL when connecting to `nginx`).


Configuration directives
========================
zeromq_threads
--------------
* **syntax**: `zeromq_threads <number>`
* **default**: `1`
* **context**: `main`

Configure number of ZeroMQ I/O threads to be used by each worker process.


zeromq_local
------------
* **syntax**: `zeromq_local <socket_type> <local_endpoint>`
* **default**: `none`
* **context**: `upstream`

Configure local ZeroMQ endpoint (must use random port numbers).


zeromq_remote
-------------
* **syntax**: `zeromq_remote <socket_type> <remote_endpoint>`
* **default**: `none`
* **context**: `upstream`

Configure remote ZeroMQ endpoint (cannot use random port numbers).


zeromq_single
-------------
* **syntax**: `zeromq_single on|off`
* **default**: `off`
* **context**: `upstream`

Enable `single` mode which allows use of predefined port numbers for local
endpoints. It comes at a price (only one worker can bind to such endpoint
and it will stop working after `nginx` reload) and it's supposed to be used
only during testing and development. Do not use this mode in production.


Sample configuration
=====================
Use HTTP over ZeroMQ (using standard `proxy` module).

    http {
        upstream blackhole {
            zeromq_remote   REQ tcp://127.0.0.1:5555;
        }

        server {
            location / {
                proxy_pass  http://blackhole;
            }
        }
    }


License
=======

    Copyright (c) 2012-2014, FRiCKLE <info@frickle.com>
    Copyright (c) 2012-2014, Piotr Sikora <piotr.sikora@frickle.com>
    All rights reserved.

    Redistribution and use in source and binary forms, with or without
    modification, are permitted provided that the following conditions
    are met:
    1. Redistributions of source code must retain the above copyright
       notice, this list of conditions and the following disclaimer.
    2. Redistributions in binary form must reproduce the above copyright
       notice, this list of conditions and the following disclaimer in the
       documentation and/or other materials provided with the distribution.

    THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
    "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
    LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
    A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
    HOLDERS OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
    SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
    LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
    DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
    THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
    (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
    OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
