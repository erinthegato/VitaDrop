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
#include <strings.h>
#include <time.h>
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

/*
 * Finds a header like "Content-Type: image/jpeg" (name is matched without
 * caring about upper/lower case) and copies its value. Returns 1 if found.
 */
static int get_header(const char *headers, const char *name, char *out, size_t out_size)
{
    size_t name_len = strlen(name);
    const char *line = strstr(headers, "\r\n");
    while (line) {
        line += 2;
        if (strncasecmp(line, name, name_len) == 0 && line[name_len] == ':') {
            const char *v = line + name_len + 1;
            while (*v == ' ' || *v == '\t') v++;
            size_t o = 0;
            while (v[o] && v[o] != '\r' && o + 1 < out_size) { out[o] = v[o]; o++; }
            out[o] = '\0';
            return 1;
        }
        line = strstr(line, "\r\n");
    }
    return 0;
}

/* Decodes %XX sequences in place (for names sent in a header). */
static void percent_decode(char *s)
{
    char *o = s;
    for (char *p = s; *p; ) {
        if (*p == '%' && hex_value(p[1]) >= 0 && hex_value(p[2]) >= 0) {
            *o++ = (char)(hex_value(p[1]) * 16 + hex_value(p[2]));
            p += 3;
        } else {
            *o++ = *p++;
        }
    }
    *o = '\0';
}

/* ---------- reading the request body ---------- */

/* Gives back the bytes that arrived with the headers first, then reads more. */
typedef struct {
    int fd;
    const char *pending;
    size_t pending_len;
} Reader;

static ssize_t reader_read(Reader *r, char *buf, size_t n)
{
    if (r->pending_len > 0) {
        size_t k = n < r->pending_len ? n : r->pending_len;
        memcpy(buf, r->pending, k);
        r->pending += k;
        r->pending_len -= k;
        return (ssize_t)k;
    }
    return recv(r->fd, buf, n, 0);
}

static int reader_exact(Reader *r, char *buf, size_t n)
{
    while (n > 0) {
        ssize_t got = reader_read(r, buf, n);
        if (got <= 0) return -1;
        buf += got;
        n -= (size_t)got;
    }
    return 0;
}

/* Reads one line ending in \n. Returns its length, or -1 on error. */
static int reader_line(Reader *r, char *buf, size_t max)
{
    size_t o = 0;
    for (;;) {
        char c;
        if (reader_read(r, &c, 1) != 1) return -1;
        if (o + 1 < max) buf[o++] = c;
        if (c == '\n') break;
    }
    buf[o] = '\0';
    return (int)o;
}

/* ---------- naming the file ---------- */

static int has_extension(const char *name)
{
    const char *dot = strrchr(name, '.');
    return dot && dot != name && dot[1] != '\0';
}

/* Guesses an extension from the Content-Type header. */
static const char *extension_from_type(const char *type)
{
    static const struct { const char *type, *ext; } map[] = {
        { "image/jpeg", ".jpg" }, { "image/heic", ".heic" }, { "image/heif", ".heif" },
        { "image/png", ".png" }, { "image/gif", ".gif" }, { "image/webp", ".webp" },
        { "video/quicktime", ".mov" }, { "video/mp4", ".mp4" }, { "audio/mpeg", ".mp3" },
        { "audio/mp4", ".m4a" }, { "audio/x-m4a", ".m4a" }, { "audio/wav", ".wav" },
        { "application/pdf", ".pdf" }, { "text/plain", ".txt" }, { "application/zip", ".zip" },
    };
    for (unsigned i = 0; i < sizeof(map) / sizeof(map[0]); i++)
        if (strncasecmp(type, map[i].type, strlen(map[i].type)) == 0) return map[i].ext;
    return NULL;
}

