#!/usr/bin/env python3
"""PC-side server for probes/net-probe (TESTING.md test 7).  Standard library + the openssl CLI only.

  HTTP       :39510   /hello, /bytes/N (N pattern bytes), /raw/N (pattern bytes only, no headers, then close), /slow (5 s stall)
  HTTPS      :39511   same paths; certificate signed by a local test CA (probes/net-probe/certs/)
  WebSocket  :39512   /echo (binary/text frames echoed back)

The test CA's certificate is copied to fsroot/net-probe/ca.pem, which the probe trusts explicitly.  Its key never
leaves the PC.  Usage:  python3 tools/net-probe-server.py [--host 192.168.1.206]
"""
import argparse
import base64
import hashlib
import http.server
import socket
import socketserver
import ssl
import struct
import subprocess
import sys
import threading
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
CERTS = ROOT / "probes/net-probe/certs"
FSROOT = ROOT / "fsroot"
PERIOD = 131072
PATTERN = bytes(((i * 131 + (i >> 9)) & 0xFF) for i in range(PERIOD))   # same as Program.Pattern


def make_certs(host):
    CERTS.mkdir(parents=True, exist_ok=True)
    ca_key, ca_pem = CERTS / "ca.key", CERTS / "ca.pem"
    key, csr, pem, ext = CERTS / "server.key", CERTS / "server.csr", CERTS / "server.pem", CERTS / "server.ext"
    stamp = CERTS / "host.txt"
    if not ca_pem.exists():
        subprocess.run(["openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-keyout", ca_key, "-out", ca_pem,
                        "-days", "3650", "-subj", "/CN=ounsu net-probe test CA",
                        "-addext", "basicConstraints=critical,CA:TRUE", "-addext", "keyUsage=critical,keyCertSign"],
                       check=True, capture_output=True)
    if not pem.exists() or not stamp.exists() or stamp.read_text() != host:
        ext.write_text(f"subjectAltName=IP:{host}\nbasicConstraints=CA:FALSE\nkeyUsage=digitalSignature,keyEncipherment\n"
                       "extendedKeyUsage=serverAuth\n")
        subprocess.run(["openssl", "req", "-newkey", "rsa:2048", "-nodes", "-keyout", key, "-out", csr,
                        "-subj", f"/CN={host}"], check=True, capture_output=True)
        subprocess.run(["openssl", "x509", "-req", "-in", csr, "-CA", ca_pem, "-CAkey", ca_key, "-CAcreateserial",
                        "-out", pem, "-days", "825", "-extfile", ext], check=True, capture_output=True)
        stamp.write_text(host)
    (FSROOT / "net-probe").mkdir(parents=True, exist_ok=True)
    (FSROOT / "net-probe/ca.pem").write_bytes(ca_pem.read_bytes())
    return pem, key


def pattern_chunks(n, chunk=65536):
    at = 0
    while at < n:
        k = min(chunk, n - at)
        start = at % PERIOD
        data = (PATTERN[start:] + PATTERN)[:k] if start + k > PERIOD else PATTERN[start:start + k]
        yield data
        at += k


class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    server_version = "net-probe-server/1.0"

    def log_message(self, fmt, *args):
        sys.stderr.write(f"[{self.server.label}] {self.client_address[0]} {fmt % args}\n")

    def reply(self, body, close=False, ctype="text/plain"):
        self.send_response(200)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        if close:
            self.send_header("Connection", "close")
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        parts = self.path.strip("/").split("/")
        if parts[0] == "hello":
            self.reply(f"hello from net-probe-server ({self.server.label})\n".encode(), close=self.request_version == "HTTP/1.0")
        elif parts[0] == "raw" and len(parts) == 2 and parts[1].isdigit():
            # body only, no HTTP headers, then close: for the probe's plain-socket stage
            for c in pattern_chunks(min(int(parts[1]), 256 << 20)):
                self.wfile.write(c)
            self.close_connection = True
        elif parts[0] == "bytes" and len(parts) == 2 and parts[1].isdigit():
            n = min(int(parts[1]), 256 << 20)
            close = self.request_version == "HTTP/1.0"
            self.send_response(200)
            self.send_header("Content-Type", "application/octet-stream")
            self.send_header("Content-Length", str(n))
            if close:
                self.send_header("Connection", "close")
            self.end_headers()
            t0 = time.time()
            for c in pattern_chunks(n):
                self.wfile.write(c)
            dt = time.time() - t0
            if n >= 1 << 20:
                sys.stderr.write(f"[{self.server.label}] sent {n} bytes in {dt:.2f} s ({n / 1048576 / max(dt, 1e-6):.1f} MiB/s)\n")
            if close:
                self.close_connection = True
        elif parts[0] == "slow":
            time.sleep(5)
            try:
                self.reply(b"slow reply\n")
            except OSError:
                pass
        else:
            self.send_error(404)


