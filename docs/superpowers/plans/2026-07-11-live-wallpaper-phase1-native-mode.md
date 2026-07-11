# Augustus Live Wallpaper — Phase 1 (Native Mode) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Plan-confidence status:** Accepted at 80% by explicit user override (iteration 3) — pending final go for execution

**Goal:** Add a desktop-testable `--wallpaper` native mode to Augustus that boots straight into a loaded city, renders the map full-screen with no UI chrome, keeps the simulation running, and moves the camera to a new area whenever the window is hidden/unfocused.

**Architecture:** A new runtime flag (`--wallpaper`) branches `setup()` to a new `game_init_wallpaper()` that loads a fixed save and shows a new map-only window `WINDOW_CITY_WALLPAPER`. A single wallpaper-mode flag (`game_wallpaper_mode()`) is consulted by the viewport code (full-screen), the tick code (autosave disabled), and the event loop (camera re-center on hide). No new render loop — the existing `run_and_draw()`/`main_loop()` drives frames.

**Tech Stack:** C (C11), SDL2, CMake. Caesar III original data files + a save file required to run.

**Scope note:** This plan covers **Phase 1 only** (native mode, testable on desktop). Phase 2 (Android `WallpaperService` + SDL vendoring + JNI event channel + bundled-save-in-APK) and Phase 3 (settings UI + `config` keys) are independent subsystems and will each get their own plan after Phase 1 is validated. Source spec: `docs/superpowers/specs/2026-07-11-augustus-live-wallpaper-design.md`.

**Verification model (decided — Q1.1):** Augustus has **no unit-test harness** (no `test/` dir, no `enable_testing`/`add_test` in `CMakeLists.txt`). Behavioral verification is therefore **deferred to a single manual QA pass** (Task 6) run after all implementation tasks. Each implementation task still ends with a **build** (the commit must compile) and a **commit**, but the on-screen behavior checks are consolidated in Task 6. (Trade-off: this leaves the plan at 80% confidence under the implementation-plan rubric, which rewards a fail-first check before each task — see the Confidence Survey.)

## Global Constraints

- **Language/standard:** C, matching surrounding style in each file (early returns, no unnecessary comments). Copy the existing code idioms in the file you touch.
- **Single binary:** no new build target, no product flavor. Wallpaper mode is chosen at runtime via `--wallpaper`.
- **SDL2** only; work targets local `master` (branch `feature/live-wallpaper`). Upstream feature branches out of scope.
- **Reuse existing code:** load saves via `game_file_load_saved_game()`, resolve paths via the `dir_*_location` APIs, render via `widget_city_draw()`. Do **not** build a parallel asset/render path.
- **Render scale (Q1.6):** Phase 1 uses the loaded save's existing render scale. Forced zoom / DPI-based scaling is deferred to Phase 3 settings.
- **Save location (Q1.2):** wallpaper mode loads a fixed `wallpaper.svx` from `PATH_LOCATION_SAVEGAME`; the developer places it manually for desktop testing. Phase 2 copies it from APK assets.
- **Wallpaper window id** `WINDOW_CITY_WALLPAPER` must be added at the **end** of the `window_id` enum (outside the `WINDOW_CITY..WINDOW_RACE_BET` range so existing range-based city predicates are unaffected) and wired explicitly where needed.
- **One source of truth** for "wallpaper mode is active": `int game_wallpaper_mode(void)` in `src/game/game.c`. Do not duplicate the flag.

### Build & run (used by every task's build step and by Task 6)

```bash
# From repo root. Configure once, then rebuild each task.
cmake -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j4
```

Run (macOS bundle path; on Linux the binary is `./build/augustus`):

```bash
# <C3_DIR> = folder containing your Caesar III data files (c3.eng, etc.)
./build/augustus.app/Contents/MacOS/augustus --wallpaper "<C3_DIR>"
```

**Prerequisite save file:** wallpaper mode loads a save named `wallpaper.svx` from the save-game location. To create it once: launch Augustus normally (`./build/.../augustus "<C3_DIR>"`), start or load a city, save the game, then copy that `.svx` into the save-game folder as `wallpaper.svx`. The save-game folder is the Augustus user directory (shown in the log line `Saving to ...` / configurable in-game). Keep this file present for Task 6.

---

### Task 1: Add the `--wallpaper` CLI flag

