/*
 * hbxl_dl_ssl.c - fake libdl for apps that only need OpenSSL (net-probe).  .NET's
 * System.Security.Cryptography.Native.OpenSsl dlopen()s "libssl.so.3" and dlsym()s each function; the
 * statically linked OpenSSL answers through obj/ssl_syms.c (gen-ssl-syms.py).  osu-game has the same
 * logic in hbxl_vk_shim.c, next to its Vulkan loader.
 */
#include <stdio.h>
#include <string.h>

extern void hbxl_log(const char *text);

struct hbxl_dl_sym { const char *name; void *addr; };
extern const struct hbxl_dl_sym hbxl_ssl_syms[];
extern const unsigned hbxl_ssl_count;

#define OPENSSL_HANDLE ((void *)0x5353514cu)
static const char *g_dlerror;

static void *ssl_lookup(const char *name) {
    unsigned lo = 0, hi = hbxl_ssl_count;           /* sorted by strcmp */
    while (lo < hi) {
        unsigned mid = (lo + hi) / 2;
        int c = strcmp(name, hbxl_ssl_syms[mid].name);
        if (!c) return hbxl_ssl_syms[mid].addr;
        if (c < 0) hi = mid; else lo = mid + 1;
    }
    return 0;
}

void *dlopen(const char *file, int mode) {
    char line[200];
    (void)mode;
    if (file && (strstr(file, "libssl") || strstr(file, "libcrypto"))) {
        snprintf(line, sizeof line, "dl-ssl: dlopen(%s) -> static OpenSSL (%u symbols)", file, hbxl_ssl_count);
        hbxl_log(line);
        g_dlerror = 0;
        return OPENSSL_HANDLE;
    }
    snprintf(line, sizeof line, "dl-ssl: dlopen(%s) refused", file ? file : "(null)");
    hbxl_log(line);
    g_dlerror = "hbxl: dynamic loading is not supported";
    return 0;
}

void *dlsym(void *restrict handle, const char *restrict name) {
    void *p = handle == OPENSSL_HANDLE && name ? ssl_lookup(name) : 0;
    g_dlerror = p ? 0 : "hbxl: symbol not found";
    return p;
}

char *dlerror(void) {
    const char *e = g_dlerror;
    g_dlerror = 0;
    return (char *)e;
}

int dlclose(void *handle) {
    (void)handle;
    return 0;
}
