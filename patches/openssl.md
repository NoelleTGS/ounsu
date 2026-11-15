# OpenSSL (`src/native/openssl`)

Upstream: [openssl/openssl](https://github.com/openssl/openssl) `openssl-3.5.9` (`openssl.base`). Patch: `openssl.diff`.

.NET's cryptography and TLS (`System.Security.Cryptography.Native.OpenSsl`) load `libssl.so.3` with dlopen. The console
has no dynamic loading, so OpenSSL is built as static `libssl.a` + `libcrypto.a`, and a generated symbol table
(`tools/gen-ssl-syms.py` → `obj/ssl_syms.c`) answers .NET's `dlopen`/`dlsym` calls from the island's fake libdl. It's what osu! uses for HTTPS, and for hashing.

## Change

| File | Change |
|---|---|
| `crypto/armcap.c` | Use `getauxval()` on musl Linux for CPU feature detection. Without it OpenSSL probes instructions and catches SIGILL; the console has no signals, so the first unsupported probe would kill the app. The island passes an `AT_HWCAP` with the console CPU's features (`lx/island/lx_island.c`), so OpenSSL uses its ARMv8 crypto code |

## Notes

- `OPENSSLDIR` is `/usr/local/ssl`, so OpenSSL looks for CA certificates at `/usr/local/ssl/cert.pem`. On the console that
  path is in the `.nro`'s romfs: `console/osu-game/build.sh` copies `fsroot/usr/local/ssl/cert.pem` (a CA bundle from
  the PC) into it.
- Random numbers come from the host's `getrandom`, which uses the console's CSPRNG (`randomGet`).

## Restoring

```sh
git -C src/native/openssl checkout -f 45e844fa2a14ec92d146bd8f5778ac130b6625fb
git -C src/native/openssl apply --whitespace=nowarn patches/openssl.diff
sh tools/build-native.sh openssl   # libssl.a and libcrypto.a, in the checkout
```
