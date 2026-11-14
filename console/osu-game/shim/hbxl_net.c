/*
 * hbxl_net.c - name resolution for networked apps on hbxl.
 *
 * Link with -Wl,--wrap=getaddrinfo -Wl,--wrap=freeaddrinfo.  musl's resolver would read /etc/resolv.conf and
 * /etc/hosts through the file server and talk UDP to a nameserver; instead names go to the console's own
 * resolver (nn GetAddrInfo, hbxl runtime hbxl_resolve_ipv4_all in sdk/linux/net.c).  IPv4 only, like hbxl's
 * sockets.  .NET's Dns (SystemNative_GetHostEntryForName / GetAddrInfo) and SocketsHttpHandler land here.
 *
 * Every result list comes from this file (one block, a magic header), so __wrap_freeaddrinfo frees ours and
 * hands anything else to musl.
 */
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>

extern int hbxl_resolve_ipv4_all(const char *name, uint32_t *out, int max);
extern void hbxl_log(const char *text);
int __real_getaddrinfo(const char *node, const char *service, const struct addrinfo *hints, struct addrinfo **res);
void __real_freeaddrinfo(struct addrinfo *ai);

#define MAX_ADDRS 8
#define MAGIC 0x6862786c6e657431ull   /* "hbxlnet1" */

struct entry {
    struct addrinfo ai;
    struct sockaddr_in sa;
};
struct block {
    uint64_t magic;
    int count;
    char canon[256];
    struct entry e[];
};

static int parse_port(const char *service, int socktype, int numeric_only, uint16_t *port) {
    char *end;
    long v;
    (void)socktype;
    *port = 0;
    if (!service || !*service) return 0;
    v = strtol(service, &end, 10);
    if (!*end && v >= 0 && v <= 65535) { *port = (uint16_t)v; return 0; }
    if (numeric_only) return EAI_NONAME;
    if (!strcasecmp(service, "http")) { *port = 80; return 0; }
    if (!strcasecmp(service, "https")) { *port = 443; return 0; }
    return EAI_SERVICE;
}

int __wrap_getaddrinfo(const char *node, const char *service, const struct addrinfo *hints, struct addrinfo **res) {
    struct addrinfo h;
    uint32_t addrs[MAX_ADDRS];
    struct in_addr numeric;
    struct block *b;
    uint16_t port;
    int count, i, r, types[2], ntypes = 0;
    char line[320];

    if (!res) return EAI_FAIL;
    *res = 0;
    memset(&h, 0, sizeof h);
    if (hints) h = *hints;
    if (h.ai_family != AF_UNSPEC && h.ai_family != AF_INET) return EAI_FAMILY;
    if (!node && !service) return EAI_NONAME;
    r = parse_port(service, h.ai_socktype, (h.ai_flags & AI_NUMERICSERV) != 0, &port);
    if (r) return r;

    if (!node) {                                          /* bind/connect to "any" or loopback */
        addrs[0] = htonl((h.ai_flags & AI_PASSIVE) ? INADDR_ANY : INADDR_LOOPBACK);
        count = 1;
    } else if (inet_pton(AF_INET, node, &numeric) == 1) {
        addrs[0] = numeric.s_addr;
        count = 1;
    } else if (strchr(node, ':')) {
        return EAI_FAMILY;                                /* an IPv6 literal: no IPv6 here */
    } else if (h.ai_flags & AI_NUMERICHOST) {
        return EAI_NONAME;
    } else if (!strcasecmp(node, "localhost") || !strcasecmp(node, "localhost.")) {
        addrs[0] = htonl(INADDR_LOOPBACK);
        count = 1;
    } else {
        snprintf(line, sizeof line, "hbxl-net: getaddrinfo(%s)", node);
        hbxl_log(line);
        count = hbxl_resolve_ipv4_all(node, addrs, MAX_ADDRS);
        snprintf(line, sizeof line, "hbxl-net: resolve %s -> %d address(es)%s", node, count < 0 ? 0 : count,
                 count > 0 ? "" : " (EAI_NONAME)");
        hbxl_log(line);
        if (count <= 0) return count < 0 ? EAI_AGAIN : EAI_NONAME;
    }

    if (h.ai_socktype) types[ntypes++] = h.ai_socktype;
    else { types[ntypes++] = SOCK_STREAM; types[ntypes++] = SOCK_DGRAM; }

    b = calloc(1, sizeof *b + sizeof(struct entry) * (size_t)(count * ntypes));
    if (!b) return EAI_MEMORY;
    b->magic = MAGIC;
    b->count = count * ntypes;
    if (node) snprintf(b->canon, sizeof b->canon, "%s", node);
    for (i = 0; i < b->count; i++) {
        struct entry *e = &b->e[i];
        int type = types[i % ntypes];
        e->sa.sin_family = AF_INET;
        e->sa.sin_port = htons(port);
        e->sa.sin_addr.s_addr = addrs[i / ntypes];
        e->ai.ai_family = AF_INET;
        e->ai.ai_socktype = type;
        e->ai.ai_protocol = h.ai_protocol ? h.ai_protocol : (type == SOCK_DGRAM ? IPPROTO_UDP : IPPROTO_TCP);
        e->ai.ai_addrlen = sizeof e->sa;
        e->ai.ai_addr = (struct sockaddr *)&e->sa;
        e->ai.ai_canonname = (i == 0 && (h.ai_flags & AI_CANONNAME) && node) ? b->canon : 0;
        e->ai.ai_next = i + 1 < b->count ? &b->e[i + 1].ai : 0;
    }
    *res = &b->e[0].ai;
    return 0;
}

void __wrap_freeaddrinfo(struct addrinfo *ai) {
    struct block *b;
    if (!ai) return;
    b = (struct block *)((char *)ai - offsetof(struct block, e));
    if (b->magic == MAGIC) {
        b->magic = 0;
        free(b);
        return;
    }
    __real_freeaddrinfo(ai);
}
