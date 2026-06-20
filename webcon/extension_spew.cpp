#include "extension_spew.h"

#include <cstdio>
#include <cstdarg>
#include <mutex>

#ifdef _WIN32
#include <io.h>
#include <Winsock2.h>
#include <Ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <errno.h>
#endif

struct WebLogger
{
    std::mutex mutex;
    FILE *file;
    bool toFile;
};

static WebLogger g_weblogger = {};

bool WebLogger_Init(const char *path)
{
    std::lock_guard<std::mutex> lock(g_weblogger.mutex);
    g_weblogger.file = NULL;
    g_weblogger.toFile = false;

    if (path) {
        g_weblogger.file = fopen(path, "a");
        if (g_weblogger.file) {
            g_weblogger.toFile = true;
            fprintf(stderr, "[webcon] Logging to file: %s\n", path);
            fflush(stderr);
            return true;
        } else {
            fprintf(stderr, "[webcon] WARNING: Failed to open log file: %s\n", path);
            fflush(stderr);
            return false;
        }
    }
    return true;
}

void WebLogger_Shutdown()
{
    std::lock_guard<std::mutex> lock(g_weblogger.mutex);
    if (g_weblogger.file) {
        fclose(g_weblogger.file);
        g_weblogger.file = NULL;
    }
    g_weblogger.toFile = false;
}

void WebLogger_Log(const char *fmt, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    std::lock_guard<std::mutex> lock(g_weblogger.mutex);

    fprintf(stderr, "[webcon] %s\n", buf);
    fflush(stderr);

    if (g_weblogger.toFile && g_weblogger.file) {
        fprintf(g_weblogger.file, "%s\n", buf);
        fflush(g_weblogger.file);
    }
}

void WebLogger_LogConn(MHD_Connection *conn, const char *fmt, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    const char *peer = "(unknown)";
    char peer_buf[64];
    if (conn) {
        auto info = MHD_get_connection_info(conn, MHD_CONNECTION_INFO_CLIENT_ADDRESS);
        if (info && info->client_addr) {
            // Use inet_ntop for thread-safe address formatting
            sockaddr_in *addr = (sockaddr_in *)info->client_addr;
            char ipbuf[INET_ADDRSTRLEN] = {0};
            if (inet_ntop(AF_INET, &addr->sin_addr, ipbuf, sizeof(ipbuf))) {
                snprintf(peer_buf, sizeof(peer_buf), "%s:%u", ipbuf, ntohs(addr->sin_port));
                peer = peer_buf;
            } else {
                snprintf(peer_buf, sizeof(peer_buf), "(addr-error):%u", ntohs(addr->sin_port));
                peer = peer_buf;
            }
        }
    }

    char full_buf[1400];
    snprintf(full_buf, sizeof(full_buf), "[%s] %s", peer, buf);

    std::lock_guard<std::mutex> lock(g_weblogger.mutex);

    fprintf(stderr, "[webcon] %s\n", full_buf);
    fflush(stderr);

    if (g_weblogger.toFile && g_weblogger.file) {
        fprintf(g_weblogger.file, "%s\n", full_buf);
        fflush(g_weblogger.file);
    }
}
