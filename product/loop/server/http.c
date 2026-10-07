#include "loop.h"
#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define HEADER_CAP 32768
#define WORKERS 4
#define QUEUE_CAP 16
#ifdef LOOP_TESTING
#define HTTP_IO_TIMEOUT_MS 1000
#else
#define HTTP_IO_TIMEOUT_MS 15000
#endif

typedef struct { int fd; int64_t deadline; } http_client;
typedef struct { loop_service *service; http_client clients[QUEUE_CAP]; size_t head, count; pthread_mutex_t mutex; pthread_cond_t ready; } http_pool;
static int64_t monotonic_ms(void) {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC,&now)) return -1;
    return (int64_t)now.tv_sec*1000 + now.tv_nsec/1000000;
}
static int remaining_timeout(int fd, int option, int64_t deadline) {
    int64_t now=monotonic_ms(), remaining=deadline-now;
    if (now<0 || remaining<=0) { errno=ETIMEDOUT; return 0; }
    struct timeval tv={.tv_sec=(time_t)(remaining/1000),.tv_usec=(suseconds_t)((remaining%1000)*1000)};
    return setsockopt(fd,SOL_SOCKET,option,&tv,sizeof(tv))==0;
}
static ssize_t receive(int fd, void *buffer, size_t size, int64_t deadline) {
    for (;;) {
        if (!remaining_timeout(fd,SO_RCVTIMEO,deadline)) return -1;
        ssize_t got=recv(fd,buffer,size,0);
        if (got<0 && errno==EINTR) continue;
        return got;
    }
}
static int send_all(int fd, const void *data, size_t n, int64_t deadline) {
    const unsigned char *p = data;
    while (n) {
        if (!remaining_timeout(fd,SO_SNDTIMEO,deadline)) return 0;
        ssize_t sent = send(fd, p, n, 0);
        if (sent < 0 && errno == EINTR) continue;
        if (sent <= 0) return 0;
        p += (size_t)sent; n -= (size_t)sent;
    }
    return 1;
}
static void respond(loop_service *s, int fd, loop_response *r, int64_t deadline) {
    char header[10240];
    char voice_origin[LOOP_ORIGIN_CAP]="";
    if (!loop_voice_origin(s,voice_origin)) voice_origin[0]=0;
    int n = snprintf(header, sizeof(header),
        "HTTP/1.1 %d Response\r\nContent-Type: %s\r\nContent-Length: %zu\r\nConnection: close\r\n"
        "Cache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\nReferrer-Policy: same-origin\r\n"
        "Permissions-Policy: microphone=(self), camera=(), geolocation=()\r\n"
        "Content-Security-Policy: default-src 'self'; script-src 'self' 'wasm-unsafe-eval'; worker-src 'self' blob:; "
        "connect-src 'self' %s; media-src 'self' blob:; object-src 'none'; base-uri 'self'; frame-ancestors 'none'\r\n"
        "%s\r\n", r->status, r->content_type, r->size, voice_origin, r->headers);
    if (n > 0 && (size_t)n < sizeof(header) && send_all(fd, header, (size_t)n, deadline)) (void)send_all(fd, r->body, r->size, deadline);
}
static int static_file(loop_service *s, const char *url, loop_response *r) {
    const char *path = !strcmp(url, "/") ? "/index.html" : url;
    const char *type = NULL;
    if (!strcmp(path, "/index.html")) type = "text/html; charset=utf-8";
    else if ((!strcmp(path, "/app.js") || !strcmp(path, "/mic-worklet.js"))) type = "text/javascript; charset=utf-8";
    else if (!strcmp(path, "/styles.css")) type = "text/css; charset=utf-8";
    else if (!strncmp(path, "/fonts/", 7) && !strstr(path, "..") && !strchr(path + 7, '/') &&
             strlen(path) > 6 && !strcmp(path + strlen(path) - 6, ".woff2")) type = "font/woff2";
    if (!type || !s->static_root) return loop_reply(r, 404, "{\"error\":\"not_found\"}");
    char full[2048]; int n = snprintf(full, sizeof(full), "%s%s", s->static_root, path);
    if (n < 0 || (size_t)n >= sizeof(full)) return loop_reply(r, 404, "{}");
    FILE *file = fopen(full, "rb");
    if (!file) return loop_reply(r, 404, "{\"error\":\"asset_not_built\"}");
    struct stat st;
    if (fstat(fileno(file), &st) || !S_ISREG(st.st_mode) || st.st_size < 0 || st.st_size > LOOP_REPLY_MAX) { fclose(file); return loop_reply(r, 500, "{}"); }
    r->size = (size_t)st.st_size; r->body = malloc(r->size + 1);
    if (!r->body) { fclose(file); return 0; }
    if (fread(r->body, 1, r->size, file) != r->size) { fclose(file); free(r->body); r->body = NULL; return loop_reply(r, 500, "{}"); }
    fclose(file); r->status = 200; snprintf(r->content_type, sizeof(r->content_type), "%s", type); return 1;
}
static void connection(loop_service *s, http_client client) {
    int fd=client.fd; int64_t deadline=client.deadline;
    char header[HEADER_CAP + 1]; size_t used = 0; char *end = NULL;
    loop_response r = {0};
    while (used < HEADER_CAP) {
        ssize_t got = receive(fd, header + used, HEADER_CAP - used, deadline);
        if (got <= 0) goto done;
        used += (size_t)got; header[used] = 0;
        end = strstr(header, "\r\n\r\n"); if (end) break;
    }
    if (!end) { loop_reply(&r, 431, "{\"error\":\"headers_too_large\"}"); goto done; }
    size_t header_len = (size_t)(end - header) + 4;
    if (memchr(header, 0, header_len)) goto malformed;
    char *line = strstr(header, "\r\n"); if (!line) goto malformed; *line = 0;
    char method[8], path[2048], version[16], excess;
    if (sscanf(header, "%7s %2047s %15s%c", method, path, version, &excess) != 3 || strcmp(version, "HTTP/1.1") || path[0] != '/' ||
        (strcmp(method, "GET") && strcmp(method, "POST"))) goto malformed;
    loop_request q = { .method = method, .path = path, .cookie = "", .content_type = "", .origin = "" };
    unsigned seen = 0; size_t body_len = 0;
    for (char *p = line + 2; p < end; ) {
        char *next = strstr(p, "\r\n"); if (!next) goto malformed; *next = 0;
        char *colon = strchr(p, ':'); if (!colon || colon == p) goto malformed;
        *colon = 0;
        for (char *c = p; *c; c++) if (!(isalnum((unsigned char)*c) || *c == '-')) goto malformed;
        char *value = colon + 1; while (*value == ' ' || *value == '\t') value++;
        for (char *c = value; *c; c++) if ((unsigned char)*c < 32 && *c != '\t') goto malformed;
        char *trim = value + strlen(value); while (trim > value && (trim[-1] == ' ' || trim[-1] == '\t')) *--trim = 0;
        unsigned bit = 0;
        if (!strcasecmp(p, "Content-Length")) {
            bit = 1;
            if (!*value) goto malformed;
            body_len = 0;
            for (char *c = value; *c; c++) { if (*c < '0' || *c > '9') goto malformed; body_len = body_len * 10 + (size_t)(*c - '0'); if (body_len > LOOP_BODY_MAX) { loop_reply(&r, 413, "{\"error\":\"body_too_large\"}"); goto done; } }
        } else if (!strcasecmp(p, "Cookie")) { bit = 2; q.cookie = value; }
        else if (!strcasecmp(p, "Origin")) { bit = 4; q.origin = value; }
        else if (!strcasecmp(p, "Content-Type")) { bit = 8; q.content_type = value; }
        else if (!strcasecmp(p,"X-Envoy-Oidc-Id-Token")) { bit=32; if (!*value || strlen(value)>CMP_GATEWAY_TOKEN_CAP) goto malformed; q.gateway_token=value; }
        else if (!strcasecmp(p, "Host")) { bit = 16; if (!*value) goto malformed; }
        else if (!strcasecmp(p, "Transfer-Encoding") || !strcasecmp(p, "Expect")) goto malformed;
        if (bit && (seen & bit)) goto malformed;
        seen |= bit; p = next + 2;
    }
    if (!(seen & 16) || (!strcmp(method, "POST") && !(seen & 1)) || (!strcmp(method, "GET") && body_len)) goto malformed;
    if (used - header_len > body_len) goto malformed;
    unsigned char *body = calloc(body_len + 1, 1); if (!body) goto done;
    size_t received = used - header_len; memcpy(body, header + header_len, received);
    while (received < body_len) {
        ssize_t got = receive(fd, body + received, body_len - received, deadline);
        if (got <= 0) { free(body); goto done; } received += (size_t)got;
    }
    q.body = body; q.body_len = body_len;
    if (!remaining_timeout(fd,SO_RCVTIMEO,deadline)) { free(body); goto done; }
    if (!strncmp(path, "/api/", 5) || !strcmp(path, "/healthz")) (void)loop_handle(s, &q, &r);
    else if (!strcmp(method, "GET")) (void)static_file(s, path, &r);
    else (void)loop_reply(&r, 404, "{\"error\":\"not_found\"}");
    free(body); goto done;
malformed:
    loop_reply(&r, 400, "{\"error\":\"malformed_request\"}");
done:
    if (r.body) respond(s, fd, &r, deadline);
    loop_response_free(&r); close(fd);
}
static void *worker(void *arg) {
    http_pool *pool = arg;
    for (;;) {
        pthread_mutex_lock(&pool->mutex);
        while (!pool->count) pthread_cond_wait(&pool->ready, &pool->mutex);
        http_client client = pool->clients[pool->head]; pool->head = (pool->head + 1) % QUEUE_CAP; pool->count--;
        pthread_mutex_unlock(&pool->mutex); connection(pool->service, client);
    }
    return NULL;
}
int loop_http_serve(loop_service *s, const char *bind_address, int port) {
    static http_pool pool; pool.service = s;
    int fd = socket(AF_INET, SOCK_STREAM, 0); if (fd < 0) return 0;
    int yes = 1; setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    struct sockaddr_in address = { .sin_family = AF_INET, .sin_port = htons((uint16_t)port) };
    if (inet_pton(AF_INET, bind_address, &address.sin_addr) != 1 || bind(fd, (struct sockaddr *)&address, sizeof(address)) || listen(fd, 32)) { close(fd); return 0; }
    if (pthread_mutex_init(&pool.mutex, NULL) || pthread_cond_init(&pool.ready, NULL)) { close(fd); return 0; }
    for (int i = 0; i < WORKERS; i++) { pthread_t thread; if (loop_thread_create(&thread, worker, &pool)) { close(fd); return 0; } pthread_detach(thread); }
    fprintf(stderr, "Loop listening on %s:%d\n", bind_address, port);
    for (;;) {
        int client = accept(fd, NULL, NULL); if (client < 0) { if (errno == EINTR) continue; return 0; }
        int64_t now=monotonic_ms(); if(now<0) { close(client); continue; }
        pthread_mutex_lock(&pool.mutex);
        if (pool.count == QUEUE_CAP) close(client);
        else { pool.clients[(pool.head + pool.count) % QUEUE_CAP] = (http_client){.fd=client,.deadline=now+HTTP_IO_TIMEOUT_MS}; pool.count++; pthread_cond_signal(&pool.ready); }
        pthread_mutex_unlock(&pool.mutex);
    }
}
