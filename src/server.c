/*
 * VitaDrop web server.
 *
 * A tiny HTTP server with two pages:
 *   GET  /                  -> the upload page (web/index.html)
 *   POST /upload?name=FILE  -> the request body is the file; it is saved to disk
 *
 * It only uses standard C and POSIX sockets, so the same code runs on the
 * Vita (built with VitaSDK) and on a normal computer (for testing).
 */
#include "server.h"
#include "page_html.h"

#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <unistd.h>

#ifdef __vita__
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#else
#include <pthread.h>
#endif

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

#define HEADER_MAX   8192
#define CHUNK_SIZE   (256 * 1024)   /* read/write the file 256 KB at a time */

static char g_dir[256];
static int g_port;
static int g_listen_fd = -1;
static volatile int g_keep_running = 0;
static ServerStatus g_status;

/* ---------- small helpers ---------- */

static void set_message(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    vsnprintf(g_status.message, sizeof(g_status.message), fmt, args);
    va_end(args);
}

/* Stops the Vita from going to sleep while a file is arriving. */
static void keep_awake(void)
{
#ifdef __vita__
    sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DISABLE_AUTO_SUSPEND);
#endif
}

static int send_all(int fd, const void *data, size_t len)
{
    const char *p = (const char *)data;
    while (len > 0) {
        ssize_t n = send(fd, p, len, MSG_NOSIGNAL);
        if (n <= 0) return -1;
        p += n;
        len -= (size_t)n;
    }
    return 0;
}

static void send_response(int fd, const char *status, const char *type,
                          const void *body, size_t body_len)
{
    char head[256];
    int n = snprintf(head, sizeof(head),
                     "HTTP/1.1 %s\r\n"
                     "Content-Type: %s\r\n"
                     "Content-Length: %lu\r\n"
                     "Cache-Control: no-store\r\n"
                     "Connection: close\r\n\r\n",
                     status, type, (unsigned long)body_len);
    if (send_all(fd, head, (size_t)n) == 0 && body_len > 0)
        send_all(fd, body, body_len);
}

static void send_text(int fd, const char *status, const char *text)
{
    send_response(fd, status, "text/plain; charset=utf-8", text, strlen(text));
}