Adds the flag to the args struct and parser, and branches `setup()` so `--wallpaper` is recognized. In this task the branch calls a temporary stub that logs and falls through to normal init; the real bootstrap replaces it in Task 2.

**Files:**
- Modify: `src/platform/arguments.h:4-13` (struct)
- Modify: `src/platform/arguments.c:70-78` (defaults), `:126-139` (parse chain)
- Modify: `src/platform/SDL2/augustus.c:686` (branch)

**Interfaces:**
- Produces: `augustus_args.wallpaper` (int, 0/1) — consumed by `setup()` in later tasks.

- [ ] **Step 1: Add the struct field**

In `src/platform/arguments.h`, add `wallpaper` to the struct:

```c
typedef struct {
    const char *data_directory;
    int display_scale_percentage;
    int cursor_scale_percentage;
    int force_windowed;
    int launch_asset_previewer;
    int enable_joysticks;
    int use_software_cursor;
    int force_fullscreen;
    int display_id;
    int wallpaper;
} augustus_args;
```

- [ ] **Step 2: Default the field**

In `src/platform/arguments.c`, in the "Set sensible defaults" block (after `output_args->display_id = 0;`), add:

```c
    output_args->display_id = 0;
    output_args->wallpaper = 0;
```

- [ ] **Step 3: Parse the flag**

In `src/platform/arguments.c`, add a branch to the option chain, next to the `--asset-previewer` branch:

```c
        } else if (strcmp(argv[i], "--asset-previewer") == 0) {
            output_args->launch_asset_previewer = 1;
        } else if (strcmp(argv[i], "--wallpaper") == 0) {
            output_args->wallpaper = 1;
        } else if (strcmp(argv[i], "--enable-joysticks") == 0) {
```

- [ ] **Step 4: Branch in setup() with a temporary log stub**

In `src/platform/SDL2/augustus.c`, replace the single init line at `:686`:

```c
    int result;
    if (args->wallpaper) {
        SDL_Log("Wallpaper mode requested (stub: falling through to normal init)");
        result = game_init();
    } else if (args->launch_asset_previewer) {
        result = window_asset_previewer_show();
    } else {
        result = game_init();
    }
```

- [ ] **Step 5: Build**

Run: `cmake --build build -j4`
Expected: builds with no errors.

- [ ] **Step 6: Commit**

```bash
git add src/platform/arguments.h src/platform/arguments.c src/platform/SDL2/augustus.c
git commit -m "feat(wallpaper): add --wallpaper CLI flag and setup branch"
```

---

### Task 2: Wallpaper bootstrap — load save, skip menu, disable autosave

Adds the wallpaper-mode flag and a `game_init_wallpaper()` that loads `wallpaper.svx` and shows the **standard** city window (map-only window comes in Task 3). Guards autosave so the running city never overwrites the save.

**Files:**
- Modify: `src/game/game.h` (declare `game_wallpaper_mode`, `game_set_wallpaper_mode`, `game_init_wallpaper`)
- Modify: `src/game/game.c:80-165` (add flag + accessor + `game_init_wallpaper`)
- Modify: `src/game/tick.c:117-119` (guard monthly autosave), `:137-141` (guard yearly autosave)
- Modify: `src/platform/SDL2/augustus.c:686` (call real bootstrap)

**Interfaces:**
- Consumes: `augustus_args.wallpaper` (Task 1).
- Produces:
  - `int game_wallpaper_mode(void)` — returns 1 when wallpaper mode active.
  - `void game_set_wallpaper_mode(int enabled)`.
  - `int game_init_wallpaper(void)` — returns 1 on success, 0 on failure (like `game_init`).

- [ ] **Step 1: Declare the new functions in the header**

In `src/game/game.h`, add near the other `game_init` declarations:

```c
/**
 * Initializes the game for live-wallpaper mode: loads the wallpaper save and
 * shows the wallpaper city window. Returns 1 on success, 0 on failure.
 */
int game_init_wallpaper(void);

/**
 * @return 1 if the game is running in live-wallpaper mode
 */
int game_wallpaper_mode(void);

void game_set_wallpaper_mode(int enabled);
```

- [ ] **Step 2: Add includes and the flag accessor in game.c**

In `src/game/game.c`, add the includes needed by the bootstrap near the top with the other includes (`core/log.h` for `log_info` is already present via `errlog`):

```c
#include "city/view.h"
#include "core/dir.h"
#include "figure/formation.h"
#include "game/file.h"
#include "window/city.h"
```