class Server(socketserver.ThreadingMixIn, http.server.HTTPServer):
    daemon_threads = True
    allow_reuse_address = True
    request_queue_size = 64      # the probe's socket-pool stage opens 40 connections at once


def ws_serve(conn, addr):
    """Minimal RFC 6455 echo: handshake, then echo every data frame (client frames are masked)."""
    f = conn.makefile("rb")
    headers = {}
    line = f.readline()
    while True:
        h = f.readline()
        if h in (b"\r\n", b"\n", b""):
            break
        k, _, v = h.decode().partition(":")
        headers[k.strip().lower()] = v.strip()
    key = headers.get("sec-websocket-key")
    if not key:
        conn.close()
        return
    accept = base64.b64encode(hashlib.sha1((key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11").encode()).digest()).decode()
    conn.sendall(("HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                  f"Sec-WebSocket-Accept: {accept}\r\n\r\n").encode())
    sys.stderr.write(f"[ws] {addr[0]} connected ({line.decode().strip()})\n")
    try:
        while True:
            hdr = f.read(2)
            if len(hdr) < 2:
                break
            fin_op, ln = hdr[0], hdr[1] & 0x7F
            op = fin_op & 0x0F
            if ln == 126:
                ln = struct.unpack(">H", f.read(2))[0]
            elif ln == 127:
                ln = struct.unpack(">Q", f.read(8))[0]
            mask = f.read(4) if hdr[1] & 0x80 else b"\0\0\0\0"
            data = bytearray(f.read(ln))
            for i in range(len(data)):
                data[i] ^= mask[i & 3]
            if op == 8:                                   # close: answer and stop
                conn.sendall(bytes([0x88, len(data)]) + bytes(data[:125]))
                break
            if op == 9:                                   # ping -> pong
                op = 10
            out = bytes([0x80 | op if fin_op & 0x80 else op])
            n = len(data)
            out += bytes([n]) if n < 126 else (bytes([126]) + struct.pack(">H", n) if n < 65536 else bytes([127]) + struct.pack(">Q", n))
            conn.sendall(out + bytes(data))
            sys.stderr.write(f"[ws] echoed {n} bytes (op {op})\n")
    finally:
        conn.close()


def ws_main(host, port):
    s = socket.socket()
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind((host, port))
    s.listen(8)
    while True:
        c, a = s.accept()
        threading.Thread(target=ws_serve, args=(c, a), daemon=True).start()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="192.168.1.206")
    ap.add_argument("--bind", default="0.0.0.0")
    a = ap.parse_args()
    pem, key = make_certs(a.host)
    http_srv = Server((a.bind, 39510), Handler)
    http_srv.label = "http"
    https_srv = Server((a.bind, 39511), Handler)
    https_srv.label = "https"
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    ctx.load_cert_chain(pem, key)
    https_srv.socket = ctx.wrap_socket(https_srv.socket, server_side=True)
    for srv in (http_srv, https_srv):
        threading.Thread(target=srv.serve_forever, daemon=True).start()
    threading.Thread(target=ws_main, args=(a.bind, 39512), daemon=True).start()
    print(f"net-probe-server: http :39510, https :39511 (cert for IP {a.host}), ws :39512; "
          f"CA copied to {FSROOT / 'net-probe/ca.pem'}", flush=True)
    try:
        while True:
            time.sleep(3600)
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