static int hex_value(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Finds ?key=value in a URL path and decodes %XX and + characters. */
static int get_query_value(const char *path, const char *key, char *out, size_t out_size)
{
    const char *q = strchr(path, '?');
    size_t key_len = strlen(key);
    if (!q) return 0;
    q++;
    while (*q) {
        if (strncmp(q, key, key_len) == 0 && q[key_len] == '=') {
            const char *v = q + key_len + 1;
            size_t o = 0;
            while (*v && *v != '&' && o + 1 < out_size) {
                if (*v == '%' && hex_value(v[1]) >= 0 && hex_value(v[2]) >= 0) {
                    out[o++] = (char)(hex_value(v[1]) * 16 + hex_value(v[2]));
                    v += 3;
                } else if (*v == '+') {
                    out[o++] = ' ';
                    v++;
                } else {
                    out[o++] = *v++;
                }
            }
            out[o] = '\0';
            return 1;
        }
        q = strchr(q, '&');
        if (!q) break;
        q++;
    }
    return 0;
}

/* Makes a file name safe: no folders, no characters the Vita can't store. */
static void clean_file_name(char *name)
{
    char *start = name;
    for (char *p = name; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (c < 32 || strchr("/\\:*?\"<>|", c)) *p = '_';
    }
    while (*start == '.' || *start == ' ') start++;     /* no hidden files or ".." */
    if (start != name) memmove(name, start, strlen(start) + 1);
    if (name[0] == '\0') strcpy(name, "upload.bin");
}

static int file_exists(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0;
}

/* If "photo.jpg" already exists, use "photo (1).jpg", "photo (2).jpg", ... */
static void pick_free_path(const char *name, char *out_path, size_t out_size,
                           char *out_name, size_t out_name_size)
{
    char base[160], ext[40] = "";
    const char *dot = strrchr(name, '.');
    if (dot && dot != name && strlen(dot) < sizeof(ext)) {
        snprintf(base, sizeof(base), "%.*s", (int)(dot - name), name);
        snprintf(ext, sizeof(ext), "%s", dot);
    } else {
        snprintf(base, sizeof(base), "%s", name);
    }

    snprintf(out_name, out_name_size, "%s%s", base, ext);
    snprintf(out_path, out_size, "%s/%s", g_dir, out_name);
    for (int i = 1; file_exists(out_path) && i < 10000; i++) {
        snprintf(out_name, out_name_size, "%s (%d)%s", base, i, ext);
        snprintf(out_path, out_size, "%s/%s", g_dir, out_name);
    }
}

/* Reads the Content-Length header. Returns -1 if it isn't there. */
static long long get_content_length(const char *headers)
{
    char lower[HEADER_MAX];
    size_t i;
    for (i = 0; headers[i] && i + 1 < sizeof(lower); i++)
        lower[i] = (char)tolower((unsigned char)headers[i]);
    lower[i] = '\0';

    const char *p = strstr(lower, "\r\ncontent-length:");
    if (!p) return -1;
    p += strlen("\r\ncontent-length:");
    while (*p == ' ' || *p == '\t') p++;
    if (!isdigit((unsigned char)*p)) return -1;
    return strtoll(p, NULL, 10);
}

/* ---------- the upload itself ---------- */

static void handle_upload(int fd, const char *path, long long length,
                          const char *already, size_t already_len)
{
    char name[200], file_path[512], saved_name[220];

    if (length < 0) { send_text(fd, "411 Length Required", "Missing file size"); return; }
    if (!get_query_value(path, "name", name, sizeof(name))) strcpy(name, "upload.bin");
    clean_file_name(name);
    pick_free_path(name, file_path, sizeof(file_path), saved_name, sizeof(saved_name));

    FILE *f = fopen(file_path, "wb");
    if (!f) {
        set_message("Could not create %s", saved_name);
        send_text(fd, "500 Internal Server Error", "Could not create the file on the Vita");
        return;
    }

    char *buffer = (char *)malloc(CHUNK_SIZE);
    if (!buffer) {
        fclose(f);
        remove(file_path);
        send_text(fd, "500 Internal Server Error", "Out of memory");
        return;
    }

    snprintf(g_status.current_name, sizeof(g_status.current_name), "%.127s", saved_name);
    g_status.current_total = (uint64_t)length;
    g_status.current_done = 0;
    g_status.busy = 1;
    set_message("Receiving %s", saved_name);

    unsigned long long remaining = (unsigned long long)length;
    int ok = 1;

    /* Part of the file may have arrived together with the headers. */
    if (already_len > remaining) already_len = (size_t)remaining;
    if (already_len > 0) {
        if (fwrite(already, 1, already_len, f) != already_len) ok = 0;
        remaining -= already_len;
        g_status.current_done += already_len;
    }

    while (ok && remaining > 0) {
        size_t want = remaining < CHUNK_SIZE ? (size_t)remaining : CHUNK_SIZE;
        ssize_t n = recv(fd, buffer, want, 0);
        if (n <= 0) { ok = 0; break; }              /* phone disconnected */
        if (fwrite(buffer, 1, (size_t)n, f) != (size_t)n) { ok = 0; break; }  /* disk full? */
        remaining -= (unsigned long long)n;
        g_status.current_done += (uint64_t)n;
        keep_awake();
    }

    free(buffer);
    if (fclose(f) != 0) ok = 0;
    g_status.busy = 0;

    if (!ok) {
        remove(file_path);                           /* don't leave half a file */
        set_message("Upload of %s failed", saved_name);
        send_text(fd, "500 Internal Server Error", "Upload failed (connection lost or memory card full)");
        return;
    }

    g_status.files_received++;
    g_status.bytes_received += (uint64_t)length;
    set_message("Saved %s", saved_name);

    char reply[320];
    snprintf(reply, sizeof(reply), "{\"ok\":true,\"saved\":\"%s\"}", saved_name);
    send_response(fd, "200 OK", "application/json", reply, strlen(reply));
}

/* ---------- one browser request ---------- */

static void handle_client(int fd)
{
    char head[HEADER_MAX];
    size_t got = 0;
    char *end = NULL;

    /* Read until the blank line that ends the headers. */
    while (got < sizeof(head) - 1) {
        ssize_t n = recv(fd, head + got, sizeof(head) - 1 - got, 0);
        if (n <= 0) return;
        got += (size_t)n;
        head[got] = '\0';
        end = strstr(head, "\r\n\r\n");
        if (end) break;
    }
    if (!end) { send_text(fd, "431 Request Header Fields Too Large", "Headers too large"); return; }

    size_t header_len = (size_t)(end - head) + 4;
    char method[8] = "", path[1024] = "";
    if (sscanf(head, "%7s %1023s", method, path) != 2) {
        send_text(fd, "400 Bad Request", "Bad request");
        return;
    }

    end[2] = '\0';   /* cut the text so header parsing stops before the file data */
    long long length = get_content_length(head);

    if (strcmp(method, "GET") == 0 && (strcmp(path, "/") == 0 || strncmp(path, "/?", 2) == 0)) {
        send_response(fd, "200 OK", "text/html; charset=utf-8", PAGE_HTML, PAGE_HTML_LEN);
    } else if (strcmp(method, "POST") == 0 && strncmp(path, "/upload", 7) == 0) {
        handle_upload(fd, path, length, head + header_len, got - header_len);
    } else {
        send_text(fd, "404 Not Found", "Not found");
    }
}

/* ---------- the background loop ---------- */

static void server_loop(void)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { set_message("Could not open network socket"); return; }

    int yes = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((unsigned short)g_port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0 || listen(fd, 4) < 0) {
        set_message("Port %d is busy", g_port);
        close(fd);
        return;
    }

    g_listen_fd = fd;
    g_status.running = 1;
    set_message("Ready. Scan the QR code.");

    while (g_keep_running) {
        int client = accept(fd, NULL, NULL);
        if (client < 0) {
            if (!g_keep_running) break;
            continue;
        }
        /* If the phone goes quiet for 30 seconds, give up on it. */
        struct timeval tv = { 30, 0 };
        setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

        handle_client(client);
        close(client);
    }

    g_status.running = 0;
}

