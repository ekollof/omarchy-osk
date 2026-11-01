#!/usr/bin/env python3
"""Add the read filter around the user's existing Steam desktop launcher."""
import os
from pathlib import Path
import shutil
import re
import shlex
import stat
import sys
import tempfile
import time

home = Path.home()
wrapper = str(home / '.local/bin/osk-steam')


def publish(dest, updated, mode):
    if dest.exists():
        backup = dest.with_name(f'{dest.name}.osk-backup-{time.time_ns()}')
        shutil.copy2(dest, backup)
        print(f'Steam launcher backup: {backup}')
    dest.parent.mkdir(parents=True, exist_ok=True)
    fd, tmp = tempfile.mkstemp(prefix='.osk-steam-', dir=dest.parent)
    try:
        with os.fdopen(fd, 'w') as f:
            f.write(updated)
        os.chmod(tmp, mode)
        os.replace(tmp, dest)
    finally:
        if os.path.exists(tmp):
            os.unlink(tmp)


# A user may already shadow steam with an autoscaling shell script, possibly
# through a symlink. Preserve it and wrap only its direct packaged Steam
# execs. Never follow a link outside this user's local bin directory.
cli = (home / '.local/bin/steam').resolve()
if cli.is_relative_to(home / '.local/bin') and cli.is_file() and cli.stat().st_uid == os.getuid():
    original = cli.read_text()
    if original.startswith(('#!/bin/bash\n', '#!/bin/sh\n', '#!/usr/bin/env bash\n')):
        updated = re.sub(r'^(\s*exec) /usr/bin/steam(?=\s|$)',
                         lambda m: m[1] + ' ' + shlex.quote(wrapper) + ' /usr/bin/steam',
                         original, flags=re.MULTILINE)
        if updated != original:
            publish(cli, updated, stat.S_IMODE(cli.stat().st_mode))

dest = home / '.local/share/applications/steam.desktop'
source = dest if dest.exists() else Path('/usr/share/applications/steam.desktop')
if dest.is_symlink() or (dest.exists() and dest.stat().st_uid != os.getuid()):
    sys.exit(f'refusing launcher not owned by this user: {dest}')
if source.exists():
    original = source.read_text()
    # Preserve autoscaling wrappers, flags, field codes, and desktop actions.
    lines = []
    for line in original.splitlines(keepends=True):
        if line.startswith('Exec=') and not line.startswith(f'Exec="{wrapper}" '):
            line = f'Exec="{wrapper}" ' + line[5:]
        lines.append(line)
    updated = ''.join(lines)
    if not dest.exists() or updated != original:
        publish(dest, updated, 0o644)