Then add the flag + accessors above `game_pre_init` (near `:80`):

```c
static int wallpaper_mode;

int game_wallpaper_mode(void)
{
    return wallpaper_mode;
}

void game_set_wallpaper_mode(int enabled)
{
    wallpaper_mode = enabled;
}
```

- [ ] **Step 3: Implement game_init_wallpaper**

In `src/game/game.c`, add after `game_init()` (after `:165`). The asset-loading calls are duplicated from `game_init` deliberately (Q1.3) to isolate the wallpaper path from the shipping init:

```c
int game_init_wallpaper(void)
{
    game_set_wallpaper_mode(1);

    if (!image_load_climate(CLIMATE_CENTRAL, 0, 1, 0)) {
        errlog("unable to load main graphics");
        return 0;
    }
    if (!image_load_enemy(ENEMY_0_BARBARIAN)) {
        errlog("unable to load enemy graphics");
        return 0;
    }
    if (!image_load_fonts(encoding_get())) {
        if (encoding_get() != ENCODING_KOREAN && encoding_get() != ENCODING_JAPANESE) {
            errlog("unable to load font graphics");
            return 0;
        }
    }
    model_reset();
    building_properties_init();
    load_augustus_messages();
    sound_system_init();
    game_state_init();
    resource_init();

    const char *save_path = dir_get_file_at_location("wallpaper.svx", PATH_LOCATION_SAVEGAME);
    if (!save_path) {
        errlog("wallpaper mode: 'wallpaper.svx' not found in the save-game folder");
        return 0;
    }
    log_info("Wallpaper: loading save", save_path, 0);
    if (game_file_load_saved_game(save_path) != FILE_LOAD_SUCCESS) {
        errlog("wallpaper mode: failed to load 'wallpaper.svx'");
        return 0;
    }
    formation_set_selected(0); // a loaded save may have a legion selected; keep the map view clean
    window_city_show();
    return 1;
}
```

On load failure the function returns 0; `setup()` then logs "game init failed" and exits with status 2 (Q2.3 — no fallback save/city in Phase 1). The `city_view_go_to_random_tile()` call that randomizes the first frame (Q2.5) is added to this function in Task 5, once that helper exists.

- [ ] **Step 4: Guard monthly autosave**

In `src/game/tick.c`, wrap the monthly autosave call (`:117-119`):

```c
    if (!game_wallpaper_mode() && setting_monthly_autosave()) {
        game_file_write_saved_game(dir_append_location("autosave.svx", PATH_LOCATION_SAVEGAME));
    }
```

- [ ] **Step 5: Guard yearly autosave**

In `src/game/tick.c`, wrap the yearly autosave call (`:137-141`):

```c
    if (!game_wallpaper_mode() && config_get(CONFIG_GP_CH_YEARLY_AUTOSAVE) &&
        game_time_month() == 11 && game_time_day() == 15) {
        // 0-based index so 11 = December, 15 = last day of the month
        game_file_make_yearly_autosave();
    }
```

If `tick.c` does not already include `game/game.h`, add `#include "game/game.h"` with the other includes at the top.

- [ ] **Step 6: Call the real bootstrap from setup()**

In `src/platform/SDL2/augustus.c`, replace the Task-1 stub branch:

```c
    int result;
    if (args->wallpaper) {
        result = game_init_wallpaper();
    } else if (args->launch_asset_previewer) {
        result = window_asset_previewer_show();
    } else {
        result = game_init();
    }
```

Ensure `augustus.c` includes `game/game.h` (it already calls `game_init`, so it does).

- [ ] **Step 7: Build**

Run: `cmake --build build -j4`
Expected: builds with no errors.

- [ ] **Step 8: Commit**

```bash
git add src/game/game.h src/game/game.c src/game/tick.c src/platform/SDL2/augustus.c
git commit -m "feat(wallpaper): boot into loaded city, disable autosave in wallpaper mode"
```

---

### Task 3: Map-only wallpaper window (hide all UI chrome, keep sim ticking)

Adds `WINDOW_CITY_WALLPAPER` and a window whose callbacks draw **only** the map — no top menu, no sidebar, no minimap — with a no-op input handler. Wires the new id into the tick gate and the city-view predicate so the simulation keeps running. Switches the bootstrap to show it.

