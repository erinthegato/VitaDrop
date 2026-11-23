/*
 * A tiny mDNS responder: answers only "what is the address of vitadrop.local?".
 *
 * iPhones, Macs and Windows 10+ ask that question on the local network
 * (to 224.0.0.251, port 5353) whenever you type or use a ".local" name.
 * We listen there and reply with the Vita's IP address.
 */
#include "mdns.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

#ifdef __vita__
#include <psp2/kernel/threadmgr.h>
#else
#include <pthread.h>
#endif

#define MDNS_PORT  5353
#define MDNS_GROUP "224.0.0.251"
#define TTL_SECONDS 120

static volatile int g_running = 0;
static volatile int g_active = 0;
static volatile uint32_t g_ip = 0;       /* network byte order; 0 = unknown */
static int g_sock = -1;

static void sleep_ms(int ms)
{
#ifdef __vita__
    sceKernelDelayThread((SceUInt)ms * 1000);
#else
    usleep((useconds_t)ms * 1000);
#endif
}

/* Writes "vitadrop.local" in DNS form: 8 v i t a d r o p 5 l o c a l 0 */
static size_t write_name(unsigned char *out, const char *name)
{
    size_t o = 0;
    while (*name) {
        const char *dot = strchr(name, '.');
        size_t len = dot ? (size_t)(dot - name) : strlen(name);
        out[o++] = (unsigned char)len;
        memcpy(out + o, name, len);
        o += len;
        name += len;
        if (*name == '.') name++;
    }
    out[o++] = 0;
    return o;
}

/*
 * Reads a name from a DNS packet into "a.b.c" form, lower-cased.
 * Returns the position just after the name, or 0 if it's broken.
 */
static size_t read_name(const unsigned char *pkt, size_t len, size_t pos, char *out, size_t out_size)
{
    size_t o = 0, after = 0;
    int jumps = 0;
    while (pos < len) {
        unsigned char l = pkt[pos];
        if (l == 0) { pos++; break; }
        if ((l & 0xC0) == 0xC0) {                 /* compressed: jump elsewhere */
            if (pos + 1 >= len || ++jumps > 8) return 0;
            if (!after) after = pos + 2;
            pos = ((size_t)(l & 0x3F) << 8) | pkt[pos + 1];
            continue;
        }
        pos++;
        if (pos + l > len) return 0;
        if (o && o + 1 < out_size) out[o++] = '.';
        for (unsigned i = 0; i < l && o + 1 < out_size; i++) {
            char c = (char)pkt[pos + i];
            out[o++] = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
        }
        pos += l;
    }
    out[o] = '\0';
    return after ? after : pos;
}

/* Builds the answer: "vitadrop.local is at <ip>". */
static size_t build_answer(unsigned char *out, uint16_t id, int include_question)
{
    size_t o = 0;
    out[o++] = (unsigned char)(id >> 8); out[o++] = (unsigned char)id;
    out[o++] = 0x84; out[o++] = 0x00;            /* response, authoritative */
    out[o++] = 0; out[o++] = include_question ? 1 : 0;   /* questions */
    out[o++] = 0; out[o++] = 1;                  /* answers */
    out[o++] = 0; out[o++] = 0;                  /* authority */
    out[o++] = 0; out[o++] = 0;                  /* additional */

    if (include_question) {
        o += write_name(out + o, MDNS_HOSTNAME);
        out[o++] = 0; out[o++] = 1;              /* type A */
        out[o++] = 0; out[o++] = 1;              /* class IN */
    }

    o += write_name(out + o, MDNS_HOSTNAME);
    out[o++] = 0; out[o++] = 1;                  /* type A */
    out[o++] = include_question ? 0x00 : 0x80;   /* cache-flush bit for normal mDNS */
    out[o++] = 1;                                /* class IN */
    out[o++] = 0; out[o++] = 0;
    out[o++] = (unsigned char)(TTL_SECONDS >> 8); out[o++] = (unsigned char)TTL_SECONDS;
    out[o++] = 0; out[o++] = 4;                  /* 4-byte IPv4 address */
    memcpy(out + o, (const void *)&g_ip, 4);
    o += 4;
    return o;
}

