# Augustus Live Wallpaper — Design

Date: 2026-07-11
Status: Ready for implementation plan
**Plan-confidence status:** Ready for implementation plan (iteration 2, confidence 92%)

Convert Augustus (a Caesar III city-builder, C + SDL2) into an Android Live
Wallpaper that autoloads a bundled city save, renders the city map full-screen
with no UI chrome, keeps the simulation running ("living city"), holds the
camera static while visible, and moves the camera to a new location each time
the wallpaper is hidden (so each reveal shows a different part of the city).

The approach is modeled on **h2lwp**, a fork of `fheroes2` that ships as a live
wallpaper. Reference paths in this document use `h2lwp:` for that fork and plain
paths for Augustus.

## Reference: how h2lwp works (validated)

- **Android hosting:** does not add a separate service. It vendors SDL's
  `org/libsdl/app/*` into the app module and rewrites `SDLActivity` from
  `extends Activity` to `extends WallpaperService`, keeping the class name so all
  `Java_org_libsdl_app_SDLActivity_*` JNI callbacks still resolve. Manifest
  declares it as a `<service>` with `BIND_WALLPAPER` and the
  `android.service.wallpaper.WallpaperService` intent-filter + `@xml/livewallpaper`
  meta-data. The SDL render surface is rebound by overriding `getNativeSurface()`
  to return the wallpaper `Engine`'s `SurfaceHolder.getSurface()`. The SDL main
  thread is started from `Engine.onSurfaceChanged`; `onVisibilityChanged` drives
  pause/resume.
- **Native↔Java bridge:** one JNI export `pushWallpaperEvent(int)` →
  `SDL_USEREVENT` with codes `HIDE / UPDATE_CONFIGS / RESIZE_DISPLAY`; native
  replies with `SDL_AndroidSendMessage(COMMAND_PAUSE_NOW)`.
- **Engine changes:** `main()` calls a new `Game::Wallpaper()` instead of the
  menu loop; audio init dropped; display sized from device DPI. A new
  `game_wallpaper.cpp` autoloads a map and renders the stock adventure-map draw
  path with UI levels stripped, into a borderless full-screen area. UI hidden via
  `Settings::OverrideSettingsForLiveWallpaper()` + only drawing the borderless
  game area. Camera "scroll" = re-center on a new random tile on the `HIDE`
  event.
- **Settings:** Jetpack Compose screen edits the on-disk config file; native
  re-reads it on `UPDATE_CONFIGS` (fired when the wallpaper becomes visible).

## Behavior (decided)

- **Content:** a **bundled demo save** shipped in the APK. It still renders using
  the user's original Caesar III graphics/data (the user must have supplied the
  C3 data folder, same constraint h2lwp has with HoMM2 assets).