**Files:**
- Modify: `src/graphics/window.h:101` (add enum member at end)
- Modify: `src/window/city.h` (declare `window_city_wallpaper_show`)
- Modify: `src/window/city.c:79-85` (predicate), and add wallpaper window near `:920`
- Modify: `src/game/speed.c:56-64` (tick switch)
- Modify: `src/game/game.c` (`game_init_wallpaper` shows the new window)

**Interfaces:**
- Consumes: `game_wallpaper_mode()` (Task 2).
- Produces: `void window_city_wallpaper_show(void)`; enum `WINDOW_CITY_WALLPAPER`.

- [ ] **Step 1: Add the enum member (at the end, outside the city range)**

In `src/graphics/window.h`, add a trailing comma to the current last member and append the new one:

```c
    WINDOW_EDITOR_SELECT_CITY_RESOURCES_FOR_ROUTE,
    WINDOW_CITY_WALLPAPER
} window_id;
```

- [ ] **Step 2: Declare the show function**

In `src/window/city.h`, add near `window_city_show`:

```c
void window_city_wallpaper_show(void);
```

- [ ] **Step 3: Recognize the wallpaper window as a city view**

In `src/window/city.c`, update `window_city_is_window_cityview()` (`:79`). Including the id (Q1.4) makes map-rendering/camera helpers that gate on "is city view" work; the wallpaper window's no-op callbacks avoid city UI side effects:

```c
int window_city_is_window_cityview(void)
{
    int is_regular_cityview = ((window_get_id() >= WINDOW_CITY && window_get_id() <= WINDOW_RACE_BET)
        && window_get_id() != WINDOW_TOP_MENU);
    int is_wallpaper = window_get_id() == WINDOW_CITY_WALLPAPER;
    int is_cart_depo_window = 0; // placeholder
    return is_regular_cityview || is_wallpaper || is_cart_depo_window;
}
```

- [ ] **Step 4: Add the map-only window callbacks + show function**

In `src/window/city.c`, add just before `void window_city_show(void)` (near `:920`):

```c
static void draw_foreground_wallpaper(void)
{
    window_city_draw(); // == widget_city_draw(): the map only, no chrome
}

static void handle_input_wallpaper(const mouse *m, const hotkeys *h)
{
    // Non-interactive wallpaper: ignore all input.
}

void window_city_wallpaper_show(void)
{
    window_type window = {
        WINDOW_CITY_WALLPAPER,
        0, // no draw_background: draw_foreground repaints the full map each frame
        draw_foreground_wallpaper,
        handle_input_wallpaper,
        0  // no tooltip
    };
    window_show(&window);
}
```

- [ ] **Step 5: Tick the simulation for the wallpaper window**

In `src/game/speed.c`, add the new id to the city-window case in `game_speed_get_elapsed_ticks()` (`:59-64`):

```c
        case WINDOW_CITY:
        case WINDOW_CITY_MILITARY:
        case WINDOW_CITY_WALLPAPER:
        case WINDOW_SLIDING_SIDEBAR:
        case WINDOW_OVERLAY_MENU:
        case WINDOW_MILITARY_MENU:
        case WINDOW_BUILD_MENU:
```

- [ ] **Step 6: Show the wallpaper window from the bootstrap**

In `src/game/game.c`, in `game_init_wallpaper()`, replace `window_city_show();` with:

```c
    window_city_wallpaper_show();
```

- [ ] **Step 7: Build**

Run: `cmake --build build -j4`
Expected: builds with no errors. If `-Werror` flags an unhandled `WINDOW_CITY_WALLPAPER` in any `switch(window_get_id())`, add a `default:`/no-op there.

- [ ] **Step 8: Commit**

```bash
git add src/graphics/window.h src/window/city.h src/window/city.c src/game/speed.c src/game/game.c
git commit -m "feat(wallpaper): add map-only WINDOW_CITY_WALLPAPER, keep sim ticking"
```

---

### Task 4: Full-screen viewport (edge-to-edge map)

Makes the city viewport cover the whole screen in wallpaper mode by adding a wallpaper viewport variant selected whenever `game_wallpaper_mode()` is true.

**Files:**
- Modify: `src/city/view.c:594-620` (add `set_viewport_wallpaper`, branch in `city_view_set_scale` and `city_view_set_viewport`)

**Interfaces:**
- Consumes: `game_wallpaper_mode()` (Task 2).

- [ ] **Step 1: Include game.h in view.c**

In `src/city/view.c`, add with the other includes at the top:

```c
#include "game/game.h"
```

- [ ] **Step 2: Add the wallpaper viewport variant**

