#!/bin/bash
# Build and deploy the omarchy-osk bundle:
#   hypr-osk           (compositor plugin, C++)  -> ~/.local/share/hyprland/plugins/
#   hyprgrass          (vendored edge gestures)  -> ~/.local/share/hyprland/plugins/
#   ekollof.osk        (Quickshell overlay)      -> ~/.config/omarchy/plugins/
#   ekollof.osk-applet (bar widget)              -> ~/.config/omarchy/plugins/
#   hypr/osk.lua       (plugin load, gesture, keybind)
#   hypr/osk-toggle.sh                           -> ~/.config/hypr/scripts/
#
# Idempotent: safe to re-run after every edit. The shell hot-reloads plugin
# code, but a stale component cache can serve old QML — run `omarchy restart
# shell` if changes don't land.
#
# Tools are invoked by absolute path (PATH is reset). Destinations are
# created/replaced only when they are directories/files owned by this uid
# and not symlinks; files are published via temp + rename.

set -euo pipefail
umask 022
export PATH=/usr/bin:/bin
export LC_ALL=C

DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
UID_SELF=$(/usr/bin/id -u)

need() {
  local p=$1
  [[ -x $p ]] || { echo "missing executable: $p" >&2; exit 1; }
}

need /usr/bin/meson
need /usr/bin/install
need /usr/bin/cp
need /usr/bin/rm
need /usr/bin/mv
need /usr/bin/mkdir
need /usr/bin/mktemp
need /usr/bin/stat
need /usr/bin/dirname
need /usr/bin/id
need /usr/bin/grep
need /usr/bin/sed

