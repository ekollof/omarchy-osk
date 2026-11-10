# Pinned build inputs

`install.sh` and meson never clone remote git at build time. Third-party
compositor code is vendored in this tree at the full commits below. Changing
a pin requires a new commit of this repository (and a new marketplace
validation of that commit).

## Hyprland (headers, not vendored)

`hypr-osk` and vendored hyprgrass compile against the *running* Hyprland's
pkg-config `hyprland` headers. Omarchy 4.5+ ships Hyprland 0.57; `hypr-osk`
supports the 0.57 compositor API, and `install.sh` requires 0.57 or newer
headers. A Hyprland upgrade therefore only needs a rebuild; no source change.

| Item | Value |
|---|---|
| Package / tag | Hyprland `v0.57.0` or newer |
| Commit `v0.57.0` | `0aa6745f705f2c4b0a5004eb019ef76e5bbdb426` |
| Source | https://github.com/hyprwm/Hyprland |

The optional hyprpm route (`hyprpm.toml` `commit_pins`) maps a running
Hyprland commit to a last-known-good hypr-osk plugin commit:

`0aa6745f705f2c4b0a5004eb019ef76e5bbdb426` → `47714a4e37c9c5ef5ab900ec761f5b70fb038061`

The 0.57 pin is the dual-API build with the hardware/capability pad
classification. It does not carry later unload or install changes; the
primary `install.sh` path builds the current tree. The retired 0.56 route is
not supported.

The pinned source includes the packaged-quickshell executable check, pinned
omarchy-shell instance, and mapped OSK layer requirement for injection. When
updating this pin, review the selected source as well as the manifest: hyprpm
builds the pinned commit rather than the current checkout.

`./install.sh` is the primary path and builds whatever is in this tree. It
reconfigures an existing build directory and wipes it when the pkg-config
Hyprland version changes, so a Hyprland upgrade cannot reuse objects compiled
against the previous API.

## hyprgrass (vendored)

Edge-swipe only. Typing, taps, and scrolling work without it.

| Tree | Tag | Commit | wf-touch |
|---|---|---|---|
| `vendor/hyprgrass-0.57/` | `hl-0.57.0` (tag object `dd6f08bb3337113cf00b60fdf8cd5c6dae25edb4`) | `8ca1af717d5b4ea1215a781df52bcef02d7b15e6` | `8974eb0f6a65464b63dd03b842795cb441fb6403` |

The tree is `v0.8.2` plus its Hyprland 0.57 API chase. `install.sh` always
uses this tree. The 0.56 tree was retired with the 0.56 compositor API.

hyprgrass upstream chases Hyprland's internal API. This tree is pinned for
the 0.57 generation; a future compositor API needs a fresh vendored tree and
an `install.sh` update.

## wf-touch (vendored with hyprgrass)

| Item | Value |
|---|---|
| Source | https://github.com/WayfireWM/wf-touch |
| Commit | `8974eb0f6a65464b63dd03b842795cb441fb6403` |
| Trees | `vendor/hyprgrass-0.57/subprojects/wf-touch/` |
| License | MIT (`subprojects/wf-touch/LICENSE` in each tree) |
| Extra system dep | `glm` (must already be installed; used only by this subproject) |

## hypr-osk (first-party compositor plugin)

| Item | Value |
|---|---|
| Tree | `hypr-osk/` |
| Build | meson + ninja, C++23 |
| API support | Hyprland 0.57 |
| meson `dependency()` | `hyprland` (≥ 0.57), `pixman-1`, `libinput`, `wayland-server`, `xkbcommon` (≥ 1.0), `libdrm` |

## Runtime (not compiled in)

Omarchy already ships these: quickshell ≥ 0.3 (`omarchy-shell`), `localectl`,
xkb data under `/usr/share/X11/xkb`.

## Re-vendoring hyprgrass

hyprgrass chases Hyprland internals, so a future compositor generation needs
its own vendored tree. The 0.57 tree lives in `vendor/hyprgrass-0.57/`; later
generations use `vendor/hyprgrass-<major>.<minor>/`. For a future Hyprland,
pin the matching upstream `hl-<version>` tag (or a full commit) and record both
SHAs here in the same commit as the tree:

```bash
git clone https://github.com/horriblename/hyprgrass /tmp/hyprgrass
git -C /tmp/hyprgrass checkout --detach <hl-tag-or-commit>
git -C /tmp/hyprgrass submodule update --init --depth 1
# confirm wf-touch: git -C /tmp/hyprgrass/subprojects/wf-touch rev-parse HEAD
rsync -a --delete \
  --exclude .git --exclude build --exclude 'subprojects/.wraplock' \
  /tmp/hyprgrass/ vendor/hyprgrass-<major>.<minor>/
meson setup vendor/hyprgrass-<major>.<minor>/build vendor/hyprgrass-<major>.<minor>
meson compile -C vendor/hyprgrass-<major>.<minor>/build
```

Then update `install.sh`'s `GRASS_DIR` and minimum Hyprland version for the new
generation. A Hyprland version gap without a matching tree shows up as a
**build** failure (missing headers), not a runtime crash.