In `src/city/view.c`, add after `set_viewport_without_sidebar()` (`:597`):

```c
static void set_viewport_wallpaper(void)
{
    set_viewport(0, 0, data.screen_width, data.screen_height);
}
```

- [ ] **Step 3: Select it in city_view_set_scale**

In `src/city/view.c`, update the branch in `city_view_set_scale()` (`:603-609`):

```c
void city_view_set_scale(int scale)
{
    scale = calc_bound(scale, 50, city_view_get_max_scale());
    data.scale = scale;
    if (game_wallpaper_mode()) {
        set_viewport_wallpaper();
    } else if (data.sidebar_collapsed) {
        set_viewport_without_sidebar();
    } else {
        set_viewport_with_sidebar();
    }
    check_camera_boundaries();
    graphics_renderer()->update_scale(scale);
}
```

- [ ] **Step 4: Select it in city_view_set_viewport**

In `src/city/view.c`, update the branch in `city_view_set_viewport()` (`:616-624`):

```c
void city_view_set_viewport(int screen_width, int screen_height)
{
    data.screen_width = screen_width;
    data.screen_height = screen_height;
    if (game_wallpaper_mode()) {
        set_viewport_wallpaper();
    } else if (data.sidebar_collapsed) {
        set_viewport_without_sidebar();
    } else {
        set_viewport_with_sidebar();
    }
    check_camera_boundaries();
}
```

- [ ] **Step 5: Build**

Run: `cmake --build build -j4`
Expected: builds with no errors.

- [ ] **Step 6: Commit**

```bash
git add src/city/view.c
git commit -m "feat(wallpaper): render city map edge-to-edge (full-screen viewport)"
```

---

### Task 5: Re-center camera when the wallpaper is hidden/unfocused

Adds a helper that jumps the camera to a random valid tile, and calls it from the window event handler when the window is hidden or loses focus — but only in wallpaper mode. On desktop this is triggered by minimizing or alt-tabbing; on Android (Phase 2) the same helper will be driven by the `HIDE` wallpaper event. Both `FOCUS_LOST` and `HIDDEN` are wired (Q1.5).

**Files:**
- Modify: `src/city/view.h` (declare `city_view_go_to_random_tile`)
- Modify: `src/city/view.c` (add helper + includes)
- Modify: `src/platform/SDL2/augustus.c:210-238` (call on FOCUS_LOST / HIDDEN)

**Interfaces:**
- Consumes: `game_wallpaper_mode()` (Task 2), `map_grid_*`, `random_between_from_stdlib`, existing `city_view_go_to_grid_offset`.
- Produces: `void city_view_go_to_random_tile(void)`.

- [ ] **Step 1: Declare the helper**

In `src/city/view.h`, add near `city_view_go_to_grid_offset`:

```c
void city_view_go_to_random_tile(void);
```

- [ ] **Step 2: Add includes for the helper in view.c**

In `src/city/view.c`, add with the other includes (if not already present):

```c
#include "core/log.h"
#include "core/random.h"
#include "map/grid.h"
```

- [ ] **Step 3: Implement the helper**

In `src/city/view.c`, add after `city_view_go_to_grid_offset()` (`:538`). The 100-attempt bound with a no-move fallback is intentional (degenerate-map edge case): rather than loop forever, it leaves the camera where it is. The `log_info` line (Q2.4) records each move for QA and Phase-2 Android debugging:

```c
void city_view_go_to_random_tile(void)
{
    int width = map_grid_width();
    int height = map_grid_height();
    for (int attempt = 0; attempt < 100; attempt++) {
        int x = random_between_from_stdlib(0, width - 1);
        int y = random_between_from_stdlib(0, height - 1);
        int grid_offset = map_grid_offset(x, y);
        if (map_grid_is_valid_offset(grid_offset)) {
            log_info("Wallpaper: recenter to grid offset", 0, grid_offset);
            city_view_go_to_grid_offset(grid_offset);
            return;
        }
    }
}
```

- [ ] **Step 4: Randomize the first frame (Q2.5)**

In `src/game/game.c`, in `game_init_wallpaper()`, call the helper once at the very end so the first reveal is already a random location (not the fixed `game_state_init()` tile 76,152). Replace the final `return 1;` region:

```c
    formation_set_selected(0); // a loaded save may have a legion selected; keep the map view clean
    window_city_wallpaper_show();
    city_view_go_to_random_tile();
    return 1;
}
```

