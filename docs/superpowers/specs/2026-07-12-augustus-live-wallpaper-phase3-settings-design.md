# Augustus Live Wallpaper — Phase 3 (Settings) Design

Date: 2026-07-12
Status: Ready for implementation plan

Parent spec: `docs/superpowers/specs/2026-07-11-augustus-live-wallpaper-design.md`
Phase 2 spec: `docs/superpowers/specs/2026-07-12-augustus-live-wallpaper-phase2-android-design.md`
(Phases 1 and 2 are implemented and merged to `master`; the Android live wallpaper is device-verified.)

## Goal

Give the Android live wallpaper a settings screen so the user can configure it,
a config channel that carries those settings into the running wallpaper, and an
in-app "Set wallpaper" button. Modeled on h2lwp's settings (same setting names
and the file-based config channel).

## Scope & definition of done

- **Four settings:** Scale, Brightness, Map change interval, Simulation speed.
- **Config channel:** the settings screen edits the on-disk config file; the
  native wallpaper re-reads it and applies the settings when it next becomes
  visible (`UPDATE_CONFIGS`, already wired in Phase 2 as a no-op).
- **"Set wallpaper" button** firing `WallpaperManager.ACTION_CHANGE_LIVE_WALLPAPER`.
- **UI:** plain Java + AppCompat Views (matches the existing `AssetSelectionActivity`);
  no Kotlin/Compose.
- **Definition of done = the APK builds** (`./gradlew :augustus:assembleDebug`) +
  desktop regression build clean. On-device behavior is a **deferred QA pass**,
  same model as Phases 1–2.

## Settings (names/options match h2lwp where they overlap)

| Setting | Control | Config value | Applies to |
| --- | --- | --- | --- |
| **Scale** | slider | percent (e.g. 50–200; default = save's scale) | `city_view_set_scale()` |
| **Brightness** | slider | 0–100 (100 = no dimming) | translucent dark overlay alpha |
| **Map change interval** | choice | minutes: 0 = "Every switch to home screen", else 10 / 30 / 120 / 1440 | gates `city_view_go_to_random_tile()` |
| **Simulation speed** | slider/choice | Augustus game-speed value | game speed |

"Map change interval" mirrors h2lwp exactly: options **Every switch to home
screen / 10 minutes / 30 minutes / 2 hours / 24 hours**. Stored as an integer
number of minutes (0 = every switch).

## Architecture

### §A — New config keys

Add integer `CONFIG_*` keys to `src/core/config.h` (enum) and `src/core/config.c`
(`ini_keys[]` + `set_defaults()`), e.g. `CONFIG_UI_WALLPAPER_SCALE`,
`CONFIG_UI_WALLPAPER_BRIGHTNESS`, `CONFIG_UI_WALLPAPER_MAP_CHANGE_MINUTES`,
`CONFIG_UI_WALLPAPER_SPEED`. Defaults are chosen so a non-wallpaper build is
unaffected (scale default 0 = "use the save's scale"; brightness 100 = none;
map-change 0 = every switch; speed = current default). These are ordinary
Augustus config keys — cross-platform, persisted by `config_save()`, read by
`config_load()`.

### §B — Config channel (file-based, h2lwp model)

- The settings screen (Java, in the app/activity process — the native config
  system is **not** loaded there) performs a **read-modify-write of Augustus's
  `augustus.ini`**: read every `key=value` line, replace the four wallpaper
  keys (append any that are absent), preserve all other lines verbatim, write
  the file back. The file is simple `key=value\n` text (`config.c` `config_save`/
  `config_load`), so this is robust from Java and never disturbs non-wallpaper
  keys.
- On the wallpaper becoming visible, the Phase-2 path
  `SDLEngine.onVisibilityChanged(true)` → `pushWallpaperEvent(UPDATE_CONFIGS)`
  fires. Native fills in the **currently-no-op `UPDATE_CONFIGS` branch**
  (`src/platform/SDL2/augustus.c:400`) to call `config_load()` (re-reads the
  whole ini) and then apply each setting (§C). So the flow is: edit a setting →
  return to the wallpaper → it takes effect.
- **Single writer:** the activity is the only writer of the wallpaper keys; the
  wallpaper service only *reads* config (it must not `config_save()` in
  wallpaper mode, or it would rewrite the file from memory and clobber a
  concurrently-edited value). No lock needed.
- **Path agreement (implementation detail for the plan):** the activity must
  write the same `augustus.ini` path the native side reads
  (`dir_*_location(INI_FILENAME, PATH_LOCATION_CONFIG)`), which on Android
  resolves under the app's internal storage given the empty user-dir (as in
  Phase 2). The plan pins the exact path and QA verifies a written value is
  picked up.