ensure_user_dir() {
  local d=$1
  if [[ -z $d || $d == / ]]; then
    echo "refusing directory: $d" >&2
    exit 1
  fi
  if [[ -L $d ]]; then
    echo "refusing symlink directory: $d" >&2
    exit 1
  fi
  if [[ -d $d ]]; then
    if [[ $d == "$HOME" || $d == "$HOME"/* ]]; then
      local ou
      ou=$(/usr/bin/stat -c '%u' "$d")
      if [[ $ou != "$UID_SELF" ]]; then
        echo "refusing directory owned by uid $ou: $d" >&2
        exit 1
      fi
    fi
    return 0
  fi
  if [[ -e $d ]]; then
    echo "not a directory: $d" >&2
    exit 1
  fi
  ensure_user_dir "$(/usr/bin/dirname "$d")"
  /usr/bin/mkdir -m 755 "$d"
}

publish_file() {
  local src=$1 dest=$2 mode=$3
  [[ -f $src && ! -L $src ]] || { echo "refusing source: $src" >&2; exit 1; }
  local parent
  parent=$(/usr/bin/dirname "$dest")
  ensure_user_dir "$parent"
  if [[ -L $dest ]]; then
    echo "refusing to overwrite symlink: $dest" >&2
    exit 1
  fi
  if [[ -e $dest ]]; then
    [[ -f $dest ]] || { echo "refusing non-file destination: $dest" >&2; exit 1; }
    local ou
    ou=$(/usr/bin/stat -c '%u' "$dest")
    if [[ $ou != "$UID_SELF" ]]; then
      echo "refusing file owned by uid $ou: $dest" >&2
      exit 1
    fi
  fi
  local tmp
  tmp=$(/usr/bin/mktemp "$parent/.osk.XXXXXX")
  /usr/bin/install -m "$mode" "$src" "$tmp"
  if [[ -L $tmp ]]; then
    /usr/bin/rm -f -- "$tmp"
    echo "temp path became a symlink: $tmp" >&2
    exit 1
  fi
  /usr/bin/mv -T -- "$tmp" "$dest"
}

publish_tree() {
  local src=$1 dest=$2
  [[ -d $src && ! -L $src ]] || { echo "refusing source tree: $src" >&2; exit 1; }
  local parent
  parent=$(/usr/bin/dirname "$dest")
  ensure_user_dir "$parent"
  if [[ -L $dest ]]; then
    echo "refusing to overwrite symlink: $dest" >&2
    exit 1
  fi
  if [[ -e $dest ]]; then
    [[ -d $dest ]] || { echo "refusing non-directory destination: $dest" >&2; exit 1; }
    local ou
    ou=$(/usr/bin/stat -c '%u' "$dest")
    if [[ $ou != "$UID_SELF" ]]; then
      echo "refusing directory owned by uid $ou: $dest" >&2
      exit 1
    fi
  fi
  local tmp bak
  tmp=$(/usr/bin/mktemp -d "$parent/.osk.XXXXXX")
  /usr/bin/cp -a --no-dereference -- "$src"/. "$tmp"/
  if [[ -d $dest ]]; then
    bak=$(/usr/bin/mktemp -d "$parent/.osk-old.XXXXXX")
    /usr/bin/mv -T -- "$dest" "$bak"
    /usr/bin/mv -T -- "$tmp" "$dest"
    /usr/bin/rm -rf -- "$bak"
  else
    /usr/bin/mv -T -- "$tmp" "$dest"
  fi
}

hyprpm_has() {
  [[ -x /usr/bin/hyprpm ]] || return 1
  /usr/bin/hyprpm list 2>/dev/null | /usr/bin/grep -q -- "$1"
}

# 1+2. Build the compositor plugin and install it where Hyprland plugins
# live — unless hyprpm manages it (see hyprpm.toml): then its copy wins and
# a flat copy here would fight it (rebuild with: hyprpm update).
OSK_SO="$HOME/.local/share/hyprland/plugins/libhypr-osk.so"
OSK_LOCAL_BUILT=0
if hyprpm_has hypr-osk; then
  echo "hypr-osk is hyprpm-managed: skipping the local build (rebuild with: hyprpm update)."
else
  [[ -d "$DIR/hypr-osk/build" ]] || /usr/bin/meson setup "$DIR/hypr-osk/build" "$DIR/hypr-osk" >/dev/null
  /usr/bin/meson compile -C "$DIR/hypr-osk/build"
  publish_file "$DIR/hypr-osk/build/libhypr-osk.so" "$OSK_SO" 644
  OSK_LOCAL_BUILT=1
fi

# 3. Build the vendored hyprgrass (edge-swipe gesture) the same way: upstream
# horriblename/hyprgrass + wf-touch, pinned in docs/PINS.md (no git clone).
# Skip when hyprpm manages it instead. glm must already be installed.
if hyprpm_has hyprgrass; then
  echo "hyprgrass is hyprpm-managed: skipping the vendored build (rebuild with: hyprpm update)."
else
  if [[ -x /usr/bin/pacman ]] && ! /usr/bin/pacman -Q glm >/dev/null 2>&1; then
    echo "warning: glm missing (pacman -Q glm); skipping vendored hyprgrass. Install glm and re-run." >&2
  else
    [[ -d "$DIR/vendor/hyprgrass/build" ]] || /usr/bin/meson setup "$DIR/vendor/hyprgrass/build" "$DIR/vendor/hyprgrass" >/dev/null
    /usr/bin/meson compile -C "$DIR/vendor/hyprgrass/build"
    publish_file "$DIR/vendor/hyprgrass/build/src/libhyprgrass.so" \
      "$HOME/.local/share/hyprland/plugins/hyprgrass.so" 644
  fi
fi

# 4. Deploy the shell plugins
ensure_user_dir "$HOME/.config/omarchy/plugins"
for p in ekollof.osk ekollof.osk-applet; do
  publish_tree "$DIR/shell/$p" "$HOME/.config/omarchy/plugins/$p"
done

# 5. Deploy the Hyprland integration
publish_file "$DIR/hypr/osk-toggle.sh" "$HOME/.config/hypr/scripts/osk-toggle.sh" 755
publish_file "$DIR/hypr/osk.lua" "$HOME/.config/hypr/osk.lua" 644
HYPRLAND="$HOME/.config/hypr/hyprland.lua"
if [[ -L $HYPRLAND ]]; then
  echo "refusing to edit symlink: $HYPRLAND" >&2
  exit 1
fi
if [[ -f $HYPRLAND ]]; then
  ou=$(/usr/bin/stat -c '%u' "$HYPRLAND")
  if [[ $ou != "$UID_SELF" ]]; then
    echo "refusing to edit file owned by uid $ou: $HYPRLAND" >&2
    exit 1
  fi
  # Crash-safe insert of require("hypr.osk"): exclusive flock, durable backup
  # announced before any edit, complete write loops to a same-dir staging
  # file, identity revalidation, then atomic rename. The live file is never
  # truncated. Leftover staging is discarded on the next run.
  need /usr/bin/python3
  /usr/bin/python3 - "$HYPRLAND" <<'PY'
import fcntl, os, stat, sys, time

path = sys.argv[1]
uid = os.getuid()
max_size = 1_000_000

def die(msg, code=1):
    sys.stderr.write(msg + "\n")
    sys.exit(code)

def read_all(fd, n):
    buf = bytearray()
    while len(buf) < n:
        chunk = os.read(fd, n - len(buf))
        if not chunk:
            break
        buf.extend(chunk)
    return bytes(buf)

def write_all(fd, data):
    view = memoryview(data)
    while view:
        n = os.write(fd, view)
        if n <= 0:
            raise OSError("short write")
        view = view[n:]

def fsync_dir(p):
    dfd = os.open(os.path.dirname(p) or ".", os.O_RDONLY | os.O_DIRECTORY | os.O_CLOEXEC)
    try:
        os.fsync(dfd)
    finally:
        os.close(dfd)

def unlink_ours(p):
    try:
        st = os.lstat(p)
    except FileNotFoundError:
        return
    if stat.S_ISLNK(st.st_mode) or not stat.S_ISREG(st.st_mode):
        die("refusing leftover path: " + p)
    if st.st_uid != uid:
        die("refusing leftover owned by uid %d: %s" % (st.st_uid, p))
    os.unlink(p)

def same_identity(st_a, st_b):
    return (st_a.st_dev, st_a.st_ino) == (st_b.st_dev, st_b.st_ino)

staging = path + ".osk-new"
unlink_ours(staging)

fd = os.open(path, os.O_RDWR | os.O_NOFOLLOW | os.O_CLOEXEC)
try:
    fcntl.flock(fd, fcntl.LOCK_EX)
    st = os.fstat(fd)
    if st.st_uid != uid or not stat.S_ISREG(st.st_mode):
        sys.exit(1)
    if st.st_size > max_size:
        sys.exit(1)
    data = read_all(fd, st.st_size)
    if len(data) != st.st_size:
        die("short read of hyprland.lua")
    text = data.decode("utf-8")
    if 'require("hypr.osk")' in text:
        sys.exit(0)
    needle = 'require("hypr.gestures")'
    if needle not in text:
        sys.stderr.write("hyprland.lua has no require(\"hypr.gestures\"); not editing\n")
        sys.exit(0)
    encoded = text.replace(needle, needle + '\nrequire("hypr.osk")', 1).encode("utf-8")
    mode = stat.S_IMODE(st.st_mode)

    backup = path + ".bak-before-osk-" + time.strftime("%Y%m%dT%H%M%S")
    bfd = os.open(backup, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW | os.O_CLOEXEC, mode)
    try:
        write_all(bfd, data)
        os.fsync(bfd)
    finally:
        os.close(bfd)
    fsync_dir(backup)
    sys.stdout.write("backed up %s to %s\n" % (path, backup))
    sys.stdout.flush()

    sfd = os.open(staging, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW | os.O_CLOEXEC, mode)
    try:
        write_all(sfd, encoded)
        os.fchmod(sfd, mode)
        os.fsync(sfd)
        st_live = os.fstat(fd)
        if st_live.st_uid != uid or not stat.S_ISREG(st_live.st_mode):
            die("hyprland.lua identity changed under lock")
        if not same_identity(st, st_live):
            die("hyprland.lua inode changed under lock")
        path_st = os.lstat(path)
        if stat.S_ISLNK(path_st.st_mode) or not same_identity(st, path_st):
            die("hyprland.lua path no longer names the locked inode")
        os.rename(staging, path)
        staging = None
    finally:
        os.close(sfd)
    fsync_dir(path)
finally:
    os.close(fd)
    if staging is not None:
        try:
            unlink_ours(staging)
        except Exception:
            pass
PY
fi

# 6. Register with the shell and reload Hyprland
if [[ -x /usr/bin/omarchy-shell ]]; then
  /usr/bin/omarchy-shell shell rescanPlugins >/dev/null 2>&1 || true
  /usr/bin/omarchy-shell shell putBarWidget ekollof.osk-applet '{}' >/dev/null 2>&1 || true
  /usr/bin/omarchy-shell shell setPluginEnabled ekollof.osk true >/dev/null 2>&1 || true
fi
if [[ -x /usr/bin/hyprctl ]]; then
  /usr/bin/hyprctl reload >/dev/null 2>&1 || true
fi

# hyprctl reload does not remap an already-loaded .so (the old inode stays
# mapped). Unload by full path, then load the copy we just installed —
# only when this script built that copy (hyprpm-managed machines rebuild
# with `hyprpm update`).
if [[ "$OSK_LOCAL_BUILT" == 1 && -x /usr/bin/hyprctl ]]; then
  /usr/bin/hyprctl plugin unload "$OSK_SO" >/dev/null 2>&1 || true
  /usr/bin/hyprctl plugin load "$OSK_SO" >/dev/null 2>&1 || true
fi

echo "omarchy-osk installed."
if [[ -x /usr/bin/hyprctl ]] && /usr/bin/hyprctl plugin list | /usr/bin/grep -q hypr-osk; then
  echo "compositor plugin: loaded"
else
  echo "compositor plugin: NOT loaded yet (log out/in, or: /usr/bin/hyprctl plugin load $HOME/.local/share/hyprland/plugins/libhypr-osk.so)"
fi
if [[ -x /usr/bin/hyprctl ]] && /usr/bin/hyprctl plugin list | /usr/bin/grep -q hyprgrass; then
  echo "hyprgrass: loaded"
else
  echo "hyprgrass: built and deployed; loads at next session (or: /usr/bin/hyprctl plugin load $HOME/.local/share/hyprland/plugins/hyprgrass.so)"
fi
