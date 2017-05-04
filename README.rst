varnish-cache / 4.1-websocket branch
================
This branch contains a special improvement to better support WebSocket connections in varnish 4.1 (https://tools.ietf.org/html/rfc6455).
Its main purpose is to interact with WebSocket handshakes in all VCL functions, and not only in `recv{}/pipe{}`.


Issue Fixed #1 - Sticky Connection with Cookie
--------
Implementing cookies in VCL is simple, but it's not possible in websocket handshakes with vanilla Varnish.

With this branch, just add ``Set-Cookie`` in ``vcl_backend_response`` or ``vcl_deliver``, and open pipe after delivering response.

Issue Fixed #2 - Add server and backend hints to the response
--------
Usually, you can add extra debugging information in VCL (e.g. varnish server name, backend name, timing information, ...), but it's not possible with vanilla Varnish.

With this branch, just add ``set beresp.http.backend-hint = beresp.backend;`` in ``vcl_backend_response``, and open pipe after delivering response.

Issue Fixed #3 - Check backend handshake status
--------
Checking status code is something you usually do in VCL, however you can't do that once the pipe is open.

With this branch, just add ``if(beresp.status == 101)`` in ``vcl_backend_response``, and don't open pipe if you have a different status.

Howto upgrade Websocket connections compared to vanilla Varnish
--------
In Varnish 4.1 you do::

  sub vcl_recv {
       if (req.http.Upgrade ~ "(?i)websocket") {
           return (pipe);
       }
  }
  sub vcl_pipe {
       if (req.http.upgrade) {
           set bereq.http.upgrade = req.http.upgrade;
       }
  }

With this branch you can do::

  sub vcl_backend_fetch {
       if (bereq.http.Sec-WebSocket-Key) {
           set bereq.http.Connection = "upgrade";
           set bereq.http.Upgrade = "WebSocket";
       }
  }
  sub vcl_backend_response {
       if (beresp.status == 101 && beresp.http.Upgrade ~ "(?i)websocket") {
           set beresp.do_pipe = true;
       }
  }
  sub vcl_deliver {
       if (resp.status == 101 && req.http.Upgrade ~ "(?i)websocket") {
           set resp.http.Connection = "upgrade";
           set resp.http.Upgrade = "WebSocket";
       }
  }


*Note that comments and suggestions are welcomed.*


Varnish Cache
=============

This is Varnish Cache, the high-performance HTTP accelerator.

Documentation and additional information about Varnish is available on
https://www.varnish-cache.org/

Technical questions about Varnish and this release should be addressed
to <varnish-misc@varnish-cache.org>.  Please see
https://www.varnish-cache.org/trac/wiki/Contributing for how to
contribute patches and report bugs.

Questions about commercial support and services related to Varnish
should be addressed to <sales@varnish-software.com>.