### §C — Apply mechanisms (native, on `UPDATE_CONFIGS`, after `config_load()`)

- **Scale:** if `CONFIG_UI_WALLPAPER_SCALE > 0`, `city_view_set_scale(scale)`
  (this is the forced zoom Phase 1 deferred here); 0 keeps the save's scale.
- **Brightness:** draw a translucent black rectangle over the full viewport at
  alpha `= (100 - brightness) * 255 / 100` in the wallpaper window's foreground
  draw (`window_city_wallpaper` draw path). 100 = no overlay.
- **Map change interval:** store the interval (minutes). On the `HIDE` event,
  only call `city_view_go_to_random_tile()` if `interval == 0` (every switch)
  or at least `interval` minutes have elapsed since the last recenter (track a
  `last_recenter` timestamp — mirrors h2lwp's `lastMapUpdate`).
- **Simulation speed:** set Augustus's game speed from the config value.

### §D — Settings UI + launcher (Java, plain Views)

Extend the existing `AssetSelectionActivity` (already the launcher **and** the
wallpaper's `settingsActivity`) into the app's single home screen:

- existing asset-folder status + **Select folder** control;
- a `SeekBar` for Scale, a `SeekBar` for Brightness, a choice control
  (`Spinner`/segmented) for Map change interval, a control for Simulation speed
  — each writes its config key via §B on change (or on a Save action);
- a **Set wallpaper** button firing
  `Intent(WallpaperManager.ACTION_CHANGE_LIVE_WALLPAPER)` targeting the
  `org.libsdl.app.SDLActivity` service component, with a short note that some
  phones restrict live wallpapers (mirrors h2lwp's caveat text).

No new activities; the layout grows in `res/.../activity_asset_selection.xml`.

## Verification model

- **Per task:** the relevant build succeeds — terminal gate
  `./gradlew :augustus:assembleDebug`; shared-C changes also get a desktop
  `cmake --build build` regression build. No unit-test harness (as before).
- **Deferred device QA checklist:** set each value in the screen → return to the
  wallpaper → confirm it applies (zoom changes, map dims, map-change cadence
  follows the interval, sim speed changes); the **Set wallpaper** button opens
  the live-wallpaper chooser.

## Key file map (insertion points)

| Concern | File / anchor |
| --- | --- |
| New config keys | `src/core/config.h` (enum), `src/core/config.c` (`ini_keys[]`, `set_defaults`) |
| Apply on UPDATE_CONFIGS | `src/platform/SDL2/augustus.c:400` (the no-op branch) → `config_load()` + apply |
| Scale apply | `src/city/view.c` `city_view_set_scale()` |
| Brightness overlay | wallpaper draw path (`src/window/city.c` `window_city_wallpaper` foreground / `src/widget/city/*`) |
| Map-change-interval gate | HIDE handling in `augustus.c` (guard `city_view_go_to_random_tile()` on elapsed time) |
| Sim speed | Augustus game-speed setting |
| Settings UI + launcher | `android/augustus/src/main/java/.../AssetSelectionActivity.java`, `res/android/.../activity_asset_selection.xml`, `res/.../values/strings.xml` |
| Config file I/O (Java) | read-modify-write `augustus.ini` in internal storage |

## Risks

- **Config-file path agreement (primary).** The Java activity and native must
  read/write the same `augustus.ini`; the Android path depends on the empty
  user-dir collapse from Phase 2. Pin it in the plan; verify in QA.
- **Service must not `config_save()` in wallpaper mode** or it clobbers
  edited keys. Confirm the wallpaper path is config-read-only.
- **Apply-on-visibility only.** Settings take effect when the wallpaper next
  becomes visible (not instantly while editing) — acceptable and matches h2lwp.
- **Build-only validation.** Compiling does not prove runtime; deferred to QA.

## Out of scope

- Kotlin/Compose UI.
- Live/instant apply without a visibility cycle.
- Multiple/selectable saves (still out of scope).
- Per-setting native dialogs inside the wallpaper (settings live in the activity).

## Open questions

_None at the design layer. Exact config key names/defaults, the Android
`augustus.ini` path, and slider ranges are implementation-plan detail._
