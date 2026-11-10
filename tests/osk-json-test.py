#!/usr/bin/python3
"""Focused checks for the config helper's special-file and failure paths."""
from __future__ import annotations

import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path

HELPER = Path(__file__).resolve().parent.parent / "shell/ekollof.osk/osk-json.py"


def run(*args: str, timeout: float = 3.0) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [sys.executable, str(HELPER), *args],
        capture_output=True,
        text=True,
        timeout=timeout,
    )


with tempfile.TemporaryDirectory() as tmp:
    config = Path(tmp) / "omarchy"
    config.mkdir()
    target = config / "osk.json"

    os.mkfifo(target)
    try:
        fifo = run("load", str(target), timeout=2.0)
    except subprocess.TimeoutExpired:
        raise SystemExit("FAIL: load blocked on a FIFO with no writer")
    assert fifo.returncode != 0, fifo
    target.unlink()

    target.write_text('{"layout":"us"}\n', encoding="utf-8")
    loaded = run("load", str(target))
    assert loaded.returncode == 0, loaded.stderr
    assert json.loads(loaded.stdout) == {"layout": "us"}

    saved = run("save", str(target), '{"layout":"dk"}')
    assert saved.returncode == 0, saved.stderr
    assert json.loads(target.read_text(encoding="utf-8")) == {"layout": "dk"}

    with tempfile.TemporaryDirectory() as tmp2:
        linked_config = Path(tmp2) / "omarchy"
        linked_config.mkdir()
        linked_target = linked_config / "osk.json"
        linked_target.symlink_to(target)
        symlink = run("load", str(linked_target))
        assert symlink.returncode != 0, symlink

    invalid = run("save", str(target), "{not json")
    assert invalid.returncode != 0, invalid
    assert json.loads(target.read_text(encoding="utf-8")) == {"layout": "dk"}

print("osk-json special-file and failure checks passed")
