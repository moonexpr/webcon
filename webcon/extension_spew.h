#ifndef _INCLUDE_WEBCON_EXTENSION_SPEW_H_
#define _INCLUDE_WEBCON_EXTENSION_SPEW_H_

#include "microhttpd.h"

// Thread-safe debug logger for webcon. Writes to stderr (always) and,
// optionally, to a file. Output is prefixed with "[webcon] ".
//
// WebLogger_LogConn additionally prefixes the client's "ip:port", formatted
// with inet_ntop so it is safe to call from MHD worker threads.

bool WebLogger_Init(const char *path);   // path == NULL -> stderr only
void WebLogger_Shutdown();
void WebLogger_Log(const char *fmt, ...);
void WebLogger_LogConn(MHD_Connection *conn, const char *fmt, ...);

#endif // _INCLUDE_WEBCON_EXTENSION_SPEW_H_
