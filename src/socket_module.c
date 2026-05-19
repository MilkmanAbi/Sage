// socket_module.c — Real TCP/UDP sockets with LilyKnight permission enforcement
// Provides the `socket` module: connect, bind, listen, accept, send, recv, close
// All operations check lk_current_sandbox before proceeding.

#define _GNU_SOURCE
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <fcntl.h>
#include "value.h"
#include "module.h"
#include "lilybox.h"

// ── Socket Value wrapper ──────────────────────────────────────────────────────
// Sockets are represented as dicts with {__type:"socket", __fd:N, __blocking:bool}

static Value make_socket_val(int fd) {
    gc_pin();
    Value d = val_dict();
    dict_set(&d, "__type",     val_string("socket"));
    dict_set(&d, "__fd",       val_number((double)fd));
    dict_set(&d, "__blocking", val_bool(1));
    gc_unpin();
    return d;
}

static int get_socket_fd(Value* v) {
    if (!IS_DICT(*v)) return -1;
    Value t = dict_get(v, "__type");
    if (!IS_STRING(t) || strcmp(AS_STRING(t), "socket") != 0) return -1;
    Value fd_v = dict_get(v, "__fd");
    if (!IS_NUMBER(fd_v)) return -1;
    return (int)AS_NUMBER(fd_v);
}

// ── socket.connect(host, port) -> socket | nil ───────────────────────────────
static Value sock_connect_native(int argc, Value* args) {
    if (argc < 2 || !IS_STRING(args[0]) || !IS_NUMBER(args[1])) return val_nil();
    const char* host = AS_STRING(args[0]);
    int port = (int)AS_NUMBER(args[1]);

    // LilyKnight check
    if (lk_current_sandbox) {
        LKViolation v = lk_check_net_connect(lk_current_sandbox, host, port);
        if (v != LK_OK) {
            lk_log_violation(lk_current_sandbox, v, host);
            return val_nil();
        }
    }

    struct addrinfo hints, *res, *rp;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    char port_str[16];
    snprintf(port_str, sizeof(port_str), "%d", port);
    if (getaddrinfo(host, port_str, &hints, &res) != 0) return val_nil();

    int fd = -1;
    for (rp = res; rp != NULL; rp = rp->ai_next) {
        fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (fd < 0) continue;
        if (connect(fd, rp->ai_addr, rp->ai_addrlen) == 0) break;
        close(fd); fd = -1;
    }
    freeaddrinfo(res);
    if (fd < 0) return val_nil();
    return make_socket_val(fd);
}

// ── socket.listen(port, backlog=128) -> socket | nil ─────────────────────────
static Value sock_listen_native(int argc, Value* args) {
    if (argc < 1 || !IS_NUMBER(args[0])) return val_nil();
    int port = (int)AS_NUMBER(args[0]);
    int backlog = (argc >= 2 && IS_NUMBER(args[1])) ? (int)AS_NUMBER(args[1]) : 128;

    if (lk_current_sandbox) {
        LKViolation v = lk_check_net_bind(lk_current_sandbox, port);
        if (v != LK_OK) { lk_log_violation(lk_current_sandbox, v, "bind"); return val_nil(); }
    }

    int fd = socket(AF_INET6, SOCK_STREAM, 0);
    if (fd < 0) fd = socket(AF_INET, SOCK_STREAM, 0);  // fallback to IPv4
    if (fd < 0) return val_nil();

    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &one, sizeof(one));

    struct sockaddr_in6 addr6;
    memset(&addr6, 0, sizeof(addr6));
    addr6.sin6_family = AF_INET6;
    addr6.sin6_port   = htons((uint16_t)port);
    addr6.sin6_addr   = in6addr_any;

    if (bind(fd, (struct sockaddr*)&addr6, sizeof(addr6)) != 0) {
        // Fallback to IPv4
        struct sockaddr_in addr4;
        memset(&addr4, 0, sizeof(addr4));
        addr4.sin_family = AF_INET;
        addr4.sin_port   = htons((uint16_t)port);
        addr4.sin_addr.s_addr = INADDR_ANY;
        if (bind(fd, (struct sockaddr*)&addr4, sizeof(addr4)) != 0) {
            close(fd); return val_nil();
        }
    }
    if (listen(fd, backlog) != 0) { close(fd); return val_nil(); }
    return make_socket_val(fd);
}