- **Camera motion:** **static while visible.** Camera position changes only when
  the wallpaper is hidden; on the next show it displays a new location. (Mirrors
  h2lwp's `Hide` → re-center model.)
- **Simulation:** **living city** — time advances, figures move, buildings
  progress. Requires the wallpaper window to be treated as a city-type window
  (see below).
- **Interaction:** none (non-interactive wallpaper; touches are ignored).
- **Power posture:** the city simulation runs only while the wallpaper is
  visible; native pauses both ticks and rendering when hidden (the
  `HIDE` → `COMMAND_PAUSE_NOW` path). No extra fps cap beyond Augustus's existing
  frame timing.

## Packaging & mode selection

- **Single APK** contains both the full game and the wallpaper service. There is
  no separate wallpaper-only build variant or `applicationId`; the same
  `libaugustus.so` and Java sources serve both.
- **Mode is chosen at runtime:** the `WallpaperService`'s `SDLMain` passes a
  `--wallpaper` argument via `getArguments()`, which `platform_parse_arguments`
  parses into the `args` struct; the launcher `AugustusMainActivity` starts the
  game normally (no flag). The startup branch at `augustus.c:686` reads this flag.

## Constraints

- Augustus requires original Caesar III data files. On Android these are accessed
  through a Storage Access Framework (SAF) folder URI the user selects, via
  `FileManager.java` + `src/platform/android/*`. The wallpaper reuses this
  existing asset/data pipeline — we do **not** build a parallel asset system.
- Android build uses **SDL2** (the default; the Gradle scripts auto-select SDL2
  unless SDL3 AARs are present). SDL3 is out of scope.
- **In-flight / upstream:** work targets local `master` (branch
  `feature/live-wallpaper`). There is no wallpaper-related in-flight work; the
  numerous `upstream/*` feature branches are out of scope and no rebase onto them
  is planned for v1.
- SDL's Android backend assumes a single `SDLActivity` singleton and a valid
  context via `SDL_AndroidGetActivity()`. The hosting approach must keep that
  satisfiable from a service.

## Architecture

Three layers, built and validated in phases.

### 1. Native wallpaper mode (desktop-testable)

- **Entry branch.** Add an `args->wallpaper` flag (set by `--wallpaper`, parsed in
  `platform_parse_arguments`) consumed at the startup-decision point
  `src/platform/SDL2/augustus.c:686`
  (`args->launch_asset_previewer ? window_asset_previewer_show() : game_init()`).
  In wallpaper mode, run the asset-loading portion of `game_init()` (graphics,
  fonts, model, sound, `game_state_init()`), then load the bundled save and show
  the wallpaper city window instead of `window_logo_show()`.
- **Autoload.** Reuse the existing loader:
  `game_file_load_saved_game(path)` → on `FILE_LOAD_SUCCESS` →
  `window_city_wallpaper_show()`. This is the sequence at
  `src/window/file_dialog.c:685-689`. The bundled save is delivered through
  Augustus's existing file/asset mechanism (copied into the savegame location on
  first run, then loaded normally) rather than a new code path.
- **Map-only window.** Add `window_city_wallpaper_show()` in `src/window/city.c`
  registering a `window_type` whose callbacks draw **only** the map — call
  `widget_city_draw()` and nothing else. Omit `widget_top_menu_draw()` and
  `widget_sidebar_city_draw_background/foreground()` (this also removes the
  minimap, which is part of the sidebar). `handle_input` is a no-op so touches do
  nothing.
- **Living-city tick gating.** `game_speed_get_elapsed_ticks()`
  (`src/game/speed.c:48-58`) returns 0 (no simulation tick) unless
  `window_get_id()` is a city-type window. Add a dedicated
  **`WINDOW_CITY_WALLPAPER`** id and include it in that switch, and in
  `window_city_is_window_cityview()` (`src/window/city.c:79`) and any
  terrain-related predicate that must recognize the city view. Using a dedicated
  id (rather than reusing `WINDOW_CITY`) keeps standard city input/UI code from
  firing against the wallpaper window.
- **Fullscreen viewport.** The default viewport reserves 24px top and 40/160px
  right for chrome (`set_viewport_with/without_sidebar()` in `src/city/view.c`).
  Add `set_viewport_wallpaper()` (x=0, y=0, width=`screen_width`,
  height=`screen_height`) and select it from `city_view_set_viewport()` /
  `city_view_set_scale()` when wallpaper mode is active.
- **Camera on hide.** While visible, do not move the camera. Re-centering is
  driven by a single internal "became hidden" hook, mapped per platform: on
  desktop to `SDL_WINDOWEVENT_HIDDEN` / `FOCUS_LOST`, on Android to the `HIDE`
  wallpaper event — one code path for both. On that hook, re-center on a new valid
  tile via `city_view_go_to_grid_offset()` or `city_view_set_camera()`, clamped by
  the existing `check_camera_boundaries()`. This makes the behavior testable on
  desktop in Phase 1 (minimize/refocus the window) without a debug-only trigger.
- **Disable autosave.** In wallpaper mode, guard `setting_monthly_autosave()` and
  the yearly autosave call (`src/game/tick.c:117`,
  `game_file_make_yearly_autosave`) so the running "living city" never overwrites
  the bundled save or writes to the user's save directory.
- **Frame loop.** No new render loop needed. The existing
  `run_and_draw()`/`main_loop()` (`augustus.c:152,406`) already draws every frame
  and ticks the sim per `game_run()`. When hidden, the native loop pauses ticks
  and rendering (`HIDE` → `COMMAND_PAUSE_NOW`).

### 2. Android WallpaperService hosting (Approach 1)

