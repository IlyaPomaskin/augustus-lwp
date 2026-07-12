# Augustus Live Wallpaper — Phase 3 (Settings) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a settings screen (Scale, Brightness, Map change interval, Simulation speed) + a file-based config channel + an in-app "Set wallpaper" button to the Android live wallpaper.

**Architecture:** New Augustus `CONFIG_*` keys hold the four settings. The Java settings screen (extended `AssetSelectionActivity`) read-modify-writes `augustus.ini`; when the wallpaper next becomes visible, the already-wired `UPDATE_CONFIGS` event (currently a no-op at `augustus.c`) calls `config_load()` and applies each setting. h2lwp is the naming/UX reference. No new activities; plain Java + Views.

**Tech Stack:** C (C11) + SDL2 config/draw (native); Java + Gradle + Android NDK (Android). Reuses Phase 1/2 wallpaper plumbing.

**Source spec:** `docs/superpowers/specs/2026-07-12-augustus-live-wallpaper-phase3-settings-design.md`.

**Verification model (decided):** No unit-test harness. Each task ends with a **successful build** — Android `./gradlew :augustus:assembleDebug`, plus a desktop `cmake --build build` regression for any shared-C change. On-device behavior is a **deferred manual QA pass (Task 4)**. (Same model as Phases 1–2.)

## Global Constraints

- **Match surrounding style** in each file (Phase-1 C idioms; Java matches `AssetSelectionActivity`). No unrelated refactoring.
- **Config keys are ordinary Augustus config** (`config_get`/`config_set`, persisted in `augustus.ini`), defaulted so non-wallpaper builds are unaffected.
- **Settings apply on visibility**, not instantly while editing (via `UPDATE_CONFIGS`). This is intended.
- **Single writer:** only the settings activity writes the wallpaper keys; the wallpaper service is config-**read-only** (must not `config_save()` in wallpaper mode).
- **Config file path (Android):** `augustus.ini` resolves to `getFilesDir()/c3/augustus.ini` — the same c3 dir the Phase-2 `AssetSelectionActivity.c3Dir()` returns and the native base path reads (empty user-dir collapses `PATH_LOCATION_CONFIG`, exactly as `wallpaper.svx` collapsed in Phase 2).
- **h2lwp naming:** "Scale", "Brightness", "Map change interval" (options: Every switch to home screen / 10 minutes / 30 minutes / 2 hours / 24 hours), "Set wallpaper".

### Build env (every Android build task)

```
export JAVA_HOME="/Applications/Android Studio.app/Contents/jbr/Contents/Home"
export PATH="/Users/ilyapomaskin/work/augustus/.superpowers/sdd/tools/nasm-2.16.03:$PATH"
cd android && ./gradlew :augustus:assembleDebug
```
Desktop regression: `cmake --build build -j4`.

---

### Task 1: Add the four wallpaper config keys

**Files:**
- Modify: `src/core/config.h` (enum, before `CONFIG_MAX_ENTRIES`)
- Modify: `src/core/config.c` (`ini_keys[]`, `default_values[]`)

**Interfaces:**
- Produces: `CONFIG_UI_WALLPAPER_SCALE`, `CONFIG_UI_WALLPAPER_BRIGHTNESS`, `CONFIG_UI_WALLPAPER_MAP_CHANGE_MINUTES`, `CONFIG_UI_WALLPAPER_SPEED` — int config keys consumed by Task 2 and written by Task 3.

- [ ] **Step 1: Add enum members**

In `src/core/config.h`, immediately before `CONFIG_MAX_ENTRIES`:

```c
    CONFIG_UI_WALLPAPER_SCALE,
    CONFIG_UI_WALLPAPER_BRIGHTNESS,
    CONFIG_UI_WALLPAPER_MAP_CHANGE_MINUTES,
    CONFIG_UI_WALLPAPER_SPEED,
    CONFIG_MAX_ENTRIES
```

- [ ] **Step 2: Add ini key strings**

In `src/core/config.c`, in `ini_keys[]` (match the designated-initializer style), add:

```c
    [CONFIG_UI_WALLPAPER_SCALE] = "ui_wallpaper_scale",
    [CONFIG_UI_WALLPAPER_BRIGHTNESS] = "ui_wallpaper_brightness",
    [CONFIG_UI_WALLPAPER_MAP_CHANGE_MINUTES] = "ui_wallpaper_map_change_minutes",
    [CONFIG_UI_WALLPAPER_SPEED] = "ui_wallpaper_speed",
```