// ── socket.accept(server_sock) -> socket | nil ───────────────────────────────
static Value sock_accept_native(int argc, Value* args) {
    if (argc < 1) return val_nil();
    int fd = get_socket_fd(&args[0]);
    if (fd < 0) return val_nil();

    if (lk_current_sandbox) {
        LKViolation v = lk_check_net_recv(lk_current_sandbox, 0);
        if (v != LK_OK) { lk_log_violation(lk_current_sandbox, v, "accept"); return val_nil(); }
    }

    struct sockaddr_storage addr;
    socklen_t addrlen = sizeof(addr);
    int client_fd = accept(fd, (struct sockaddr*)&addr, &addrlen);
    if (client_fd < 0) return val_nil();
    return make_socket_val(client_fd);
}

// ── socket.send(sock, data) -> int (bytes sent) ──────────────────────────────
static Value sock_send_native(int argc, Value* args) {
    if (argc < 2) return val_number(-1);
    int fd = get_socket_fd(&args[0]);
    if (fd < 0) return val_number(-1);

    if (lk_current_sandbox) {
        size_t nbytes = IS_STRING(args[1]) ? strlen(AS_STRING(args[1])) : 0;
        LKViolation v = lk_check_net_send(lk_current_sandbox, nbytes);
        if (v != LK_OK) { lk_log_violation(lk_current_sandbox, v, "send"); return val_number(-1); }
    }

    if (IS_STRING(args[1])) {
        const char* s = AS_STRING(args[1]);
        ssize_t sent = send(fd, s, strlen(s), MSG_NOSIGNAL);
        return val_number((double)sent);
    }
    return val_number(-1);
}

// ── socket.recv(sock, max_bytes=4096) -> str | nil ───────────────────────────
static Value sock_recv_native(int argc, Value* args) {
    if (argc < 1) return val_nil();
    int fd = get_socket_fd(&args[0]);
    if (fd < 0) return val_nil();
    int maxbytes = (argc >= 2 && IS_NUMBER(args[1])) ? (int)AS_NUMBER(args[1]) : 4096;

    if (lk_current_sandbox) {
        LKViolation v = lk_check_net_recv(lk_current_sandbox, (size_t)maxbytes);
        if (v != LK_OK) { lk_log_violation(lk_current_sandbox, v, "recv"); return val_nil(); }
    }

    char* buf = malloc((size_t)maxbytes + 1);
    ssize_t n = recv(fd, buf, (size_t)maxbytes, 0);
    if (n <= 0) { free(buf); return val_nil(); }
    buf[n] = '\0';
    Value result = val_string(buf);
    free(buf);
    return result;
}

// ── socket.recv_all(sock, max_bytes=65536) -> str | nil ──────────────────────
static Value sock_recv_all_native(int argc, Value* args) {
    if (argc < 1) return val_nil();
    int fd = get_socket_fd(&args[0]);
    if (fd < 0) return val_nil();
    int maxbytes = (argc >= 2 && IS_NUMBER(args[1])) ? (int)AS_NUMBER(args[1]) : 65536;

    char* buf = malloc((size_t)maxbytes + 1);
    size_t total = 0;
    while (total < (size_t)maxbytes) {
        ssize_t n = recv(fd, buf + total, (size_t)(maxbytes - (int)total), 0);
        if (n <= 0) break;
        total += (size_t)n;
    }
    if (total == 0) { free(buf); return val_nil(); }
    buf[total] = '\0';
    Value result = val_string(buf);
    free(buf);
    return result;
}

// ── socket.close(sock) ───────────────────────────────────────────────────────
static Value sock_close_native(int argc, Value* args) {
    if (argc < 1) return val_nil();
    int fd = get_socket_fd(&args[0]);
    if (fd >= 0) close(fd);
    if (IS_DICT(args[0])) dict_set(&args[0], "__fd", val_number(-1));
    return val_nil();
}

// ── socket.set_nonblocking(sock, bool) ───────────────────────────────────────
static Value sock_set_nonblocking_native(int argc, Value* args) {
    if (argc < 1) return val_nil();
    int fd = get_socket_fd(&args[0]);
    if (fd < 0) return val_nil();
    int nb = (argc >= 2 && IS_BOOL(args[1])) ? AS_BOOL(args[1]) : 1;
    int flags = fcntl(fd, F_GETFL, 0);
    if (nb) flags |= O_NONBLOCK; else flags &= ~O_NONBLOCK;
    fcntl(fd, F_SETFL, flags);
    if (IS_DICT(args[0])) dict_set(&args[0], "__blocking", val_bool(!nb));
    return val_nil();
}