- **Vendor SDL Java.** Copy `org/libsdl/app/*` into the Augustus app module
  (currently pulled from the SDL2 distribution via
  `android/SDL2/build.gradle`), and modify the SDL activity/service layer so the
  wallpaper entry `extends WallpaperService`. Override `getNativeSurface()` to
  return the `Engine`'s `SurfaceHolder.getSurface()`; start the SDL thread from
  `Engine.onSurfaceChanged`; drive resume/pause from `Engine.onVisibilityChanged`
  and surface create/destroy. Keep the existing `AugustusMainActivity` for
  first-run setup and the C3-data-folder (SAF) picker.
- **Manifest.** Add the `<service>` (`BIND_WALLPAPER`,
  `android.service.wallpaper.WallpaperService` intent-filter, `@xml/livewallpaper`
  meta-data, `<uses-feature android:name="android.software.live_wallpaper"/>`).
- **JNI event channel.** Add `pushWallpaperEvent(int)` (native export) →
  `SDL_USEREVENT`; handle `HIDE` (re-center camera, allow pause),
  `UPDATE_CONFIGS` (re-read config), `RESIZE_DISPLAY` (resize) in the native
  loop. Native→Java pause via `SDL_AndroidSendMessage(COMMAND_PAUSE_NOW)`.
- **Assets — reuse existing pipeline.** The C3 graphics/data continue to load
  through Augustus's existing `FileManager` + `src/platform/android/*` + SAF. The
  only adaptation is ensuring those calls work from the service context (SAF
  persisted-URI grants are app-wide, so the existing `ContentResolver`-based
  access is reusable; use the application/service context where the current code
  assumes an activity). The bundled save is delivered via the existing asset/file
  APIs.
- **Process re-init caveat.** SDL cannot cleanly re-init in-process; h2lwp calls
  `Process.killProcess()` on wallpaper re-selection. Adopt the same guard.

### 3. Settings (start minimal, grow)

