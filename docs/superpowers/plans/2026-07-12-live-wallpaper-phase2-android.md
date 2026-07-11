# Augustus Live Wallpaper — Phase 2 (Android Hosting) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Ship a wallpaper-only Android APK that boots the Phase-1 `--wallpaper` path inside an Android `WallpaperService`, so Augustus can be selected as a live wallpaper.

**Architecture:** Follow h2lwp (the reference fork, [github.com/IlyaPomaskin/h2lwp](https://github.com/IlyaPomaskin/h2lwp)): vendor SDL2's Android Java (`org/libsdl/app/*`) into the app module and rewrite `SDLActivity` to `extends WallpaperService` (keeping the class name so `Java_org_libsdl_app_SDLActivity_*` JNI symbols resolve). An inner `SDLEngine extends WallpaperService.Engine` drives the SDL surface/thread and posts wallpaper events; a native JNI export `pushWallpaperEvent(int)` turns them into `SDL_USEREVENT`s handled in Augustus's existing event loop, reusing the Phase-1 `city_view_go_to_random_tile()` helper. The playable game launcher is dropped for this build.

**Tech Stack:** C (C11) + SDL2 + CMake (native, existing); Java + Gradle + Android NDK (Android). No new native render loop — Phase-1 code is reused.

**Source spec:** `docs/superpowers/specs/2026-07-12-augustus-live-wallpaper-phase2-android-design.md`.

**Verification model (decided):** Augustus has **no unit-test harness**. Every task's verification is a **successful Gradle build** of the affected module (the terminal gate is `cd android && ./gradlew :augustus:assembleDebug`). On-device behavior is **deferred to a single manual QA pass (Task 7)** run when a device/emulator is attached. This mirrors Phase 1 (build per task; behavioral QA consolidated at the end). Build passing does **not** prove the wallpaper runs — that is Task 7.

## Global Constraints

- **Language/style:** match surrounding code in each file (Phase-1 C idioms: early return, no needless comments; Java matches the vendored SDL/h2lwp style). Do not restructure unrelated code.
- **Wallpaper-only APK:** the `MAIN`/`LAUNCHER` playable-game activity is removed for this build. (Amends the parent spec's "single APK has both.")
- **Single source of truth** for wallpaper mode remains `game_wallpaper_mode()` (Phase 1, `src/game/game.c`). Do not duplicate.
- **Reuse Phase 1:** the native side already has `--wallpaper` parsing, `game_init_wallpaper()`, `WINDOW_CITY_WALLPAPER`, full-screen viewport, silent audio, hidden cursor, and `city_view_go_to_random_tile()`. Phase 2 adds the Android host + one native event bridge that calls the existing helper. Do **not** add a new render loop or a parallel wallpaper draw path.
- **SDL2 only** (the Gradle scripts auto-select SDL2 unless SDL3 AARs are present). SDL3 is out of scope.
- **Java ↔ native version match:** the vendored `org/libsdl/app/*` Java MUST come from the exact SDL2 source version provisioned into `ext/SDL2` (Task 1). Never mix Java from one SDL2 version with native from another.
- **JNI symbol names:** the SDL host class must stay named `org.libsdl.app.SDLActivity` so existing/native `Java_org_libsdl_app_SDLActivity_*` bindings resolve. The native JNI export added in Task 5 must be exactly `Java_org_libsdl_app_SDLActivity_pushWallpaperEvent`.
- **Wallpaper event codes** (`HIDE=0`, `UPDATE_CONFIGS=1`, `RESIZE_DISPLAY=2`) MUST match between the Java constants (`WALLPAPER_EVENT_*` in `SDLActivity.java`) and the native enum (`live_wallpaper_event` in C). `UPDATE_CONFIGS` is accepted but is a **no-op in Phase 2** (settings are Phase 3).
- **h2lwp reference:** use [github.com/IlyaPomaskin/h2lwp](https://github.com/IlyaPomaskin/h2lwp) for *what* to change. Key files: `android/app/src/main/java/org/libsdl/app/SDLActivity.java`, `android/app/src/main/AndroidManifest.xml`, `android/app/src/main/res/xml/livewallpaper.xml`, `src/fheroes2/game/game_wallpaper.cpp` (native event plumbing). h2lwp is a *reference*, not a copy source — apply its changes onto Augustus's provisioned SDL2 Java and Augustus's own event loop.

### Build & run (used by every task)

```bash
# From repo root. Gradle needs a JDK; use Android Studio's bundled JBR.
export JAVA_HOME="/Applications/Android Studio.app/Contents/jbr/Contents/Home"
cd android
./gradlew :augustus:assembleDebug
# APK: android/augustus/build/outputs/apk/debug/augustus-debug.apk
```

Toolchain present on this machine (verified): SDK at `~/Library/Android/sdk`, NDK `29.0.14206865`, SDK CMake `4.1.2`, `adb`, JBR as above.

---

### Task 1: Toolchain provisioning + baseline build gate

Make the **unmodified** tree build for Android, so later green builds prove our code, not the environment. No Augustus source changes in this task.

**Files:**
- Create: `android/local.properties`
- Create (provision, git-ignored/untracked): `ext/SDL2/SDL2-2.32.x/`, `ext/SDL2/SDL2_mixer-2.8.x/`

**Interfaces:**
- Produces: a working `./gradlew :augustus:assembleDebug` for the current tree; SDL2 native + stock SDL2 Android Java available under `ext/SDL2/` for Task 2 to vendor.

- [ ] **Step 1: Point Gradle at the SDK**

Create `android/local.properties`:

```properties
sdk.dir=/Users/ilyapomaskin/Library/Android/sdk
```

- [ ] **Step 2: Provision SDL2 + SDL2_mixer source**

The `:SDL2` Gradle module builds SDL from `ext/SDL2` and errors if it is absent (`SDL2 source not found ... 'SDL2-2.0.*'`). Fetch stock releases matching desktop's SDL2 2.32.10:

```bash
cd ext/SDL2
curl -L -o sdl2.tar.gz https://github.com/libsdl-org/SDL/releases/download/release-2.32.10/SDL2-2.32.10.tar.gz
tar xzf sdl2.tar.gz && rm sdl2.tar.gz          # -> ext/SDL2/SDL2-2.32.10/
curl -L -o mixer.tar.gz https://github.com/libsdl-org/SDL_mixer/releases/download/release-2.8.1/SDL2_mixer-2.8.1.tar.gz
tar xzf mixer.tar.gz && rm mixer.tar.gz         # -> ext/SDL2/SDL2_mixer-2.8.1/
```

If a release URL 404s, pick the newest available `release-2.32.x` / `release-2.8.x` tag and record the exact version chosen in the task report (Task 2 vendors Java from this same folder).

- [ ] **Step 3: Baseline build**

Run:
```bash
export JAVA_HOME="/Applications/Android Studio.app/Contents/jbr/Contents/Home"
cd android && ./gradlew :augustus:assembleDebug
```
Expected: `BUILD SUCCESSFUL`; APK at `android/augustus/build/outputs/apk/debug/`. This builds the **current** (game-launcher) APK — that is expected; we convert it to wallpaper-only in later tasks.

If the build fails for a toolchain reason (missing SDK component, NDK, cmake), resolve it (e.g. `sdkmanager` / Android Studio SDK manager) and note what was needed. If it cannot be resolved, report **BLOCKED** with the exact error — do not proceed.

- [ ] **Step 4: Commit**

`ext/SDL2/SDL2-*` and `local.properties` are build inputs, not source. Commit only a short note recording the provisioned versions (do not commit the SDL sources or `local.properties` — confirm they are git-ignored; if not, add them to `.gitignore`).

```bash
git add .gitignore android/  # only if .gitignore changed; never the SDL source tree
git commit -m "build(wallpaper-android): record Phase 2 toolchain provisioning (SDL2 2.32.10, SDL2_mixer 2.8.1)" --allow-empty
```

---

### Task 2: Vendor SDL2's Android Java into the app module

Copy the **stock** `org/libsdl/app/*` from the SDL2 version provisioned in Task 1 into the Augustus app module, and make the app use the vendored Java while still linking the native `.so` from the `:SDL2` module. This isolates the SDL Java layer so Task 3 can rewrite it.

**Files:**
- Create: `android/augustus/src/main/java/org/libsdl/app/*.java` (copied from `ext/SDL2/SDL2-2.32.10/android-project/app/src/main/java/org/libsdl/app/`)
- Modify: `android/augustus/build.gradle` (ensure vendored Java is compiled; native still from `:SDL2`)

**Interfaces:**
- Consumes: provisioned SDL2 source (Task 1).
- Produces: `org.libsdl.app.SDLActivity` (+ `SDLSurface`, `SDLControllerManager`, `SDLAudioManager`, `SDL`, `HIDBusManager` if present) compiled from the app module.

- [ ] **Step 1: Copy the stock SDL Java**

```bash
SRC=ext/SDL2/SDL2-2.32.10/android-project/app/src/main/java/org/libsdl/app
DST=android/augustus/src/main/java/org/libsdl/app
mkdir -p "$DST" && cp "$SRC"/*.java "$DST"/
ls "$DST"
```

- [ ] **Step 2: Ensure the module compiles the vendored Java (no duplicate class)**

Confirm the `:SDL2` module contributes only the native library (`.so`) and its Java is not also on the app classpath (which would collide with the vendored copy). Inspect `android/SDL2/build.gradle` and `android/augustus/build.gradle`. If the SDL2 module publishes Java that duplicates the vendored classes, exclude it from the app's Java sources (keep its `prefab`/native output). Make the minimal change needed so exactly one copy of `org.libsdl.app.*` is compiled — the vendored one.

- [ ] **Step 3: Build**

```bash
export JAVA_HOME="/Applications/Android Studio.app/Contents/jbr/Contents/Home"
cd android && ./gradlew :augustus:assembleDebug
```
Expected: `BUILD SUCCESSFUL`, no "duplicate class org.libsdl.app.SDLActivity" error. Behavior is still the normal game.

- [ ] **Step 4: Commit**

```bash
git add android/augustus/src/main/java/org/libsdl/app android/augustus/build.gradle android/SDL2/build.gradle
git commit -m "feat(wallpaper-android): vendor SDL2 Android Java into the app module"
```

---

### Task 3: Rewrite the vendored SDLActivity into a WallpaperService

Apply h2lwp's modifications to the **vendored** `SDLActivity.java`: make it a `WallpaperService`, add the `SDLEngine`, the re-init guard, the wallpaper-event constants, the `pushWallpaperEvent` native declaration, and Augustus's `--wallpaper` argument + library names. Use h2lwp's `SDLActivity.java` as the reference for exactly which methods change.

**Files:**
- Modify: `android/augustus/src/main/java/org/libsdl/app/SDLActivity.java`
- Delete: `android/augustus/src/main/java/com/github/Keriew/augustus/AugustusMainActivity.java` (game launcher; its two native calls move — see below)

**Interfaces:**
- Consumes: vendored SDL Java (Task 2).
- Produces: `org.libsdl.app.SDLActivity extends WallpaperService`; `native void pushWallpaperEvent(int)`; `getArguments()` returning `{"--wallpaper"}`; `WALLPAPER_EVENT_HIDE/UPDATE_CONFIGS/RESIZE_DISPLAY` = `0/1/2`.

- [ ] **Step 1: Change the class declaration and imports**

In `SDLActivity.java`, replace `extends Activity` (and Activity-only imports) so the class is:

```java
import android.service.wallpaper.WallpaperService;
import android.view.SurfaceHolder;
import android.os.Process;

public class SDLActivity extends WallpaperService implements View.OnSystemUiVisibilityChangeListener {
```

Keep `protected static SDLActivity mSingleton;` and the existing static native/JNI plumbing. Follow h2lwp's `SDLActivity.java` (lines around `class SDLActivity`, `mSingleton`) for the exact set of Activity-only members to drop or guard.

- [ ] **Step 2: Add the wallpaper-event constants and native export**

Add near the other constants:

```java
public static final int WALLPAPER_EVENT_HIDE = 0;
public static final int WALLPAPER_EVENT_UPDATE_CONFIGS = 1;
public static final int WALLPAPER_EVENT_RESIZE_DISPLAY = 2;
```

Add with the other `public static native` declarations:

```java
public static native void pushWallpaperEvent(int code);
```

- [ ] **Step 3: Add the SDLEngine and onCreateEngine (with re-init guard)**

Add the inner engine class and factory, mirroring h2lwp (`SDLEngine extends Engine`, `onCreateEngine`). Concretely:

```java
private static SDLEngine mEngine;

class SDLEngine extends Engine {
    SDLEngine() { }

    @Override
    public void onVisibilityChanged(boolean isVisible) {
        if (isVisible) {
            pushWallpaperEvent(WALLPAPER_EVENT_UPDATE_CONFIGS);
        } else {
            pushWallpaperEvent(WALLPAPER_EVENT_HIDE);
        }
    }

    @Override
    public void onSurfaceCreated(SurfaceHolder holder) {
        super.onSurfaceCreated(holder);
        // SDL surface path: mSurface is bound via getNativeSurface() below
    }

    @Override
    public void onSurfaceChanged(SurfaceHolder holder, int format, int width, int height) {
        super.onSurfaceChanged(holder, format, width, height);
        pushWallpaperEvent(WALLPAPER_EVENT_RESIZE_DISPLAY);
        SDLActivity.nativeResume();
    }

    @Override
    public void onSurfaceDestroyed(SurfaceHolder holder) {
        SDLActivity.nativePause();
        super.onSurfaceDestroyed(holder);
    }
}

@Override
public Engine onCreateEngine() {
    // SDL cannot cleanly re-init in-process (h2lwp guard)
    if (mEngine != null && mEngine.mSDLThread != null) {
        SDLActivity.nativeQuit();
        Process.killProcess(Process.myPid());
    }
    mEngine = new SDLEngine();
    return mEngine;
}
```

Wire the SDL render surface to the engine by overriding `getNativeSurface()` to return the current engine's `getSurfaceHolder().getSurface()`, and start the SDL thread from `onSurfaceChanged` (h2lwp starts it there). Follow h2lwp's `SDLActivity.java` for the exact surface-binding and thread-start wiring against this SDL2 version's method names (`SDLActivity.mSurface`, `handleNativeState`, `onNativeSurfaceChanged`) — those names are version-specific, which is why the vendored Java and native SDL are pinned to the same version (Global Constraints).

- [ ] **Step 4: Set Augustus's arguments and libraries**

Fold the old `AugustusMainActivity` overrides into `SDLActivity` (since it is now the entry). Add:

```java
@Override
protected String[] getArguments() {
    return new String[]{ "--wallpaper" };
}

@Override
protected String[] getLibraries() {
    return new String[]{ "SDL2", "SDL2_mixer", "augustus" };
}
```

The two native methods `AugustusMainActivity` declared (`gotDirectory`, `releaseAssetManager`) are used by the SAF flow; move their declarations to `DirectorySelectionActivity` (Task 4) or keep them on a retained class. Their JNI symbols are `Java_com_github_Keriew_augustus_AugustusMainActivity_*` (`src/platform/android/android.c:196`, `asset_handler.c:135`) — renaming the class means renaming those exports; if you move them to `DirectorySelectionActivity`, update the C symbol names accordingly and note it for Task 5.

- [ ] **Step 5: Delete the game launcher activity**

```bash
git rm android/augustus/src/main/java/com/github/Keriew/augustus/AugustusMainActivity.java
```

- [ ] **Step 6: Build**

```bash
export JAVA_HOME="/Applications/Android Studio.app/Contents/jbr/Contents/Home"
cd android && ./gradlew :augustus:assembleDebug
```
Expected: `BUILD SUCCESSFUL`. (The manifest still references the old activity — that is fixed in Task 4; if the manifest merge fails on the missing class, proceed to Task 4 in the same review cycle and build once at the end of Task 4. Note this in the report.)

- [ ] **Step 7: Commit**

```bash
git add android/augustus/src/main/java
git commit -m "feat(wallpaper-android): rewrite SDLActivity into a WallpaperService (h2lwp-style)"
```

---

### Task 4: Manifest + live-wallpaper resource + SAF settings activity

Convert the manifest to a wallpaper-only app: remove the game launcher, declare the wallpaper `<service>`, and expose the SAF data-folder picker as the wallpaper's settings activity.

**Files:**
- Modify: `android/augustus/src/main/AndroidManifest.xml`
- Create: `res/android/xml/livewallpaper.xml`
- Modify: `android/augustus/src/main/java/com/github/Keriew/augustus/DirectorySelectionActivity.java` (only if native calls moved here in Task 3)

**Interfaces:**
- Consumes: `org.libsdl.app.SDLActivity` (Task 3), `DirectorySelectionActivity` (existing).
- Produces: a `<service>` selectable from Android's live-wallpaper picker; `@xml/livewallpaper` with `settingsActivity` → `DirectorySelectionActivity`.

- [ ] **Step 1: Rewrite the manifest**

Replace the `<application>` body so it declares the wallpaper service (no launcher activity):

```xml
    <uses-feature android:name="android.software.live_wallpaper" android:required="true" />
    <uses-feature android:glEsVersion="0x00020000" />
    <uses-feature android:name="android.hardware.touchscreen" android:required="false" />

    <application
        android:allowBackup="true"
        android:hardwareAccelerated="true"
        android:icon="@mipmap/augustus"
        android:label="@string/app_name"
        android:theme="@android:style/Theme.NoTitleBar.Fullscreen">

        <service
            android:name="org.libsdl.app.SDLActivity"
            android:exported="true"
            android:label="@string/app_name"
            android:icon="@mipmap/augustus"
            android:permission="android.permission.BIND_WALLPAPER">
            <intent-filter>
                <action android:name="android.service.wallpaper.WallpaperService" />
            </intent-filter>
            <meta-data
                android:name="android.service.wallpaper"
                android:resource="@xml/livewallpaper" />
        </service>

        <activity
            android:name=".DirectorySelectionActivity"
            android:exported="true"
            android:theme="@style/Theme.AppCompat.NoActionBar" />
    </application>
```

- [ ] **Step 2: Create the live-wallpaper resource**

Create `res/android/xml/livewallpaper.xml`:

```xml
<?xml version="1.0" encoding="UTF-8"?>
<wallpaper xmlns:android="http://schemas.android.com/apk/res/android"
    android:settingsActivity="com.github.Keriew.augustus.DirectorySelectionActivity"
    android:thumbnail="@mipmap/augustus" />
```

(`res/android` is already on `main.res.srcDirs` in `android/augustus/build.gradle`.)

- [ ] **Step 3: Reconcile moved native declarations (only if Task 3 moved them)**

If Task 3 moved `gotDirectory`/`releaseAssetManager` off the deleted `AugustusMainActivity`, declare them on `DirectorySelectionActivity` here and confirm Task 5 renames the C exports to match. If they were left on a retained class, no change.

- [ ] **Step 4: Build**

```bash
export JAVA_HOME="/Applications/Android Studio.app/Contents/jbr/Contents/Home"
cd android && ./gradlew :augustus:assembleDebug
```
Expected: `BUILD SUCCESSFUL`; manifest merges with no missing-class error.

- [ ] **Step 5: Commit**

```bash
git add android/augustus/src/main/AndroidManifest.xml res/android/xml/livewallpaper.xml android/augustus/src/main/java
git commit -m "feat(wallpaper-android): wallpaper-only manifest + live-wallpaper resource"
```

---

### Task 5: Native JNI event bridge (HIDE → recenter + pause)

Add the native `pushWallpaperEvent` JNI export and handle the resulting `SDL_USEREVENT` in Augustus's existing SDL event loop, reusing the Phase-1 `city_view_go_to_random_tile()` helper. Mirror h2lwp's `game_wallpaper.cpp` plumbing, adapted to Augustus's C event loop (no new loop).

**Files:**
- Modify: `src/platform/android/android.c` (or `jni.c`) — add the JNI export + event enum
- Modify: `src/platform/SDL2/augustus.c` — handle `SDL_USEREVENT` in the existing event handler
- Modify: `src/platform/android/android.c`/`asset_handler.c` — rename `AugustusMainActivity` JNI symbols only if Task 3 moved them

**Interfaces:**
- Consumes: Java `pushWallpaperEvent(int)` (Task 3), Phase-1 `city_view_go_to_random_tile()` and `game_wallpaper_mode()`.
- Produces: `Java_org_libsdl_app_SDLActivity_pushWallpaperEvent`; native `live_wallpaper_event` enum.

- [ ] **Step 1: Add the event enum + JNI export**

In `src/platform/android/android.c` (with the other JNI exports; include `<SDL.h>`):

```c
// Values must match WALLPAPER_EVENT_* in SDLActivity.java
typedef enum {
    LIVE_WALLPAPER_EVENT_HIDE = 0,
    LIVE_WALLPAPER_EVENT_UPDATE_CONFIGS = 1,
    LIVE_WALLPAPER_EVENT_RESIZE_DISPLAY = 2
} live_wallpaper_event;

JNIEXPORT void JNICALL Java_org_libsdl_app_SDLActivity_pushWallpaperEvent(JNIEnv *env, jclass cls, jint code)
{
    if (SDL_WasInit(SDL_INIT_EVENTS) == 0) {
        return;
    }
    SDL_Event event;
    SDL_zero(event);
    event.type = SDL_USEREVENT;
    event.user.code = (int) code;
    SDL_PushEvent(&event);
}
```

- [ ] **Step 2: Handle SDL_USEREVENT in the existing event loop**

In `src/platform/SDL2/augustus.c`, in the event-handling switch (the same function that handles `SDL_WINDOWEVENT` where Phase 1 wired `city_view_go_to_random_tile()` on `FOCUS_LOST`/`HIDDEN`), add:

```c
        case SDL_USEREVENT:
            if (game_wallpaper_mode()) {
                switch (event.user.code) {
                    case LIVE_WALLPAPER_EVENT_HIDE:
                        city_view_go_to_random_tile();
                        SDL_AndroidSendMessage(COMMAND_PAUSE_NOW, 0);
                        break;
                    case LIVE_WALLPAPER_EVENT_RESIZE_DISPLAY:
                        // handled by SDL's own resize path; no-op here
                        break;
                    case LIVE_WALLPAPER_EVENT_UPDATE_CONFIGS:
                        // Phase 3 (settings); no-op in Phase 2
                        break;
                    default:
                        break;
                }
            }
            break;
```

Define `COMMAND_PAUSE_NOW` (`0x8000 + 1`) and the `LIVE_WALLPAPER_EVENT_*` values visibly to this file (shared header or a local `#define`/enum matching Step 1). `SDL_AndroidSendMessage` is Android-only — guard the call with `#ifdef __ANDROID__` so the desktop build (which also compiles `augustus.c`) is unaffected; the `SDL_USEREVENT` case itself is harmless on desktop but only fires in wallpaper mode.

- [ ] **Step 3: Reconcile renamed SAF symbols (only if Task 3 moved them)**

If `gotDirectory`/`releaseAssetManager` moved to `DirectorySelectionActivity`, rename the C exports from `Java_com_github_Keriew_augustus_AugustusMainActivity_*` to `Java_com_github_Keriew_augustus_DirectorySelectionActivity_*` in `src/platform/android/android.c:196` and `asset_handler.c:135`. Otherwise leave them.

- [ ] **Step 4: Build (Android)**

```bash
export JAVA_HOME="/Applications/Android Studio.app/Contents/jbr/Contents/Home"
cd android && ./gradlew :augustus:assembleDebug
```
Expected: `BUILD SUCCESSFUL`; native compiles the new JNI export for all ABIs.

- [ ] **Step 5: Build (desktop regression)**

Confirm the shared `augustus.c` change didn't break desktop:
```bash
cmake --build build -j4
```
Expected: builds clean (the `#ifdef __ANDROID__` guard keeps `SDL_AndroidSendMessage` out of the desktop build).

- [ ] **Step 6: Commit**

```bash
git add src/platform/android src/platform/SDL2/augustus.c
git commit -m "feat(wallpaper-android): JNI wallpaper-event bridge (HIDE -> recenter + pause)"
```

---

### Task 6: Bundle the save into the APK and copy it on first run

Ship `wallpaper.svx` as an APK asset and copy it into the savegame location on wallpaper startup if absent, so `game_init_wallpaper()` (Phase 1) finds it. Ensure the C3-data SAF pipeline works from the service context.

**Files:**
- Create: `res/assets/wallpaper.svx` (or the packed-assets location the build uses) — an Augustus-format save
- Modify: `src/game/game.c` `game_init_wallpaper()` — copy bundled save to `PATH_LOCATION_SAVEGAME` before loading (Android only)
- Modify (if needed): `src/platform/android/asset_handler.c` — read the bundled save asset via the app/service context

**Interfaces:**
- Consumes: Phase-1 `game_init_wallpaper()` load path; `dir_get_file_at_location`, `PATH_LOCATION_SAVEGAME`.
- Produces: `wallpaper.svx` present in the savegame dir at load time on Android.

- [ ] **Step 1: Add the bundled save asset**

Place an Augustus-format `wallpaper.svx` where the Android build packs assets. `android/augustus/build.gradle` uses `res/packed_assets` if present, else `res/assets`. Add it under `res/assets/` (path: `res/assets/wallpaper.svx`) so it is bundled and reachable via Augustus's asset handler.

- [ ] **Step 2: Copy-on-start in the bootstrap (Android only)**

In `src/game/game.c`, in `game_init_wallpaper()`, before the existing `dir_get_file_at_location("wallpaper.svx", PATH_LOCATION_SAVEGAME)` lookup, add an Android-guarded copy that materializes the bundled asset into the savegame dir if it is not already there:

```c
#ifdef __ANDROID__
    if (!dir_get_file_at_location("wallpaper.svx", PATH_LOCATION_SAVEGAME)) {
        // Copy the APK-bundled save into the savegame folder on first run.
        android_copy_bundled_asset_to_savegame("wallpaper.svx");
    }
#endif
```

Implement `android_copy_bundled_asset_to_savegame(const char *name)` in `src/platform/android/asset_handler.c` using the existing `AAssetManager`/`ContentResolver` access (the same mechanism the C3 data pipeline uses), writing to the resolved `PATH_LOCATION_SAVEGAME` path. Match the file-copy idiom already in `asset_handler.c`. Declare it in `asset_handler.h`, guarded so it is only referenced on Android.

- [ ] **Step 3: Build (Android)**

```bash
export JAVA_HOME="/Applications/Android Studio.app/Contents/jbr/Contents/Home"
cd android && ./gradlew :augustus:assembleDebug
```
Expected: `BUILD SUCCESSFUL`; `wallpaper.svx` is present under the APK's `assets/` (verify: `unzip -l android/augustus/build/outputs/apk/debug/augustus-debug.apk | grep wallpaper.svx`).

- [ ] **Step 4: Build (desktop regression)**

```bash
cmake --build build -j4
```
Expected: clean (the copy is `#ifdef __ANDROID__`, so desktop `game_init_wallpaper()` is unchanged).

- [ ] **Step 5: Commit**

```bash
git add res/assets/wallpaper.svx src/game/game.c src/platform/android/asset_handler.c src/platform/android/asset_handler.h
git commit -m "feat(wallpaper-android): bundle wallpaper.svx in APK and copy to savegame on first run"
```

---

### Task 7: Final QA pass (manual, on device — deferred)

Behavioral verification for the whole Android wallpaper. **Requires a connected device/emulator with the C3 data files available**; not run during implementation. Rebuild first: `cd android && ./gradlew :augustus:assembleDebug`.

- [ ] **Check 1: Install + wallpaper appears**

`adb install -r android/augustus/build/outputs/apk/debug/augustus-debug.apk`. Open the system wallpaper picker → Live Wallpapers → "Augustus" appears with a thumbnail.

- [ ] **Check 2: Data grant**

From the wallpaper preview's Settings, the `DirectorySelectionActivity` opens; grant the folder holding the Caesar III data files.

- [ ] **Check 3: Renders the living city**

Set the wallpaper. The city map fills the screen, no chrome, silent, no cursor; figures move and the clock advances while visible.

- [ ] **Check 4: Re-center on hide**

Go to the app drawer / lock+unlock several times. Each reveal shows a different area; `adb logcat` shows a `Wallpaper: recenter to grid offset` line per reveal.

- [ ] **Check 5: Pause when hidden**

While hidden, ticks + rendering stop (no continuous CPU/GPU; confirm via `adb shell top`/battery). On reveal it resumes.

- [ ] **Check 6: Re-select guard**

Re-select the wallpaper; the `onCreateEngine` `killProcess` guard restarts the service cleanly (no black surface / crash).

---

## Self-Review

**Spec coverage (spec §A–§F + phasing):**
- §A toolchain + SDL2 provisioning + baseline gate → Task 1. ✅
- §B vendor SDL Java + WallpaperService rewrite → Tasks 2–3. ✅
- §C manifest (remove launcher, add service) + livewallpaper `settingsActivity` → Task 4. ✅
- §D JNI `pushWallpaperEvent` → `SDL_USEREVENT`, HIDE → recenter + `COMMAND_PAUSE_NOW`, RESIZE_DISPLAY → Task 5. ✅
- §E bundled save asset + copy-on-start + SAF-from-service → Task 6. ✅
- §F `Process.killProcess()` re-init guard → Task 3 (`onCreateEngine`). ✅
- Build-only per-task verification + deferred device QA → every task's build step + Task 7. ✅
- Reused Phase-1 pieces (`game_init_wallpaper`, `city_view_go_to_random_tile`, `--wallpaper`, `game_wallpaper_mode`) → consumed, not rebuilt. ✅

**Placeholder scan:** Java surface-binding/thread-start wiring in Task 3 Step 3 intentionally defers exact method names to the provisioned SDL2 version + h2lwp reference — flagged explicitly (version-specific), not a hidden TODO. All other steps show concrete code/commands.

**Type consistency:** `pushWallpaperEvent(int)` (Java, Task 3) ↔ `Java_org_libsdl_app_SDLActivity_pushWallpaperEvent(...jint)` (C, Task 5). `WALLPAPER_EVENT_HIDE/UPDATE_CONFIGS/RESIZE_DISPLAY = 0/1/2` (Java) ↔ `LIVE_WALLPAPER_EVENT_* = 0/1/2` (C). `getLibraries()` → `SDL2/SDL2_mixer/augustus`. `game_wallpaper_mode()`, `city_view_go_to_random_tile()` match Phase-1 symbols.

**Risks to watch during execution:**
- **Version-specific SDL Java internals** (Task 3): surface-binding/thread-start method names differ across SDL2 releases; the vendored Java and native SDL are pinned to the same version to keep them consistent. If the provisioned 2.32.10 Java diverges structurally from h2lwp's reference, follow the *intent* (bind engine surface, start thread on surface-changed) against 2.32.10's actual methods.
- **Build ≠ runtime:** all per-task verification is compilation. Whether the wallpaper actually renders/pauses is unknown until Task 7.
- **SAF from service context** (Task 6): the primary Android-specific runtime risk; only exercised at Task 7.
- **Manifest/class ordering** (Tasks 3–4): deleting `AugustusMainActivity` before the manifest stops referencing it can fail the manifest merge; Tasks 3 and 4 may need to land together — noted in Task 3 Step 6.

## Confidence Survey

_Open at execution. This plan carries higher irreducible uncertainty than Phase 1: the SDL2→WallpaperService Java rewrite depends on version-specific SDL internals resolved against the provisioned source + h2lwp reference, and verification is build-only (runtime deferred to Task 7). Confidence is bounded accordingly — the plan is structured so each build gate catches integration breakage early, and the risky Java wiring (Task 3) is isolated behind the Task-1/2 baseline gates._
