#!/usr/bin/python3
"""Load/save ~/.config/omarchy/osk.json without following symlinks.

Usage:
  osk-json.py load <abs-path>
  osk-json.py save <abs-path> <json>

Exit 0 on success, 2 if load and the file is missing, 1 otherwise.
Reads and writes are capped; the target must be a regular file owned by us.
"""
from __future__ import annotations

import json
import os
import stat
import sys

MAX = 8192


def _check_path(path: str) -> str:
    if not path.startswith("/") or ".." in path.split("/"):
        sys.exit(1)
    if not path.endswith("/omarchy/osk.json"):
        sys.exit(1)
    return path


def _open_dir(path: str) -> int:
    return os.open(path, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW | os.O_CLOEXEC)


def load(path: str) -> None:
    flags = os.O_RDONLY | os.O_NOFOLLOW | os.O_CLOEXEC
    try:
        fd = os.open(path, flags)
    except FileNotFoundError:
        sys.exit(2)
    except OSError:
        sys.exit(1)
    try:
        st = os.fstat(fd)
        if not stat.S_ISREG(st.st_mode) or st.st_uid != os.getuid() or st.st_size > MAX:
            sys.exit(1)
        data = os.read(fd, MAX + 1)
        if len(data) > MAX:
            sys.exit(1)
        sys.stdout.buffer.write(data)
    finally:
        os.close(fd)


def save(path: str, body: bytes) -> None:
    if len(body) > MAX:
        sys.exit(1)
    try:
        json.loads(body.decode("utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError, ValueError):
        sys.exit(1)

    omarchy = os.path.dirname(path)
    config_home = os.path.dirname(omarchy)
    name = os.path.basename(path)
    if name != "osk.json":
        sys.exit(1)

    try:
        cfd = _open_dir(config_home)
    except OSError:
        sys.exit(1)
    try:
        st = os.fstat(cfd)
        if st.st_uid != os.getuid() or not stat.S_ISDIR(st.st_mode):
            sys.exit(1)
        try:
            os.mkdir("omarchy", 0o755, dir_fd=cfd)
        except FileExistsError:
            pass
        ofd = os.open(
            "omarchy",
            os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW | os.O_CLOEXEC,
            dir_fd=cfd,
        )
    finally:
        os.close(cfd)

    try:
        st = os.fstat(ofd)
        if st.st_uid != os.getuid() or not stat.S_ISDIR(st.st_mode):
            sys.exit(1)
        tmpname = ".osk.json.%d.tmp" % os.getpid()
        flags = os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW | os.O_CLOEXEC
        try:
            tfd = os.open(tmpname, flags, 0o644, dir_fd=ofd)
        except FileExistsError:
            os.unlink(tmpname, dir_fd=ofd)
            tfd = os.open(tmpname, flags, 0o644, dir_fd=ofd)
        try:
            off = 0
            while off < len(body):
                n = os.write(tfd, body[off:])
                if n <= 0:
                    sys.exit(1)
                off += n
            os.fsync(tfd)
        finally:
            os.close(tfd)
        os.rename(tmpname, name, src_dir_fd=ofd, dst_dir_fd=ofd)
    finally:
        os.close(ofd)


def main() -> None:
    if len(sys.argv) < 3:
        sys.exit(1)
    op = sys.argv[1]
    path = _check_path(sys.argv[2])
    if op == "load" and len(sys.argv) == 3:
        load(path)
    elif op == "save" and len(sys.argv) == 4:
        save(path, sys.argv[3].encode("utf-8"))
    else:
        sys.exit(1)


if __name__ == "__main__":
    main()
