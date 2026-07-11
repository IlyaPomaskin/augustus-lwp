# Augustus Live Wallpaper — Phase 2 (Android Hosting) Design

Date: 2026-07-12
Status: Ready for implementation plan

Parent spec: `docs/superpowers/specs/2026-07-11-augustus-live-wallpaper-design.md`
(Phase 1, native `--wallpaper` mode, is implemented and merged to `master`.)

## Goal

Ship an Android APK that installs as a **live wallpaper**: selected from the
system wallpaper picker, it boots straight into the Phase-1 wallpaper path,
renders the Caesar III city map full-screen with no UI chrome, keeps the
simulation running while visible, pauses when hidden, and re-centers the camera
to a new area each time it becomes visible again.

The Android hosting is modeled on **h2lwp** (an `fheroes2` fork shipping as a
live wallpaper): vendor SDL's Java layer and make the SDL host a
`WallpaperService`.

## Scope & definition of done

- **This is a wallpaper-only APK.** The playable game launcher is **dropped**
  for this build. This intentionally **amends** the parent spec's "single APK
  contains both the full game and the wallpaper service" — the combined
  game+wallpaper APK is deferred to a later phase.
- **Definition of done for Phase 2 = the APK builds.** `./gradlew
  :augustus:assembleDebug` produces a debug-signed APK with the wallpaper
  service wired to the Phase-1 `--wallpaper` entry point.
- **On-device behavior is a deferred QA pass.** No device is connected during
  implementation. Each task's verification is a successful Gradle build; a
  written device-QA checklist (below) is run later when a device is attached.
  This mirrors Phase 1's model (build per task; behavioral QA consolidated in a
  final manual pass).

## Reuse from Phase 1 (already on `master`)

These exist and are consumed unchanged by Phase 2:

- `int game_wallpaper_mode(void)` / `game_set_wallpaper_mode(int)` — single
  source of truth (`src/game/game.c`).
- `int game_init_wallpaper(void)` — loads `wallpaper.svx` from
  `PATH_LOCATION_SAVEGAME`, shows the map-only window, randomizes the first
  frame; audio init skipped (silent).
- `WINDOW_CITY_WALLPAPER` map-only window; sim tick-gating; full-screen
  viewport (`set_viewport_wallpaper`).
- `void city_view_go_to_random_tile(void)` — the "became hidden" re-center
  helper. Phase 1 wired it to desktop `FOCUS_LOST`/`HIDDEN`; Phase 2 wires it to
  the Android `HIDE` event via the **same** helper.
- `--wallpaper` CLI flag parsed by `platform_parse_arguments` and consumed at
  the `setup()` startup branch in `src/platform/SDL2/augustus.c`.
- Cursor hidden in wallpaper mode.

## Behavior (decided; inherited from parent spec)

- **Content:** a bundled Augustus-format `wallpaper.svx` shipped in the APK,
  rendered with the user's own Caesar III graphics/data.
- **Camera:** static while visible; re-centers on a new random valid tile each
  time the wallpaper becomes hidden (`HIDE`).
- **Simulation:** living city while visible; ticks + rendering paused when
  hidden.
- **Interaction:** none (touches ignored).
- **Audio:** none (Phase 1 skips audio init).

## Architecture

### §A — Build/toolchain provisioning (first task; de-risks the rest)

The local toolchain is present and version-matched to the build:

- Android SDK at `~/Library/Android/sdk`; NDK `29.0.14206865`; SDK CMake
  `4.1.2`; `adb`; JDK via Android Studio's bundled JBR
  (`/Applications/Android Studio.app/Contents/jbr/Contents/Home`).

Two gaps block any build and are fixed first:

1. **`android/local.properties`** with `sdk.dir=/Users/ilyapomaskin/Library/Android/sdk`.
   Gradle is pointed at the JBR via `org.gradle.java.home` (or `JAVA_HOME`) so
   `./gradlew` runs without a system JDK on `PATH`.
