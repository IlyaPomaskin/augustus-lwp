# Augustus Live Wallpaper — Design

Date: 2026-07-11
Status: Approved (design), pending implementation plan

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

## Constraints

- Augustus requires original Caesar III data files. On Android these are accessed
  through a Storage Access Framework (SAF) folder URI the user selects, via
  `FileManager.java` + `src/platform/android/*`. The wallpaper reuses this
  existing asset/data pipeline — we do **not** build a parallel asset system.
- Android build uses **SDL2** (the default). SDL3 is out of scope.
- SDL's Android backend assumes a single `SDLActivity` singleton and a valid
  context via `SDL_AndroidGetActivity()`. The hosting approach must keep that
  satisfiable from a service.

## Architecture

Three layers, built and validated in phases.

### 1. Native wallpaper mode (desktop-testable)

- **Entry branch.** Add a wallpaper mode flag consumed at the startup-decision
  point `src/platform/SDL2/augustus.c:686`
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
- **Camera on hide.** While visible, do not move the camera. On the hide trigger
  (Phase 1: a debug key/timer; Phase 2: the `HIDE` wallpaper event), re-center on
  a new valid tile via `city_view_go_to_grid_offset()` or
  `city_view_set_camera()`, clamped by the existing `check_camera_boundaries()`.
- **Frame loop.** No new render loop needed. The existing
  `run_and_draw()`/`main_loop()` (`augustus.c:152,406`) already draws every frame
  and ticks the sim per `game_run()`.

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

- Reuse Augustus's existing config mechanism (`config_*` / settings file) for
  wallpaper keys; the settings UI edits it; native re-reads on `UPDATE_CONFIGS`.
  No JNI argument plumbing for config (file-based channel, like h2lwp).
- A launcher entry offers "Set wallpaper" firing
  `WallpaperManager.ACTION_CHANGE_LIVE_WALLPAPER` for the service component.
- Initial settings kept small (e.g. render scale; optionally brightness).

## Phasing

1. **Phase 1 — Native mode.** `--wallpaper` flag, bundled-save autoload,
   `WINDOW_CITY_WALLPAPER` map-only fullscreen window, living city, camera
   re-center on a trigger (debug key/timer). Validated on **desktop**.
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
- **SDL re-init in-process.** Mitigate with the `Process.killProcess()` guard on
  re-selection (h2lwp precedent).
- **Living city drift.** A running simulation mutates the loaded save state over
  time; since we reload the bundled save on start this is acceptable, but confirm
  no autosave writes back over the bundle.
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
