# Pinned build inputs

`install.sh` and meson never clone remote git at build time. Third-party
compositor code is vendored in this tree at the full commits below. Changing
a pin requires a new commit of this repository (and a new marketplace
validation of that commit).

## Hyprland (headers, not vendored)

`hypr-osk` and vendored hyprgrass compile against the *running* Hyprland's
pkg-config `hyprland` headers. Rebuild after a Hyprland upgrade.

| Item | Value |
|---|---|
| Package / tag | Hyprland `v0.56.2` |
| Commit | `efb50993780079460b0cbed1363e2166a2de1d9f` |
| Source | https://github.com/hyprwm/Hyprland |

The optional hyprpm route (`hyprpm.toml` `commit_pins`) maps that Hyprland
commit to a last-known-good hypr-osk plugin commit:

`efb50993780079460b0cbed1363e2166a2de1d9f` → `d64eeb9b32871ede4eb4f83c582c45929300a56e`

`./install.sh` is the primary path and builds whatever is in this tree.

## hyprgrass (vendored)

Edge-swipe only. Typing, taps, and scrolling work without it.

| Item | Value |
|---|---|
| Source | https://github.com/horriblename/hyprgrass |
| Tag | `hl-0.56.1` (annotated tag object `1a8e258f33d44959468f65087dd9f0c789fe99d0`) |
| Commit | `36df29f57f94a77b4d5dcf91100f620a46663fa9` |
| `VERSION` | `v0.8.2` |
| Tree | `vendor/hyprgrass/` |
| License | BSD 3-Clause (`vendor/hyprgrass/LICENSE`) + AOSP (`vendor/hyprgrass/LICENSE.aosp`) |

hyprgrass HEAD already requires Hyprland newer than 0.56.x (it includes
`hyprland/src/keybinds/Manager.hpp`, absent from 0.56.2 headers) and
upstream `commit_pins` had no 0.56.2 entry. The vendored tag is the
offline-capable pin for this Hyprland.

## wf-touch (vendored with hyprgrass)

| Item | Value |
|---|---|
| Source | https://github.com/WayfireWM/wf-touch |
| Commit | `8974eb0f6a65464b63dd03b842795cb441fb6403` |
| Tree | `vendor/hyprgrass/subprojects/wf-touch/` |
| License | MIT (`vendor/hyprgrass/subprojects/wf-touch/LICENSE`) |
| Extra system dep | `glm` (`omarchy pkg add glm` if missing; used only by this subproject) |

## hypr-osk (first-party compositor plugin)

| Item | Value |
|---|---|
| Tree | `hypr-osk/` |
| Build | meson + ninja, C++23 |
| meson `dependency()` | `hyprland`, `pixman-1`, `libinput`, `wayland-server`, `xkbcommon` (≥ 1.0), `libdrm` |

## Runtime (not compiled in)

Omarchy already ships these: quickshell ≥ 0.3 (`omarchy-shell`), `localectl`,
xkb data under `/usr/share/X11/xkb`.

## Re-vendoring hyprgrass

For a future Hyprland, pin the matching upstream `hl-<version>` tag (or a
full commit) and record both SHAs here in the same commit as the tree:

```bash
git clone https://github.com/horriblename/hyprgrass /tmp/hyprgrass
git -C /tmp/hyprgrass checkout --detach 36df29f57f94a77b4d5dcf91100f620a46663fa9
git -C /tmp/hyprgrass submodule update --init --depth 1
# confirm wf-touch: git -C /tmp/hyprgrass/subprojects/wf-touch rev-parse HEAD
rsync -a --delete \
  --exclude .git --exclude build --exclude 'subprojects/.wraplock' \
  /tmp/hyprgrass/ vendor/hyprgrass/
meson setup vendor/hyprgrass/build vendor/hyprgrass
meson compile -C vendor/hyprgrass/build
```

A Hyprland version gap shows up as a **build** failure (missing headers),
not a runtime crash.