2. **SDL2 + SDL2_mixer sources** extracted into `ext/SDL2/` as
   `ext/SDL2/SDL2-2.0.x/` and `ext/SDL2/SDL2_mixer-2.0.x/` (the `:SDL2` Gradle
   module builds SDL from there; currently only a stub `Android.mk` is present
   and there are no prebuilt AARs). Use an SDL2 2.x release matching what the
   desktop/CI build uses (desktop currently links SDL2 2.32.10) and a matching
   SDL2_mixer 2.x.

**Gate:** a baseline `./gradlew :augustus:assembleDebug` of the **unmodified**
tree must succeed before any wallpaper changes, proving the toolchain (not our
code) when later builds are green.

### §B — SDL Java vendoring + `WallpaperService` (h2lwp-style)

- **Vendor** `org/libsdl/app/*` from the SDL2 source into the app module
  (`android/augustus/src/main/java/org/libsdl/app/`) so the SDL Java layer is
  under our control instead of pulled verbatim from the SDL distribution.
- **Rewrite the SDL host** so the entry `extends WallpaperService`. Keep the
  class name `org.libsdl.app.SDLActivity` so all
  `Java_org_libsdl_app_SDLActivity_*` JNI callbacks still resolve.
- Override `getNativeSurface()` to return the wallpaper `Engine`'s
  `SurfaceHolder.getSurface()`. Start the SDL native thread from
  `Engine.onSurfaceChanged`; drive resume/pause and the `HIDE` event from
  `Engine.onVisibilityChanged` and surface create/destroy.
- `getArguments()` returns `{"--wallpaper"}` so the native `setup()` branch
  takes the Phase-1 wallpaper path.

### §C — Manifest

- **Remove** the `MAIN`/`LAUNCHER` game activity (`AugustusMainActivity`).
- **Add** a `<service>` with `android:permission="android.permission.BIND_WALLPAPER"`,
  the `android.service.wallpaper.WallpaperService` intent-filter,
  `@xml/livewallpaper` meta-data, and
  `<uses-feature android:name="android.software.live_wallpaper" android:required="true"/>`.
- **Keep a minimal `DirectorySelectionActivity`** (exported, non-game) for the
  Storage Access Framework (SAF) folder grant — the wallpaper cannot render
  without the C3 data folder, and a service cannot launch the SAF picker
  itself. With no launcher icon, this activity is reached via the
  live-wallpaper **settings hook**: `@xml/livewallpaper` declares
  `android:settingsActivity` pointing at `DirectorySelectionActivity`, so the
  wallpaper picker's "Settings" button opens the SAF grant. The wallpaper
  itself is selected from Android's system wallpaper picker. (A dedicated
  in-app "set wallpaper" button and a full settings UI are Phase 3.)

### §D — Native ↔ Java bridge

- Add JNI export `pushWallpaperEvent(int)` → posts an `SDL_USEREVENT` with a
  code. Codes handled by the native event loop:
  - **`HIDE`** → call `city_view_go_to_random_tile()` (the Phase-1 helper) and
    allow pause; native replies with `SDL_AndroidSendMessage(COMMAND_PAUSE_NOW)`
    so ticks + rendering stop while hidden.
  - **`RESIZE_DISPLAY`** → resize the display to the surface dimensions.
  - (`UPDATE_CONFIGS` / settings channel is **Phase 3** — not wired here.)
- The "became hidden" behavior is the same native code path Phase 1 exercises
  on desktop `FOCUS_LOST`/`HIDDEN`; only the trigger source differs.

### §E — Bundled save + C3 data

- Ship `wallpaper.svx` (an Augustus-format save) as an **APK asset**. On service
  start, if it is absent from the savegame location, copy it there, then load
  via the Phase-1 path (`dir_get_file_at_location("wallpaper.svx",
  PATH_LOCATION_SAVEGAME)` → `game_file_load_saved_game`).