- [ ] **Step 3: Add defaults**

In `src/core/config.c`, in `default_values[]` (match its existing designated-initializer style), add. Defaults are chosen as "no override / no effect":

```c
    [CONFIG_UI_WALLPAPER_SCALE] = 0,              // 0 = keep the save's scale
    [CONFIG_UI_WALLPAPER_BRIGHTNESS] = 100,       // 100 = no dimming
    [CONFIG_UI_WALLPAPER_MAP_CHANGE_MINUTES] = 0, // 0 = every switch to home screen
    [CONFIG_UI_WALLPAPER_SPEED] = 0,              // 0 = don't override game speed
```

If `default_values[]` is a plain (non-designated) list, append the four values in the same order as the enum instead. Match whatever style the file uses.

- [ ] **Step 4: Build (desktop + android)**

`cmake --build build -j4` and `./gradlew :augustus:assembleDebug` (env above). Expect both clean. (`config_load`/`config_save` iterate `CONFIG_MAX_ENTRIES`, so the new keys are read/written automatically.)

- [ ] **Step 5: Commit**

```bash
git add src/core/config.h src/core/config.c
git commit -m "feat(wallpaper): add Phase 3 wallpaper settings config keys"
```

---

### Task 2: Apply the settings natively (UPDATE_CONFIGS + recenter gate + brightness)

Fill the no-op `UPDATE_CONFIGS` branch to re-read config and apply Scale + Speed; gate the recenter on the Map-change interval; dim the map per Brightness in the wallpaper draw.

**Files:**
- Modify: `src/platform/SDL2/augustus.c` (the `SDL_USEREVENT` block: `UPDATE_CONFIGS` apply + `HIDE` recenter gate)
- Modify: `src/window/city.c` (`draw_foreground_wallpaper` — brightness overlay)

**Interfaces:**
- Consumes: the Task-1 config keys; Phase-1 `city_view_go_to_random_tile()`, `city_view_set_scale()`, `game_wallpaper_mode()`.

- [ ] **Step 1: Apply Scale + Speed on UPDATE_CONFIGS**

In `src/platform/SDL2/augustus.c`, replace the `WALLPAPER_EVENT_UPDATE_CONFIGS` no-op branch:

```c
            } else if (event->user.code == WALLPAPER_EVENT_UPDATE_CONFIGS) {
                if (game_wallpaper_mode()) {
                    config_load(); // re-read augustus.ini edited by the settings screen
                    int scale = config_get(CONFIG_UI_WALLPAPER_SCALE);
                    if (scale > 0) {
                        city_view_set_scale(scale);
                    }
                    int speed = config_get(CONFIG_UI_WALLPAPER_SPEED);
                    if (speed > 0) {
                        setting_set_game_speed(speed);
                    }
                }
            }
```