(The `city/view.h` include for this call was added to `game.c` in Task 2 Step 2. Note `window_city_wallpaper_show()` here is the Task 3 form of the line.)

- [ ] **Step 5: Call it on hide/unfocus**

In `src/platform/SDL2/augustus.c`, add includes if missing (near the top, with other `#include` lines):

```c
#include "city/view.h"
#include "game/game.h"
```

Then in `handle_window_event()`, extend the `FOCUS_LOST` and `HIDDEN` cases (`:210-238`):

```c
        case SDL_WINDOWEVENT_FOCUS_LOST:
            mouse_set_window_focus(0);
            if (game_wallpaper_mode()) {
                city_view_go_to_random_tile();
            }
            break;
```

```c
        case SDL_WINDOWEVENT_HIDDEN:
            SDL_Log("Window %u hidden", (unsigned int) event->windowID);
            *window_active = 0;
            if (game_wallpaper_mode()) {
                city_view_go_to_random_tile();
            }
            break;
```

- [ ] **Step 6: Build**

Run: `cmake --build build -j4`
Expected: builds with no errors.

- [ ] **Step 7: Commit**

```bash
git add src/city/view.h src/city/view.c src/game/game.c src/platform/SDL2/augustus.c
git commit -m "feat(wallpaper): re-center camera on hide/unfocus and randomize first frame"
```

---

### Task 6: Final QA pass (manual)

Behavioral verification for the whole feature (Q1.1 = deferred to a single QA pass). Run against a real Caesar III install with `wallpaper.svx` present (see Prerequisite save file). Rebuild first: `cmake --build build -j4`.

- [ ] **Check 1: Boots straight into the city**

Run: `./build/augustus.app/Contents/MacOS/augustus --wallpaper "<C3_DIR>"`
Expected: no `Option --wallpaper not recognized`; the logo and main menu are skipped; the loaded city appears immediately.

- [ ] **Check 2: No UI chrome**

Expected: no top menu bar, no right sidebar, no minimap — only the map is drawn. Clicking/tapping does nothing.

- [ ] **Check 3: Edge-to-edge map**

Expected: the map fills the entire window (no ~24px top gap, no right-hand sidebar gap). Resize the window — the map stays full-bleed.

- [ ] **Check 4: Living city, no autosave write**

Expected: figures walk, water/flags animate, and the in-game clock advances. After at least one in-game month passes, confirm `autosave.svx` is **not** created/updated in the save folder.

- [ ] **Check 5: Camera re-centers on hide/unfocus**

Expected: the first frame already shows a random area (not the fixed default tile). The camera then holds still while focused. Alt-tab away (or minimize) and back several times — each return shows a different part of the city, the log prints a `Wallpaper: recenter to grid offset` line per move, and the view never runs off the map edge.

- [ ] **Check 6: Regression — normal launch still works**

Run: `./build/augustus.app/Contents/MacOS/augustus "<C3_DIR>"` (no flag)
Expected: normal logo → main menu → gameplay, unchanged; autosave still works in a normal game.

---

## Self-Review

**1. Spec coverage (spec Architecture §1 + Phasing Phase 1):**
- `--wallpaper` flag + runtime mode selection → Task 1. ✅
- Autoload save (`wallpaper.svx` via `game_file_load_saved_game`) → Task 2. ✅
- Autosave disabled in wallpaper mode → Task 2. ✅
- Map-only `WINDOW_CITY_WALLPAPER`, chrome hidden, living city (tick gating) → Task 3. ✅
- Full-screen viewport → Task 4. ✅
- Camera re-center on hide (`FOCUS_LOST`/`HIDDEN`) → Task 5. ✅
- Behavioral verification → Task 6 (deferred QA, per Q1.1). ✅
- Frame-loop reuse (no new loop) → inherent (no code needed). ✅
- Deferred to later plans: Android hosting (Phase 2), settings `CONFIG_*` + UI (Phase 3), native pause-when-hidden via `COMMAND_PAUSE_NOW` (Phase 2). Documented in Scope note.

**2. Placeholder scan:** No "TBD"/"add error handling"/"similar to Task N". Every code step shows full code. Empty `handle_input_wallpaper` body is an intentional documented no-op.