/* Guesses an extension from the first bytes of the file. */
static const char *extension_from_bytes(const unsigned char *b, size_t n)
{
    if (n >= 3 && b[0] == 0xFF && b[1] == 0xD8 && b[2] == 0xFF) return ".jpg";
    if (n >= 4 && memcmp(b, "\x89PNG", 4) == 0) return ".png";
    if (n >= 4 && memcmp(b, "GIF8", 4) == 0) return ".gif";
    if (n >= 4 && memcmp(b, "%PDF", 4) == 0) return ".pdf";
    if (n >= 4 && memcmp(b, "PK\x03\x04", 4) == 0) return ".zip";
    if (n >= 3 && memcmp(b, "ID3", 3) == 0) return ".mp3";
    if (n >= 12 && memcmp(b, "RIFF", 4) == 0 && memcmp(b + 8, "WAVE", 4) == 0) return ".wav";
    if (n >= 12 && memcmp(b + 4, "ftyp", 4) == 0) {
        if (memcmp(b + 8, "hei", 3) == 0 || memcmp(b + 8, "mif1", 4) == 0) return ".heic";
        if (memcmp(b + 8, "qt  ", 4) == 0) return ".mov";
        if (memcmp(b + 8, "M4A ", 4) == 0) return ".m4a";
        return ".mp4";
    }
    return ".bin";
}

static void default_name(char *out, size_t size)
{
    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    if (t)
        snprintf(out, size, "VitaDrop_%04d%02d%02d_%02d%02d%02d",
                 t->tm_year + 1900, t->tm_mon + 1, t->tm_mday, t->tm_hour, t->tm_min, t->tm_sec);
    else
        snprintf(out, size, "VitaDrop_upload");
}

/* ---------- the upload itself ---------- */

/*
 * The file can arrive two ways:
 *   - with a Content-Length header (browsers do this), or
 *   - "chunked", in pieces with sizes in between (iOS Shortcuts may do this).
 * It is written to a temporary file first and renamed once complete, so a
 * failed upload never leaves half a file behind.
 *
 * The file name comes from ?name=... in the address, or an X-File-Name header.
 * If it has no extension, one is guessed from the Content-Type or the bytes.
 */
static void handle_upload(int fd, const char *path, const char *headers,
                          const char *already, size_t already_len)
{
    char name[200] = "", value[128];
    char temp_path[300], file_path[512], saved_name[220];
    unsigned char first_bytes[16];
    size_t first_len = 0;
    uint64_t received = 0;
    int ok = 1;

    long long length = -1;
    if (get_header(headers, "Content-Length", value, sizeof(value)))
        length = strtoll(value, NULL, 10);
    int chunked = get_header(headers, "Transfer-Encoding", value, sizeof(value)) &&
                  strncasecmp(value, "chunked", 7) == 0;
    if (!chunked && length < 0) {
        send_text(fd, "411 Length Required", "Missing file size");
        return;
    }

    if (!get_query_value(path, "name", name, sizeof(name)) || !name[0]) {
        if (get_header(headers, "X-File-Name", name, sizeof(name))) percent_decode(name);
    }
    if (!name[0]) default_name(name, sizeof(name));
    clean_file_name(name);

    snprintf(temp_path, sizeof(temp_path), "%s/.incoming.tmp", g_dir);
    FILE *f = fopen(temp_path, "wb");
    if (!f) {
        set_message("Could not write to %s", g_dir);
        send_text(fd, "500 Internal Server Error", "Could not create the file on the Vita");
        return;
    }
    char *buffer = (char *)malloc(CHUNK_SIZE);
    if (!buffer) {
        fclose(f);
        remove(temp_path);
        send_text(fd, "500 Internal Server Error", "Out of memory");
        return;
    }

    snprintf(g_status.current_name, sizeof(g_status.current_name), "%.127s", name);
    g_status.current_total = length > 0 ? (uint64_t)length : 0;
    g_status.current_done = 0;
    g_status.busy = 1;
    set_message("Receiving %.100s", name);

    Reader r = { fd, already, already_len };

#define SAVE_BYTES(data, count) do {                                          \
        size_t _n = (count);                                                  \
        if (first_len < sizeof(first_bytes)) {                                \
            size_t _k = sizeof(first_bytes) - first_len;                      \
            if (_k > _n) _k = _n;                                             \
            memcpy(first_bytes + first_len, (data), _k);                      \
            first_len += _k;                                                  \
        }                                                                     \
        if (fwrite((data), 1, _n, f) != _n) { ok = 0; }                       \
        received += _n;                                                       \
        g_status.current_done = received;                                     \
        keep_awake();                                                         \
    } while (0)

    if (chunked) {
        char line[64];
        while (ok) {
            if (reader_line(&r, line, sizeof(line)) < 0) { ok = 0; break; }
            unsigned long long size = strtoull(line, NULL, 16);
            if (size == 0) {
                /* end of the file: skip any trailing headers up to the blank line */
                while (reader_line(&r, line, sizeof(line)) > 2) {}
                break;
            }
            while (ok && size > 0) {
                size_t want = size < CHUNK_SIZE ? (size_t)size : CHUNK_SIZE;
                ssize_t n = reader_read(&r, buffer, want);
                if (n <= 0) { ok = 0; break; }
                SAVE_BYTES(buffer, (size_t)n);
                size -= (unsigned long long)n;
            }
            char crlf[2];
            if (ok && reader_exact(&r, crlf, 2) < 0) ok = 0;
        }
    } else {
        unsigned long long remaining = (unsigned long long)length;
        while (ok && remaining > 0) {
            size_t want = remaining < CHUNK_SIZE ? (size_t)remaining : CHUNK_SIZE;
            ssize_t n = reader_read(&r, buffer, want);
            if (n <= 0) { ok = 0; break; }        /* phone disconnected */
            SAVE_BYTES(buffer, (size_t)n);
            remaining -= (unsigned long long)n;
        }
    }