/* Returns 1 if the packet asks for vitadrop.local (type A or ANY). */
static int asks_for_us(const unsigned char *pkt, size_t len, int *unicast_wanted)
{
    if (len < 12) return 0;
    if (pkt[2] & 0x80) return 0;                 /* it's an answer, not a question */
    int questions = (pkt[4] << 8) | pkt[5];
    size_t pos = 12;
    for (int q = 0; q < questions; q++) {
        char name[256];
        pos = read_name(pkt, len, pos, name, sizeof(name));
        if (!pos || pos + 4 > len) return 0;
        int type = (pkt[pos] << 8) | pkt[pos + 1];
        int klass = (pkt[pos + 2] << 8) | pkt[pos + 3];
        pos += 4;
        if (strcmp(name, MDNS_HOSTNAME) == 0 && (type == 1 || type == 255) &&
            (klass & 0x7FFF) == 1) {
            if (klass & 0x8000) *unicast_wanted = 1;
            return 1;
        }
    }
    return 0;
}

static int open_socket(void)
{
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return -1;

    int yes = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
#ifdef SO_REUSEPORT
    setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &yes, sizeof(yes));
#endif

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(MDNS_PORT);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) { close(fd); return -1; }

    struct ip_mreq group;
    memset(&group, 0, sizeof(group));
    group.imr_multiaddr.s_addr = inet_addr(MDNS_GROUP);
    group.imr_interface.s_addr = htonl(INADDR_ANY);
    if (setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &group, sizeof(group)) < 0) {
        close(fd);
        return -1;
    }

    unsigned char ttl = 255;
    setsockopt(fd, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));

    /* wake up once a second so we can notice when we're asked to stop */
    struct timeval tv = { 1, 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    return fd;
}

static void mdns_loop(void)
{
    unsigned char pkt[1500], reply[256];

    while (g_running) {
        if (g_ip == 0) { g_active = 0; sleep_ms(1000); continue; }

        int fd = open_socket();
        if (fd < 0) { g_active = 0; sleep_ms(3000); continue; }
        g_sock = fd;
        g_active = 1;

        int errors = 0;
        while (g_running && errors < 5) {
            struct sockaddr_in from;
            socklen_t from_len = sizeof(from);
            ssize_t n = recvfrom(fd, pkt, sizeof(pkt), 0, (struct sockaddr *)&from, &from_len);
            if (n < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) continue;
                errors++;                        /* Wi-Fi went away; reopen */
                sleep_ms(500);
                continue;
            }
            errors = 0;

            int unicast_wanted = 0;
            if (g_ip == 0 || !asks_for_us(pkt, (size_t)n, &unicast_wanted)) continue;

            if (ntohs(from.sin_port) != MDNS_PORT) {
                /* a simple one-off question (e.g. from a test tool): answer it directly */
                uint16_t id = (uint16_t)((pkt[0] << 8) | pkt[1]);
                size_t len = build_answer(reply, id, 1);
                sendto(fd, reply, len, 0, (struct sockaddr *)&from, from_len);
            } else {
                size_t len = build_answer(reply, 0, 0);
                if (unicast_wanted)
                    sendto(fd, reply, len, 0, (struct sockaddr *)&from, from_len);
                struct sockaddr_in group;
                memset(&group, 0, sizeof(group));
                group.sin_family = AF_INET;
                group.sin_port = htons(MDNS_PORT);
                group.sin_addr.s_addr = inet_addr(MDNS_GROUP);
                sendto(fd, reply, len, 0, (struct sockaddr *)&group, sizeof(group));
            }
        }

        g_active = 0;
        g_sock = -1;
        close(fd);
        if (g_running) sleep_ms(1000);
    }
    g_active = 0;
}

#ifdef __vita__
static int mdns_thread(SceSize args, void *argp)
{
    (void)args; (void)argp;
    mdns_loop();
    return sceKernelExitDeleteThread(0);
}
#else
static void *mdns_thread(void *arg)
{
    (void)arg;
    mdns_loop();
    return NULL;
}
#endif

int mdns_start(void)
{
    g_running = 1;
#ifdef __vita__
    SceUID thid = sceKernelCreateThread("vitadrop_mdns", mdns_thread, 0x10000100, 0x4000, 0, 0, NULL);
    if (thid < 0) return -1;
    return sceKernelStartThread(thid, 0, NULL);
#else
    pthread_t t;
    if (pthread_create(&t, NULL, mdns_thread, NULL) != 0) return -1;
    pthread_detach(t);
    return 0;
#endif
}

void mdns_stop(void)
{
    g_running = 0;
}

void mdns_set_ip(const char *ip)
{
    g_ip = ip && ip[0] ? inet_addr(ip) : 0;
}

int mdns_is_active(void)
{
    return g_active && g_ip != 0;
}
