# Pinned build inputs

`install.sh` and meson never clone remote git at build time. Third-party
compositor code is vendored in this tree at the full commits below. Changing
a pin requires a new commit of this repository (and a new marketplace
validation of that commit).

## Hyprland (headers, not vendored)

`hypr-osk` and vendored hyprgrass compile against the *running* Hyprland's
pkg-config `hyprland` headers. `hypr-osk` supports both the 0.56 and 0.57
compositor APIs from one source tree (it selects the API from the installed
headers), and `install.sh` selects the matching vendored hyprgrass tree. A
Hyprland upgrade therefore only needs a rebuild; no source change.

| Item | Value |
|---|---|
| Package / tag | Hyprland `v0.56.2` or `v0.57.0` |
| Commit `v0.56.2` | `efb50993780079460b0cbed1363e2166a2de1d9f` |
| Commit `v0.57.0` | `0aa6745f705f2c4b0a5004eb019ef76e5bbdb426` |
| Source | https://github.com/hyprwm/Hyprland |

The optional hyprpm route (`hyprpm.toml` `commit_pins`) maps a running
Hyprland commit to a last-known-good hypr-osk plugin commit. Both pins build
the same dual-API source:

`efb50993780079460b0cbed1363e2166a2de1d9f` → `556a2f421a540c4bb5dfcfc5ea2515e22942c55a`
`0aa6745f705f2c4b0a5004eb019ef76e5bbdb426` → `47714a4e37c9c5ef5ab900ec761f5b70fb038061`

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
| `vendor/hyprgrass/` | `hl-0.56.1` (tag object `1a8e258f33d44959468f65087dd9f0c789fe99d0`) | `36df29f57f94a77b4d5dcf91100f620a46663fa9` | `8974eb0f6a65464b63dd03b842795cb441fb6403` |
| `vendor/hyprgrass-0.57/` | `hl-0.57.0` (tag object `dd6f08bb3337113cf00b60fdf8cd5c6dae25edb4`) | `8ca1af717d5b4ea1215a781df52bcef02d7b15e6` | `8974eb0f6a65464b63dd03b842795cb441fb6403` |

Both trees are `v0.8.2` plus their Hyprland API chase. `install.sh` uses the
0.57 tree when `pkg-config --atleast-version=0.57.0 hyprland` succeeds and the
0.56 tree otherwise.

hyprgrass upstream chases Hyprland's internal API, so a single tree does not
span both generations. Vendoring each supported generation keeps the
upgrade path to a rebuild instead of a source edit.

## wf-touch (vendored with hyprgrass)

| Item | Value |
|---|---|
| Source | https://github.com/WayfireWM/wf-touch |
| Commit | `8974eb0f6a65464b63dd03b842795cb441fb6403` |
| Trees | `vendor/hyprgrass/subprojects/wf-touch/`, `vendor/hyprgrass-0.57/subprojects/wf-touch/` |
| License | MIT (`subprojects/wf-touch/LICENSE` in each tree) |
| Extra system dep | `glm` (must already be installed; used only by this subproject) |

## hypr-osk (first-party compositor plugin)

| Item | Value |
|---|---|
| Tree | `hypr-osk/` |
| Build | meson + ninja, C++23 |
| API support | Hyprland 0.56 and 0.57, selected at compile time with `__has_include` |
| meson `dependency()` | `hyprland` (≥ 0.56), `pixman-1`, `libinput`, `wayland-server`, `xkbcommon` (≥ 1.0), `libdrm` |

## Runtime (not compiled in)

Omarchy already ships these: quickshell ≥ 0.3 (`omarchy-shell`), `localectl`,
xkb data under `/usr/share/X11/xkb`.

## Re-vendoring hyprgrass

hyprgrass chases Hyprland internals, so each supported Hyprland generation
gets its own vendored tree. The 0.56 generation lives in `vendor/hyprgrass/`;
0.57 and later generations use `vendor/hyprgrass-<major>.<minor>/`. For a future
Hyprland, pin the matching upstream `hl-<version>` tag (or a full commit) and
record both SHAs here in the same commit as the tree:

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

Then extend the version selection in `install.sh` so the new tree is used for
its Hyprland generation. A Hyprland version gap without a matching tree shows
up as a **build** failure (missing headers), not a runtime crash.