- Reuse Augustus's existing config system (`config_get`/`config_set`) with new
  `CONFIG_*` keys (`src/core/config.h`) for wallpaper settings; the settings UI
  edits them; native re-reads on `UPDATE_CONFIGS`. No JNI argument plumbing for
  config (config-system channel, analogous to h2lwp's file-based channel).
- A launcher entry offers "Set wallpaper" firing
  `WallpaperManager.ACTION_CHANGE_LIVE_WALLPAPER` for the service component.
- Initial settings kept small (e.g. render scale; optionally brightness).

## Phasing

1. **Phase 1 — Native mode.** `--wallpaper` flag, bundled-save autoload
   (copy-to-savegame on first run), autosave disabled, `WINDOW_CITY_WALLPAPER`
   map-only fullscreen window, living city, camera re-center on the "became
   hidden" hook (desktop: `SDL_WINDOWEVENT_HIDDEN`/`FOCUS_LOST`). Validated on
   **desktop**.
2. **Phase 2 — Android hosting.** Vendor + rewrite SDL layer into a
   `WallpaperService`, surface binding, manifest, JNI event channel, bundled
   save + reuse of the existing SAF asset pipeline from the service context.
   Validated on **device/emulator**.
3. **Phase 3 — Settings.** Settings UI + config channel +
   `ACTION_CHANGE_LIVE_WALLPAPER` launcher.

Each phase is independently testable and mergeable.

## Risks

- **Service context for SAF assets (primary augustus-specific risk).** Augustus
  does heavy runtime file I/O through Java `FileManager` using the activity
  context; the service has no activity. Mitigation: use the application context +
  app-wide persisted SAF grants; reuse existing code, adapt context only.
- **SDL re-init in-process.** Decision: adopt h2lwp's guard — on wallpaper
  re-selection with a live SDL thread, call `nativeQuit()` then
  `Process.killProcess()` and let Android restart the service (SDL cannot cleanly
  re-init in-process).
- **Living city drift.** A running simulation mutates the loaded save state over
  time. Decision: the bundled save is reloaded on each start, and monthly/yearly
  autosave is disabled in wallpaper mode (see "Disable autosave"), so the bundle
  is never written back.
- **Tick gating regressions.** Adding `WINDOW_CITY_WALLPAPER` to city predicates
  must not enable standard city input/menus for the wallpaper window.

## Out of scope

- Interactivity / touch handling.
- Home-screen parallax (offset) scrolling.
- Multiple/user-selectable saves (bundled save only for v1).
- SDL3 Android build.
- Audio.

## Key file map (insertion points)

| Concern | File / anchor |
| --- | --- |
| Startup branch | `src/platform/SDL2/augustus.c:686` (`setup()`), `:152` `run_and_draw()`, `:406` `main_loop()` |
| Load save → city | `src/game/file.c` `game_file_load_saved_game()`; pattern `src/window/file_dialog.c:685` |
| Map-only window | `src/window/city.c` (new `window_city_wallpaper_show()`, `:79` predicate) |
| Map draw call | `src/widget/city/city.c` `widget_city_draw()` |
| Fullscreen viewport | `src/city/view.c` `set_viewport_*`, `city_view_set_viewport()` |
| Camera re-center | `src/city/view.c` `city_view_go_to_grid_offset()` / `city_view_set_camera()` |
| Sim tick gating | `src/game/speed.c:48-58` (`window_get_id()` switch) |
| Android JNI bridge | `src/platform/android/android.c`, `src/platform/SDL2/android/android.c`, `jni.c/.h` |
| SDL Java layer | vendored `org/libsdl/app/*` (from `android/SDL2/`) into the app module |
| Manifest | `android/augustus/src/main/AndroidManifest.xml` |
| Asset/SAF | `FileManager.java`, `src/platform/android/asset_handler.c` |
| Config/settings | `src/core/config.h` (`config_get`/`config_set`, `CONFIG_*`) |
| Autosave (drift) | `src/game/tick.c:117` (`setting_monthly_autosave` → `autosave.svx`), `game_file_make_yearly_autosave` |
| Arg parsing | `platform_parse_arguments()`; `args->launch_asset_previewer` at `augustus.c:686` |
| Android build target | `android/build.gradle` / `settings.gradle` auto-select SDL2 unless SDL3 AARs present; single `applicationId`, no product flavors |

## Confidence Survey

Edit checkboxes in-place to answer. Mark exactly one option per question with `[x]`. The option labeled *(Recommended)* is the skill's best guess given current plan + repo context — override freely.

_No open questions — all iteration-1 questions were answered and folded into the plan body (see Reconciliation Log)._

## Reconciliation Log

Append-only. Newest entry at the bottom.

### Iteration 1 — 2026-07-11
- **Confidence:** 75% (cap from Readiness / open architectural tradeoffs; universal in-flight cap 85%)
- **Resolved:** none (first pass)
- **Still uncertain:** packaging & mode-selection (Dim 1), power posture of a continuously-ticking living city (Dim 2), several open architectural tradeoffs — save delivery, autosave, SAF service context, settings channel, re-init (Dim 3)
- **New questions:** Q1.1 … Q1.10

### Iteration 2 — 2026-07-11
- **Confidence:** 92% (all iteration-1 caps lifted; no dimension below 90%)
- **Resolved:**
  - Q1.1 → single APK, runtime mode selection → new "Packaging & mode selection"
  - Q1.2 → `--wallpaper` arg via `getArguments`/`platform_parse_arguments` → Packaging + Arch §1 Entry branch
  - Q1.3 → bundle `.svx`, copy to savegame on first run, load normally → Arch §1 Autoload (confirmed)
  - Q1.4 → disable monthly+yearly autosave in wallpaper mode → Arch §1 "Disable autosave" + Risks
  - Q1.5 → single "became hidden" hook (desktop `WINDOW_HIDDEN`/`FOCUS_LOST`, Android `HIDE`) → Arch §1 Camera on hide + Phasing P1
  - Q1.6 → reuse persisted SAF grant via application context → Arch §2 Assets + Risks (confirmed)
  - Q1.7 → reuse `config_get`/`config_set` + new `CONFIG_*` keys → Arch §3 Settings
  - Q1.8 → h2lwp `Process.killProcess()` re-init guard → Risks
  - Q1.9 → SDL2 on local `master`, upstream out of scope → Constraints (in-flight note)
  - Q1.10 → living city visible, native pauses ticks+render when hidden, no fps cap → Behavior "Power posture" + Arch §1 Frame loop
- **Still uncertain:** none material at the tech-spec layer; remaining detail is implementation-plan granularity (task breakdown, test placement)
- **New questions:** none