- C3 graphics/data continue to load through Augustus's existing SAF pipeline
  (`FileManager.java` + `src/platform/android/*`). The only adaptation is using
  the **application/service context** where the current code assumes an activity
  context; SAF persisted-URI grants are app-wide, so the existing
  `ContentResolver` access is reusable from the service.

### §F — SDL in-process re-init guard

Adopt h2lwp's guard: on wallpaper re-selection while an SDL thread is live, call
`nativeQuit()` then `Process.killProcess()` and let Android restart the service
(SDL cannot cleanly re-init in-process).

## Verification model

- **Per task:** the relevant Gradle build succeeds. The terminal gate for every
  task is `./gradlew :augustus:assembleDebug` (Java + native compile). There is
  no unit-test harness (same as Phase 1).
- **Deferred device QA checklist** (run when a device/emulator is attached):
  1. `adb install -r` the debug APK; grant the C3 data folder via the SAF
     picker.
  2. Select "Augustus" from the system live-wallpaper picker; confirm the city
     map renders full-screen, no chrome, silent.
  3. Confirm the simulation runs while visible (figures move, clock advances).
  4. Go to the home screen / lock + unlock; confirm the camera shows a new area
     on each reveal and logs a re-center.
  5. Confirm ticks + rendering pause while hidden (no battery churn).
  6. Re-select the wallpaper; confirm the `killProcess` guard restarts cleanly.

## Key file map (insertion points)

| Concern | File / anchor |
| --- | --- |
| local.properties / SDK | `android/local.properties` (new) |
| SDL2 source | `ext/SDL2/SDL2-2.0.x/`, `ext/SDL2/SDL2_mixer-2.0.x/` (provision) |
| Vendored SDL Java | `android/augustus/src/main/java/org/libsdl/app/*` (new, from SDL source) |
| WallpaperService host | rewritten `SDLActivity` (`extends WallpaperService`) + wallpaper `Engine` |
| Manifest | `android/augustus/src/main/AndroidManifest.xml` |
| Live-wallpaper meta | `res/android/xml/livewallpaper.xml` (new) |
| SAF setup activity | `android/augustus/src/main/java/.../DirectorySelectionActivity.java` (kept) |
| JNI event channel | `src/platform/android/*`, `pushWallpaperEvent`, `SDL_USEREVENT` handling in the native loop |
| Reused native entry | `game_init_wallpaper()`, `city_view_go_to_random_tile()`, `--wallpaper` branch (Phase 1) |
| Bundled save asset | APK `assets/` → copied to `PATH_LOCATION_SAVEGAME` on first start |
| Gradle | `android/augustus/build.gradle`, `android/SDL2/build.gradle`, `android/settings.gradle` |

## Risks

- **SAF asset access from service context (primary Android-specific risk).**
  Augustus does heavy runtime file I/O through Java `FileManager` using an
  activity context; the service has none. Mitigation: use the application
  context + app-wide persisted SAF grants; adapt context only, reuse code.
- **SDL surface binding from a service.** SDL2's Android backend assumes a
  single `SDLActivity` singleton; the wallpaper `Engine` must satisfy the
  surface/context expectations. Mitigation: follow h2lwp's proven
  `getNativeSurface()` + thread-start-from-`onSurfaceChanged` pattern.
- **SDL in-process re-init.** Mitigation: `Process.killProcess()` guard (§F).
- **Build-only validation.** The APK compiling does not prove it runs; runtime
  correctness is explicitly deferred to the device QA checklist.

## Out of scope

- Playable game launcher on Android (deferred; combined game+wallpaper APK).
- Settings UI, `CONFIG_*` wallpaper keys, `UPDATE_CONFIGS` channel, and the
  in-app "set wallpaper" launcher button (all Phase 3).
- SDL3 Android build.
- Audio, interactivity/touch, home-screen parallax, multiple/selectable saves.

## Open questions

_None at the design layer. Remaining detail (exact SDL2 point release, task
breakdown, per-task build commands) is implementation-plan granularity and is
resolved in the plan._
