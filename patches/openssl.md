# OpenSSL (`src/native/openssl`)

Upstream: [openssl/openssl](https://github.com/openssl/openssl) `openssl-3.5.9` (`openssl.base`). Patch: `openssl.diff`.

.NET's cryptography and TLS (`System.Security.Cryptography.Native.OpenSsl`) load `libssl.so.3` with dlopen. The console
has no dynamic loading, so OpenSSL is built as static `libssl.a` + `libcrypto.a`, and a generated symbol table
(`probes/gfx-probe/tools/gen-ssl-syms.py` → `obj/ssl_syms.c`) answers .NET's `dlopen`/`dlsym` calls from the fake libdl in
the app's shims. It's what osu! uses for HTTPS, and for hashing.

## Change

| File | Change |
|---|---|
| `crypto/armcap.c` | Use `getauxval()` on musl Linux for CPU feature detection. Without it OpenSSL probes instructions and catches SIGILL; hbxl has no signals, so the first unsupported probe would kill the app. hbxl passes no `AT_HWCAP`, so OpenSSL uses its portable code |

## Notes

- `OPENSSLDIR` is `/usr/local/ssl`, so OpenSSL looks for CA certificates at `/usr/local/ssl/cert.pem`. On the console that
  path is on the file server: `tools/install-ca-bundle.sh` copies the PC's bundle to `fsroot/usr/local/ssl/cert.pem`.
- Random numbers come from hbxl's `getrandom`, which now uses the console's CSPRNG (`HBLauncher.md`, change 17).

## Restoring

```sh
git -C src/native/openssl checkout -f 45e844fa2a14ec92d146bd8f5778ac130b6625fb
git -C src/native/openssl apply --whitespace=nowarn ~/osu-HBLauncher/patches/openssl.diff
```