#undef SAVE_BYTES

    free(buffer);
    if (fclose(f) != 0) ok = 0;
    g_status.busy = 0;

    if (!ok) {
        remove(temp_path);                         /* don't leave half a file */
        set_message("Upload of %.100s failed", name);
        send_text(fd, "500 Internal Server Error", "Upload failed (connection lost or memory card full)");
        return;
    }

    /* Add an extension if the name doesn't have one. */
    if (!has_extension(name)) {
        const char *ext = NULL;
        if (get_header(headers, "Content-Type", value, sizeof(value))) ext = extension_from_type(value);
        if (!ext) ext = extension_from_bytes(first_bytes, first_len);
        strncat(name, ext, sizeof(name) - strlen(name) - 1);
    }

    pick_free_path(name, file_path, sizeof(file_path), saved_name, sizeof(saved_name));
    if (rename(temp_path, file_path) != 0) {
        remove(temp_path);
        set_message("Could not save %.100s", saved_name);
        send_text(fd, "500 Internal Server Error", "Could not save the file");
        return;
    }

    g_status.files_received++;
    g_status.bytes_received += received;
    set_message("Saved %.100s", saved_name);

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

    if (strcmp(method, "GET") == 0 && (strcmp(path, "/") == 0 || strncmp(path, "/?", 2) == 0)) {
        send_response(fd, "200 OK", "text/html; charset=utf-8", PAGE_HTML, PAGE_HTML_LEN);
    } else if (strcmp(method, "POST") == 0 && strncmp(path, "/upload", 7) == 0) {
        handle_upload(fd, path, head, head + header_len, got - header_len);
    } else {
        send_text(fd, "404 Not Found", "Not found");
    }
}

/* ---------- the background loop ---------- */

static void sleep_ms(int ms)
{
#ifdef __vita__
    sceKernelDelayThread((SceUInt)ms * 1000);
#else
    usleep((useconds_t)ms * 1000);
#endif
}

/* Opens the listening socket. Returns -1 if the network isn't ready yet. */
static int open_listener(void)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    int yes = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((unsigned short)g_port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0 || listen(fd, 4) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static void server_loop(void)
{
    /*
     * Outer loop: (re)open the listening socket. When the Vita goes into real
     * sleep and wakes up, Wi-Fi reconnects and the old socket stops working,
     * so we simply open a fresh one.
     */
    while (g_keep_running) {
        int fd = open_listener();
        if (fd < 0) {
            g_status.running = 0;
            set_message("Waiting for the network...");
            sleep_ms(2000);
            continue;
        }

        g_listen_fd = fd;
        g_status.running = 1;
        set_message("Ready. Scan the QR code.");

        int failures = 0;
        while (g_keep_running) {
            int client = accept(fd, NULL, NULL);
            if (client < 0) {
                if (!g_keep_running) break;
                if (errno == EINTR) continue;
                if (++failures >= 3) break;      /* socket is dead; reopen it */
                sleep_ms(500);
                continue;
            }
            failures = 0;

            /* If the phone goes quiet for 30 seconds, give up on it. */
            struct timeval tv = { 30, 0 };
            setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
            setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

            handle_client(client);
            close(client);
        }

        if (g_listen_fd == fd) {
            g_listen_fd = -1;
            close(fd);
        }
        g_status.running = 0;
        if (g_keep_running) sleep_ms(1000);
    }
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