#ifdef __vita__
static int server_thread(SceSize args, void *argp)
{
    (void)args; (void)argp;
    server_loop();
    return sceKernelExitDeleteThread(0);
}
#else
static void *server_thread(void *arg)
{
    (void)arg;
    server_loop();
    return NULL;
}
#endif

int server_start(const char *save_dir, int port)
{
    snprintf(g_dir, sizeof(g_dir), "%s", save_dir);
    g_port = port;
    g_keep_running = 1;
    memset(&g_status, 0, sizeof(g_status));
    set_message("Starting...");

    mkdir(g_dir, 0777);   /* fine if it already exists */

#ifdef __vita__
    SceUID thid = sceKernelCreateThread("vitadrop_server", server_thread,
                                        0x10000100, 0x10000, 0, 0, NULL);
    if (thid < 0) return -1;
    sceKernelStartThread(thid, 0, NULL);
#else
    pthread_t t;
    if (pthread_create(&t, NULL, server_thread, NULL) != 0) return -1;
    pthread_detach(t);
#endif
    return 0;
}

void server_stop(void)
{
    g_keep_running = 0;
    if (g_listen_fd >= 0) {
        shutdown(g_listen_fd, SHUT_RDWR);
        close(g_listen_fd);
        g_listen_fd = -1;
    }
}

void server_get_status(ServerStatus *out)
{
    *out = g_status;
}
