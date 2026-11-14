#!/usr/bin/env python3
"""Mirror a folder from the PC to the console's SD card over FTP (ounceshell's FTP server, port 1337).

    python3 tools/ftp-upload.py LOCAL_DIR REMOTE_DIR [--host 192.168.1.177] [--port 1337]
    python3 tools/ftp-upload.py --list REMOTE_DIR

Files that already exist remotely with the same size are skipped, so an interrupted upload can simply be run again.
Directories are created as needed. Standard library only (ftplib), one connection.

Example: python3 tools/ftp-upload.py fsroot/osu-resources /switch/osu/osu-resources
(ounceshell may show the SD card under a device prefix such as "sdmc:"; check with --list / first.)
Only CWD, plain LIST, MKD and STOR with bare names are used: the server crashed on a LIST with a path argument.
"""
import argparse
import ftplib
import os
import sys
import time
from pathlib import Path


def connect(host, port):
    ftp = ftplib.FTP()
    ftp.connect(host, port, timeout=30)
    ftp.login()
    return ftp


def listing(ftp):
    """name -> size (None for directories) of the current remote directory. Plain LIST, no argument: ounceshell's
    server (VitaShell's ftpvita) crashed on `LIST /` (2026-11-13), while Dolphin's CWD + LIST works."""
    entries = {}
    lines = []
    ftp.retrlines("LIST", lines.append)
    for line in lines:
        parts = line.split(None, 8)
        if len(parts) < 9:
            continue
        entries[parts[8]] = None if line.startswith("d") else int(parts[4])
    return entries


_depth = 0  # directories below where the login started


def cwd_into(ftp, path):
    """Go to path (relative to where the login started), creating missing directories: CDUP back up, then CWD down
    one bare name at a time (no path arguments at all)."""
    global _depth
    while _depth:
        ftp.sendcmd("CDUP")
        _depth -= 1
    for part in [p for p in path.split("/") if p and p != "."]:
        try:
            ftp.cwd(part)
        except ftplib.error_perm:
            ftp.mkd(part)
            ftp.cwd(part)
        _depth += 1


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("local", nargs="?")
    ap.add_argument("remote", nargs="?")
    ap.add_argument("--host", default="192.168.1.177")
    ap.add_argument("--port", type=int, default=1337)
    ap.add_argument("--list", metavar="REMOTE_DIR")
    a = ap.parse_args()
    ftp = connect(a.host, a.port)
    if a.list is not None:
        cwd_into(ftp, a.list)
        print(f"{ftp.pwd()}:")
        for name, size in listing(ftp).items():
            print(f"  {name}{'/' if size is None else f'  {size}'}")
        return
    if not a.local or not a.remote:
        ap.error("LOCAL_DIR and REMOTE_DIR are required")
    local = Path(a.local)
    files = sorted(p for p in local.rglob("*") if p.is_file())
    total = sum(p.stat().st_size for p in files)
    sent = skipped = 0
    t0 = time.time()
    cur_dir, known = None, {}
    for i, p in enumerate(files):
        rel = p.relative_to(local).as_posix()
        rdir = a.remote.rstrip("/") + ("/" + os.path.dirname(rel) if os.path.dirname(rel) else "")
        if rdir != cur_dir:
            cwd_into(ftp, rdir)
            cur_dir, known = rdir, listing(ftp)
        size = p.stat().st_size
        if known.get(p.name) == size:
            skipped += size
            continue
        with open(p, "rb") as f:
            ftp.storbinary(f"STOR {p.name}", f, blocksize=65536)
        sent += size
        if i % 50 == 0 or i == len(files) - 1:
            rate = sent / max(time.time() - t0, 0.001) / 1048576
            print(f"\r{i + 1}/{len(files)} files, {(sent + skipped) >> 20}/{total >> 20} MiB ({rate:.1f} MiB/s)",
                  end="", flush=True)
    print(f"\ndone: {sent >> 20} MiB sent, {skipped >> 20} MiB already there, {time.time() - t0:.0f} s")
    ftp.quit()


if __name__ == "__main__":
    sys.exit(main())