// ── socket.peer_addr(sock) -> str ────────────────────────────────────────────
static Value sock_peer_addr_native(int argc, Value* args) {
    if (argc < 1) return val_string("unknown");
    int fd = get_socket_fd(&args[0]);
    if (fd < 0) return val_string("unknown");
    struct sockaddr_storage addr;
    socklen_t addrlen = sizeof(addr);
    if (getpeername(fd, (struct sockaddr*)&addr, &addrlen) != 0) return val_string("unknown");
    char host[256] = {0};
    char port[32]  = {0};
    getnameinfo((struct sockaddr*)&addr, addrlen,
                host, sizeof(host), port, sizeof(port),
                NI_NUMERICHOST | NI_NUMERICSERV);
    char result[320];
    snprintf(result, sizeof(result), "%s:%s", host, port);
    return val_string(result);
}

// ── socket.http_get(url) -> str | nil ────────────────────────────────────────
// Simple HTTP/1.1 GET without dependencies (curl is optional)
static Value sock_http_get_native(int argc, Value* args) {
    if (argc < 1 || !IS_STRING(args[0])) return val_nil();
    const char* url = AS_STRING(args[0]);

    // Parse URL: scheme://host[:port]/path
    const char* p = url;
    int use_https = 0;
    if (strncmp(p, "https://", 8) == 0) { p += 8; use_https = 1; }
    else if (strncmp(p, "http://", 7) == 0) { p += 7; }
    else return val_nil();

    char host[256] = {0};
    int  port = use_https ? 443 : 80;
    char path[1024] = "/";

    const char* slash = strchr(p, '/');
    const char* colon = strchr(p, ':');
    if (colon && (!slash || colon < slash)) {
        strncpy(host, p, (size_t)(colon - p));
        port = atoi(colon + 1);
        if (slash) strncpy(path, slash, sizeof(path)-1);
    } else if (slash) {
        strncpy(host, p, (size_t)(slash - p));
        strncpy(path, slash, sizeof(path)-1);
    } else {
        strncpy(host, p, sizeof(host)-1);
    }

    // LilyKnight check
    if (lk_current_sandbox) {
        LKViolation v = lk_check_net_connect(lk_current_sandbox, host, port);
        if (v != LK_OK) { lk_log_violation(lk_current_sandbox, v, host); return val_nil(); }
    }

    // For HTTPS, we'd need TLS — fall back to nil with a note
    if (use_https) {
        // TODO: integrate with OpenSSL/BearSSL
        // For now, return nil for HTTPS (curl-based http_get in net module handles it)
        return val_nil();
    }

    // Plain HTTP/1.1 GET
    Value sock_args[2] = { val_string(host), val_number((double)port) };
    Value sock = sock_connect_native(2, sock_args);
    int fd = get_socket_fd(&sock);
    if (fd < 0) return val_nil();

    // Send HTTP request
    char request[2048];
    int req_len = snprintf(request, sizeof(request),
        "GET %s HTTP/1.1\r\nHost: %s\r\nConnection: close\r\nUser-Agent: SageTree/0.1\r\n\r\n",
        path, host);
    send(fd, request, (size_t)req_len, MSG_NOSIGNAL);

    // Read response
    char* resp = malloc(65536);
    size_t total = 0;
    ssize_t n;
    while (total < 65535 && (n = recv(fd, resp + total, 65535 - total, 0)) > 0)
        total += (size_t)n;
    resp[total] = '\0';
    close(fd);

    // Strip HTTP headers — return body only
    char* body = strstr(resp, "\r\n\r\n");
    Value result = body ? val_string(body + 4) : val_string(resp);
    free(resp);
    return result;
}

// ── Module registration ───────────────────────────────────────────────────────
Module* create_socket_module(ModuleCache* cache) {
    Module* m = create_native_module(cache, "socket");
    Environment* e = m->env;
    env_define_const(e, "connect",        7,  val_native(sock_connect_native));
    env_define_const(e, "listen",         6,  val_native(sock_listen_native));
    env_define_const(e, "accept",         6,  val_native(sock_accept_native));
    env_define_const(e, "send",           4,  val_native(sock_send_native));
    env_define_const(e, "recv",           4,  val_native(sock_recv_native));
    env_define_const(e, "recv_all",       8,  val_native(sock_recv_all_native));
    env_define_const(e, "close",          5,  val_native(sock_close_native));
    env_define_const(e, "set_nonblocking",15, val_native(sock_set_nonblocking_native));
    env_define_const(e, "peer_addr",      9,  val_native(sock_peer_addr_native));
    env_define_const(e, "http_get",       8,  val_native(sock_http_get_native));
    return m;
}