**3. Type consistency:** `game_wallpaper_mode()`/`game_set_wallpaper_mode(int)`/`game_init_wallpaper(void)` consistent across Tasks 2–5. `window_city_wallpaper_show(void)` and `WINDOW_CITY_WALLPAPER` consistent across Tasks 2, 3, 5. `city_view_go_to_random_tile(void)` (Task 5), `set_viewport_wallpaper(void)` (Task 4). All match verified source symbols (`game_file_load_saved_game`→`FILE_LOAD_SUCCESS`, `dir_get_file_at_location`/`dir_append_location`, `PATH_LOCATION_SAVEGAME`, `map_grid_width/height`, `map_grid_offset`, `map_grid_is_valid_offset`, `random_between_from_stdlib`, `city_view_go_to_grid_offset`).

**Risks to watch during execution:**
- `widget_city_draw()` needs `city_view_init()` state — established by `initialize_saved_game()` inside `game_file_load_saved_game()`.
- `WINDOW_CITY_WALLPAPER` at enum end shifts no serialized values (window ids are runtime-only).

## Confidence Survey

Edit checkboxes in-place to answer. Mark exactly one option per question with `[x]`. The option labeled *(Recommended)* is the skill's best guess given current plan + repo context — override freely.

_No open questions. All iteration-1 and iteration-2 questions were answered and folded into the plan body (see Reconciliation Log). The plan sits at 80% under the implementation-plan rubric because verification is a deferred manual QA pass (Q1.1 / Q2.1) rather than per-task fail-first checks — a limitation the user has explicitly accepted. The only levers to exceed 80% (add fail-first steps, or a C test harness) were declined._

## Reconciliation Log

Append-only. Newest entry at the bottom.

### Iteration 1 — 2026-07-11
- **Confidence:** 80% (cap from Readiness — implementation-plan rubric: no failing-test step before impl; verification is build→run→observe because there is no test harness)
- **Resolved:** none (first pass)
- **Still uncertain:** verification/test structure (Dim 3, binding), per-task edge-case depth (Dim 2), plus a few task-level choices — save location, init code sharing, `is_window_cityview` inclusion, re-center triggers, scale
- **New questions:** Q1.1 … Q1.6

### Iteration 2 — 2026-07-11
- **Confidence:** 80% (cap from Readiness — Q1.1 chose deferred QA, which does not add the per-task fail-first step the rubric rewards)
- **Resolved:**
  - Q1.1 → defer verification to a single final QA pass → restructured Tasks 1–5 (build+commit only), added Task 6 (QA) + "Verification model" note
  - Q1.2 → fixed `wallpaper.svx` in `PATH_LOCATION_SAVEGAME`, manual placement → Global Constraints (confirmed, Task 2)
  - Q1.3 → keep duplicated loader calls in `game_init_wallpaper` → Task 2 Step 3 note
  - Q1.4 → include `WINDOW_CITY_WALLPAPER` in `window_city_is_window_cityview()` → Task 3 Step 3 note
  - Q1.5 → re-center on both `FOCUS_LOST` and `HIDDEN` → Task 5 (confirmed)
  - Q1.6 → no forced scale in Phase 1 → Global Constraints (render scale)
- **Still uncertain:** verification structure keeps the readiness cap (Q2.1 decides); edge cases around formation state, load failure, initial camera, and observability (Q2.2–Q2.5) strengthen Dim 2
- **New questions:** Q2.1 … Q2.5

### Iteration 3 — 2026-07-11
- **Confidence:** 80% — terminal/accepted. Cap from Readiness (implementation-plan rubric: no per-task fail-first step). This is a *known, accepted* limitation, not an unresolved gap: the codebase has no test harness and the user chose deferred manual QA (Q1.1/Q2.1) over the two available levers.
- **Resolved:**
  - Q2.1 → accept 80%, proceed by explicit user override → status line updated
  - Q2.2 → `formation_set_selected(0)` in `game_init_wallpaper()` → Task 2 Step 3
  - Q2.3 → return 0 → exit on bad save (no fallback) → Task 2 Step 3 note
  - Q2.4 → log loaded save path + each re-center tile (via `log_info`, the codebase idiom, not `SDL_Log`) → Task 2 Step 3, Task 5 Step 3
  - Q2.5 → `city_view_go_to_random_tile()` at end of `game_init_wallpaper()` → Task 5 Step 4
- **Deliberately not appending new questions:** every open item is resolved; the sole residual is the accepted verification tradeoff. Manufacturing filler questions to satisfy the "5–10 questions" step would violate the honesty mandate. Confidence stays an honest 80%.
- **New questions:** none
