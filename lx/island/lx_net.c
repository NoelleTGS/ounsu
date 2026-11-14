/*
 * Name resolution in the island: getaddrinfo goes to the console's resolver (struct lx_host's resolve_ipv4) instead
 * of musl's, which would read /etc/resolv.conf and talk UDP DNS itself. IPv4 only, like the host's sockets. .NET's
 * Dns (SystemNative_GetHostEntryForName / GetAddrInfo) and SocketsHttpHandler land here.
 *
 * Linked into every island with -Wl,--wrap=getaddrinfo,--wrap=freeaddrinfo (tools/make-island.sh). Every result list
 * comes from here (one block, a magic header), so __wrap_freeaddrinfo frees ours and passes anything else to musl.
 * Ported from the HBLauncher shim (hbxl_net.c).
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
#include <unistd.h>

#include "lx_abi.h"

__attribute__((visibility("hidden"))) const struct lx_host *lx_island_host(void);
int __real_getaddrinfo(const char *node, const char *service, const struct addrinfo *hints, struct addrinfo **res);
void __real_freeaddrinfo(struct addrinfo *ai);

#define MAX_ADDRS 8
#define MAGIC 0x6c786e6574316c78ull /* "lxnet1lx" */

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

static int parse_port(const char *service, int numeric_only, uint16_t *port) {
    char *end;
    long v;
    *port = 0;
    if (!service || !*service) return 0;
    v = strtol(service, &end, 10);
    if (!*end && v >= 0 && v <= 65535) {
        *port = (uint16_t)v;
        return 0;
    }
    if (numeric_only) return EAI_NONAME;
    if (!strcasecmp(service, "http")) {
        *port = 80;
        return 0;
    }
    if (!strcasecmp(service, "https")) {
        *port = 443;
        return 0;
    }
    return EAI_SERVICE;
}

int __wrap_getaddrinfo(const char *node, const char *service, const struct addrinfo *hints, struct addrinfo **res) {
    struct addrinfo h;
    uint32_t addrs[MAX_ADDRS];
    struct in_addr numeric;
    uint16_t port;
    int count, r, types[2], ntypes = 0;

    if (!res) return EAI_FAIL;
    *res = NULL;
    memset(&h, 0, sizeof h);
    if (hints) h = *hints;
    if (h.ai_family != AF_UNSPEC && h.ai_family != AF_INET) return EAI_FAMILY;
    if (!node && !service) return EAI_NONAME;
    r = parse_port(service, (h.ai_flags & AI_NUMERICSERV) != 0, &port);
    if (r) return r;

    if (!node) { /* bind/connect to "any" or loopback */
        addrs[0] = htonl((h.ai_flags & AI_PASSIVE) ? INADDR_ANY : INADDR_LOOPBACK);
        count = 1;
    } else if (inet_pton(AF_INET, node, &numeric) == 1) {
        addrs[0] = numeric.s_addr;
        count = 1;
    } else if (strchr(node, ':')) {
        return EAI_FAMILY; /* an IPv6 literal: no IPv6 here */
    } else if (h.ai_flags & AI_NUMERICHOST) {
        return EAI_NONAME;
    } else if (!strcasecmp(node, "localhost") || !strcasecmp(node, "localhost.")) {
        addrs[0] = htonl(INADDR_LOOPBACK);
        count = 1;
    } else {
        count = lx_island_host()->resolve_ipv4(node, addrs, MAX_ADDRS);
        if (count <= 0) return count < 0 ? EAI_AGAIN : EAI_NONAME;
    }

    if (h.ai_socktype) {
        types[ntypes++] = h.ai_socktype;
    } else {
        types[ntypes++] = SOCK_STREAM;
        types[ntypes++] = SOCK_DGRAM;
    }

    struct block *b = calloc(1, sizeof *b + sizeof(struct entry) * (size_t)(count * ntypes));
    if (!b) return EAI_MEMORY;
    b->magic = MAGIC;
    b->count = count * ntypes;
    if (node) snprintf(b->canon, sizeof b->canon, "%s", node);
    for (int i = 0; i < b->count; i++) {
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
        e->ai.ai_canonname = (i == 0 && (h.ai_flags & AI_CANONNAME) && node) ? b->canon : NULL;
        e->ai.ai_next = i + 1 < b->count ? &b->e[i + 1].ai : NULL;
    }
    *res = &b->e[0].ai;
    return 0;
}

void __wrap_freeaddrinfo(struct addrinfo *ai) {
    if (!ai) return;
    struct block *b = (struct block *)((char *)ai - offsetof(struct block, e));
    if (b->magic == MAGIC) {
        b->magic = 0;
        free(b);
        return;
    }
    __real_freeaddrinfo(ai);
}