Add includes if missing at the top of `augustus.c`: `#include "core/config.h"`, `#include "game/settings.h"`, `#include "city/view.h"` (view.h/game.h already included from Phase 1/2 — verify; add only what's missing).

- [ ] **Step 2: Gate the recenter on the Map-change interval**

In the same file, replace the `WALLPAPER_EVENT_HIDE` branch's recenter with a time-gated version. Add a file-scope helper + static above `handle_event`/the event handler:

```c
static unsigned int last_recenter_millis;

static int wallpaper_should_recenter(void)
{
    int minutes = config_get(CONFIG_UI_WALLPAPER_MAP_CHANGE_MINUTES);
    if (minutes <= 0) {
        return 1; // every switch to home screen
    }
    unsigned int now = SDL_GetTicks();
    if (last_recenter_millis != 0 && now - last_recenter_millis < (unsigned int) minutes * 60u * 1000u) {
        return 0;
    }
    last_recenter_millis = now;
    return 1;
}
```

Then the HIDE branch becomes:

```c
            } else if (event->user.code == WALLPAPER_EVENT_HIDE) {
                if (game_wallpaper_mode()) {
                    if (wallpaper_should_recenter()) {
                        city_view_go_to_random_tile();
                    }
#ifdef __ANDROID__
                    SDL_AndroidSendMessage(0x8000 + 1 /* COMMAND_PAUSE_NOW */, 0);
#endif
                }
            }
```

(When `minutes > 0`, `last_recenter_millis` is set on the first allowed recenter, so subsequent hides within the interval are skipped. `SDL_GetTicks` is already used in this file's timing.)

- [ ] **Step 3: Brightness overlay in the wallpaper draw**

In `src/window/city.c`, in `draw_foreground_wallpaper()` (the Phase-1 map-only foreground that calls `window_city_draw()`), after the map is drawn, dim it per Brightness:

```c
static void draw_foreground_wallpaper(void)
{
    window_city_draw(); // == widget_city_draw(): the map only, no chrome
    int brightness = config_get(CONFIG_UI_WALLPAPER_BRIGHTNESS);
    if (brightness < 100) {
        // darken the whole screen; graphics_shade_rect darkness increases with the amount dimmed
        graphics_shade_rect(0, 0, screen_width(), screen_height(), (100 - brightness) * SHADE_MAX / 100);
    }
}
```

Check `graphics_shade_rect`'s darkness scale in `src/graphics/graphics.c` and set `SHADE_MAX` (or inline the max) to its documented maximum so brightness 0 = fully dark, 100 = untouched. If `graphics_shade_rect`'s range doesn't map cleanly to 0–100, use a translucent `graphics_fill_rect` with an alpha `color_t` instead — pick whichever primitive gives a clean 0–100 dim and note the choice. Add includes: `#include "core/config.h"`, `#include "graphics/graphics.h"`, `#include "graphics/screen.h"` (for `screen_width/height`) if not already present in `city.c`.

- [ ] **Step 4: Build (android + desktop regression)**

`./gradlew :augustus:assembleDebug` (env) and `cmake --build build -j4`. Both clean. (The `SDL_USEREVENT`/HIDE code is shared but guarded by `game_wallpaper_mode()`; the overlay only draws in the wallpaper window; desktop unaffected.)

- [ ] **Step 5: Commit**

```bash
git add src/platform/SDL2/augustus.c src/window/city.c
git commit -m "feat(wallpaper): apply scale/speed on UPDATE_CONFIGS, gate recenter by interval, dim by brightness"
```

---

### Task 3: Settings UI + config-file channel + Set wallpaper button (Java)

Extend `AssetSelectionActivity` into the app's home screen: the four controls, a read-modify-write of `augustus.ini`, and the launcher button. Plain Java + Views.

**Files:**
- Modify: `android/augustus/src/main/java/com/github/Keriew/augustus/AssetSelectionActivity.java`
- Modify: `android/augustus/src/main/res/layout/activity_asset_selection.xml`
- Modify: `android/augustus/src/main/res/values/strings.xml`

**Interfaces:**
- Consumes: the Task-1 config key names (as ini strings), `c3Dir()` (Phase 2).
- Produces: `augustus.ini` in `c3Dir()` with the four `ui_wallpaper_*` keys.

- [ ] **Step 1: Config-file read-modify-write helper**

In `AssetSelectionActivity.java`, add a helper that updates one key in `c3Dir()/augustus.ini`, preserving all other lines (create the file if absent):

```java
private static final String INI_NAME = "augustus.ini";

private void writeConfigKey(String key, int value) {
    File ini = new File(c3Dir(), INI_NAME);
    java.util.LinkedHashMap<String, String> kv = new java.util.LinkedHashMap<>();
    if (ini.exists()) {
        try (java.io.BufferedReader r = new java.io.BufferedReader(new java.io.FileReader(ini))) {
            String line;
            while ((line = r.readLine()) != null) {
                int eq = line.indexOf('=');
                if (eq > 0) {
                    kv.put(line.substring(0, eq), line.substring(eq + 1));
                }
            }
        } catch (IOException e) {
            Log.e(TAG, "read ini", e);
        }
    }
    kv.put(key, Integer.toString(value));
    try (java.io.BufferedWriter w = new java.io.BufferedWriter(new java.io.FileWriter(ini))) {
        for (java.util.Map.Entry<String, String> e : kv.entrySet()) {
            w.write(e.getKey() + "=" + e.getValue() + "\n");
        }
    } catch (IOException e) {
        Log.e(TAG, "write ini", e);
    }
}

private int readConfigKey(String key, int fallback) {
    File ini = new File(c3Dir(), INI_NAME);
    if (!ini.exists()) return fallback;
    try (java.io.BufferedReader r = new java.io.BufferedReader(new java.io.FileReader(ini))) {
        String line;
        while ((line = r.readLine()) != null) {
            int eq = line.indexOf('=');
            if (eq > 0 && line.substring(0, eq).equals(key)) {
                try { return Integer.parseInt(line.substring(eq + 1).trim()); }
                catch (NumberFormatException nfe) { return fallback; }
            }
        }
    } catch (IOException e) { Log.e(TAG, "read ini", e); }
    return fallback;
}
```

Key-name constants (match Task 1's ini strings exactly):

```java
private static final String K_SCALE = "ui_wallpaper_scale";
private static final String K_BRIGHTNESS = "ui_wallpaper_brightness";
private static final String K_MAP_CHANGE = "ui_wallpaper_map_change_minutes";
private static final String K_SPEED = "ui_wallpaper_speed";
```

- [ ] **Step 2: Layout — add the controls**

In `res/layout/activity_asset_selection.xml`, below the existing status/select-folder views, add (inside the existing root container): a `SeekBar` `@+id/scale_bar`, a `SeekBar` `@+id/brightness_bar`, a `Spinner` `@+id/map_change_spinner`, a `SeekBar` `@+id/speed_bar`, each with a `TextView` label, and a `Button` `@+id/set_wallpaper_button`. Use the existing layout's width/padding idioms. (Full XML block written by the implementer to match the current file's container type and style.)

- [ ] **Step 3: Strings (h2lwp names)**

In `res/values/strings.xml`, add:

```xml
<string name="scale_title">Scale</string>
<string name="brightness_title">Brightness</string>
<string name="map_change_title">Map change interval</string>
<string name="map_change_every_switch">Every switch to home screen</string>
<string name="map_change_10min">10 minutes</string>
<string name="map_change_30min">30 minutes</string>
<string name="map_change_2h">2 hours</string>
<string name="map_change_24h">24 hours</string>
<string name="speed_title">Simulation speed</string>
<string name="set_wallpaper_button">Set wallpaper</string>
```

- [ ] **Step 4: Wire the controls in onCreate**

In `AssetSelectionActivity.onCreate`, after the existing setup, initialize each control from `readConfigKey(...)` and write on change. Concretely:

```java
SeekBar scaleBar = findViewById(R.id.scale_bar);
scaleBar.setMax(200);                                  // 0..200 (%, 0 = save's scale)
scaleBar.setProgress(readConfigKey(K_SCALE, 0));
scaleBar.setOnSeekBarChangeListener(new SimpleSeek(v -> writeConfigKey(K_SCALE, v)));

SeekBar brightnessBar = findViewById(R.id.brightness_bar);
brightnessBar.setMax(100);
brightnessBar.setProgress(readConfigKey(K_BRIGHTNESS, 100));
brightnessBar.setOnSeekBarChangeListener(new SimpleSeek(v -> writeConfigKey(K_BRIGHTNESS, v)));

SeekBar speedBar = findViewById(R.id.speed_bar);
speedBar.setMax(200);
speedBar.setProgress(readConfigKey(K_SPEED, 0));
speedBar.setOnSeekBarChangeListener(new SimpleSeek(v -> writeConfigKey(K_SPEED, v)));

// Map change interval: spinner index -> minutes
final int[] INTERVAL_MINUTES = {0, 10, 30, 120, 1440};
Spinner mapChange = findViewById(R.id.map_change_spinner);
mapChange.setAdapter(ArrayAdapter.createFromResource(this, R.array.map_change_options,
        android.R.layout.simple_spinner_dropdown_item));
int cur = readConfigKey(K_MAP_CHANGE, 0);
for (int i = 0; i < INTERVAL_MINUTES.length; i++) if (INTERVAL_MINUTES[i] == cur) mapChange.setSelection(i);
mapChange.setOnItemSelectedListener(new AdapterView.OnItemSelectedListener() {
    public void onItemSelected(AdapterView<?> p, View v, int pos, long id) { writeConfigKey(K_MAP_CHANGE, INTERVAL_MINUTES[pos]); }
    public void onNothingSelected(AdapterView<?> p) { }
});
```

Add a small `SimpleSeek` implements `SeekBar.OnSeekBarChangeListener` inner class that calls an `IntConsumer` on `onStopTrackingTouch` (write on release, not every pixel). Add a `<string-array name="map_change_options">` to `strings.xml` with the five labels (Step 3). Add imports (`SeekBar`, `Spinner`, `ArrayAdapter`, `AdapterView`, `android.widget.*`).

- [ ] **Step 5: Set wallpaper button**

```java
findViewById(R.id.set_wallpaper_button).setOnClickListener(v -> {
    Intent intent = new Intent(WallpaperManager.ACTION_CHANGE_LIVE_WALLPAPER);
    intent.putExtra(WallpaperManager.EXTRA_LIVE_WALLPAPER_COMPONENT,
            new ComponentName(this, "org.libsdl.app.SDLActivity"));
    try {
        startActivity(intent);
    } catch (android.content.ActivityNotFoundException e) {
        // Fallback: open the generic live-wallpaper chooser
        startActivity(new Intent(WallpaperManager.ACTION_LIVE_WALLPAPER_CHOOSER));
    }
});
```

Add imports: `android.app.WallpaperManager`, `android.content.ComponentName`, `android.content.Intent` (already present).

- [ ] **Step 6: Build**

`./gradlew :augustus:assembleDebug` (env). Expect BUILD SUCCESSFUL. (No native change in this task; no desktop build needed.)

- [ ] **Step 7: Commit**

```bash
git add android/augustus/src/main/java/com/github/Keriew/augustus/AssetSelectionActivity.java android/augustus/src/main/res/layout/activity_asset_selection.xml android/augustus/src/main/res/values/strings.xml
git commit -m "feat(wallpaper-android): settings screen (scale/brightness/interval/speed) + Set wallpaper button"
```

---

### Task 4: Device QA (manual, deferred)

Requires the device + granted C3 data (from Phase 2). Rebuild + reinstall first (`./gradlew :augustus:assembleDebug`, `adb install -r ...`).

- [ ] **Check 1 — Set wallpaper button:** tapping **Set wallpaper** opens the live-wallpaper chooser for Augustus.
- [ ] **Check 2 — Scale:** raise the Scale slider, return to the wallpaper → the map is more zoomed.
- [ ] **Check 3 — Brightness:** lower Brightness, return → the map is dimmed; 100 = no dim.
- [ ] **Check 4 — Map change interval:** set "10 minutes" → hiding/revealing within 10 min does NOT move the camera; after 10 min (or "Every switch") it does. `adb logcat | grep -i recenter` confirms cadence.
- [ ] **Check 5 — Simulation speed:** raise speed, return → the city sim runs faster.
- [ ] **Check 6 — Persistence:** settings survive re-selecting the wallpaper (written to `augustus.ini`).

---

## Self-Review

**Spec coverage (spec §A–§D):**
- §A new config keys → Task 1. ✅
- §B config channel (Java read-modify-write `augustus.ini`; native `config_load()` on `UPDATE_CONFIGS`) → Task 3 (write) + Task 2 Step 1 (re-read). ✅
- §C apply: scale/speed (Task 2 Step 1), brightness overlay (Task 2 Step 3), map-change gate (Task 2 Step 2). ✅
- §D settings UI + Set wallpaper launcher → Task 3. ✅
- Build-only per task + deferred QA → each task's build step + Task 4. ✅

**Placeholder scan:** The layout XML block (Task 3 Step 2) and the exact `graphics_shade_rect` mapping (Task 2 Step 3) are the only "match the file / check the primitive" steps — both name the exact IDs/function and the decision rule, not a vague TODO. Everything else is concrete code.

**Type consistency:** ini key strings identical between Task 1 (`ini_keys[]`) and Task 3 (`K_*` constants): `ui_wallpaper_scale/brightness/map_change_minutes/speed`. `config_get(CONFIG_UI_WALLPAPER_*)` (Task 2) matches the Task-1 enum. `setting_set_game_speed(int)`, `city_view_set_scale(int)`, `city_view_go_to_random_tile()`, `graphics_shade_rect(...)`, `WallpaperManager.ACTION_CHANGE_LIVE_WALLPAPER` all verified against source.

**Risks to watch during execution:**
- **Config path agreement:** native reads `augustus.ini` via `PATH_LOCATION_CONFIG` (collapses to the c3 root on Android, per Phase 2); Java writes `c3Dir()/augustus.ini`. If they diverge, settings won't apply — Task 4 Check 6 verifies. If native resolves it elsewhere, adjust the Java path to match (single source: wherever `dir_get_file_at_location(INI_FILENAME, PATH_LOCATION_CONFIG)` points).
- **`graphics_shade_rect` scale:** confirm its darkness range so brightness maps 0–100 cleanly; fall back to alpha `graphics_fill_rect` if not.
- **Service must not `config_save()`** in wallpaper mode (would rewrite the ini from memory). Confirm the wallpaper path never calls it; if it does, guard on `!game_wallpaper_mode()`.
- **Build-only:** compiling ≠ working; Task 4 is the real gate.

## Confidence Survey

_Open at execution. Lower risk than Phase 2: no SDL/Java-lifecycle surgery — this is additive config + a Views screen + small native apply hooks, all behind the established wallpaper plumbing. The one integration seam (config-file path agreement between Java and native) is explicitly flagged and QA-verified._
