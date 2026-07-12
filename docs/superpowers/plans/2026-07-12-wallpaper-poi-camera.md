# Wallpaper Point-of-Interest Camera Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Plan-confidence status:** Ready for execution (iteration 1, confidence 90%)

**Goal:** Replace the live wallpaper's random-tile recenter with a scored point-of-interest picker that lands the camera on interesting Caesar III locations (landmarks, dense neighborhoods, industry clusters), with an ADB `NEXT_POI` trigger for QA.

**Architecture:** A new self-contained C module `src/city/wallpaper_poi.c` scans buildings once per recenter, scores candidates, spatially dedups, randomly samples 20, and cycles the camera through them via the engine's existing instant `city_view_go_to_grid_offset()`. It is invoked at the four existing recenter call sites and by a new debug-only Android broadcast bridged through the existing `pushWallpaperEvent` path.

**Tech Stack:** C (engine, C11-style matching the codebase), CMake build, Android Java (plain AppCompat/WallpaperService, no Kotlin), Python QA harness.

## Global Constraints

Copied verbatim from `docs/superpowers/specs/2026-07-12-wallpaper-poi-camera-design.md`:

- **Camera motion = instant jump** via `city_view_go_to_grid_offset()`; no animated panning.
- **Always on, no user setting.** POI replaces the random-tile recenter at every recenter site; `city_view_go_to_random_tile()` is the internal fallback only.
- **Faithful pipeline:** scan → score → edge-drop → sort → spatial-dedup → random-sample → cycle. Rescan on load **and** on full-cycle wrap (no `building_count()` heuristic).
- **Tunable constants (use these exact values):** `POI_SAMPLE_COUNT=20`, `POI_POOL_MAX=50`, `POI_MAX_CANDIDATES=1024`, `POI_EDGE_MARGIN=20`, `POI_MIN_SPACING=10`, `POI_CELL_TILES=15`, `POI_POP_SCORE_CAP=5`, `POI_POP_PER_POINT=500`, `POI_INDUSTRY_MIN=4`, `POI_INDUSTRY_DENSE=8`, `POI_INDUSTRY_SCORE=3`, `POI_INDUSTRY_SCORE_DENSE=5`.
- **Framing:** landmarks and multi-tile buildings target the footprint-center tile (`b->size / 2` shift, fallback to `b->grid_offset`); density/industry cells target the top qualifying building's `grid_offset`.
- **`NEXT_POI` receiver is debug-only** (`BuildConfig.DEBUG`) and **bypasses** the `wallpaper_should_recenter()` interval gate.
- **No unit-test harness exists.** Per-task verification = the relevant build compiles clean (`cmake --build build` for C, `cd android && ./gradlew :augustus:assembleDebug` for Java/JNI). Runtime behavior is deferred device QA, scriptable via the `wallpaper-qa` skill.
- **Event-code sync:** `WALLPAPER_EVENT_NEXT_POI` must be `3` in Java and `WALLPAPER_EVENT_CODE_BASE + 3` in `android.h`.
- **Verification bar (accepted decision):** build-compile per task + the scripted `wallpaper-qa` device pass — no standalone unit test. This is the deliberate DoD for this feature, consistent with phases 1–3 (the repo has no unit-test harness).
- **Cross-thread safety:** `NEXT_POI` routes through `pushWallpaperEvent` → `SDL_PushEvent` → the SDL event loop, so the camera advance runs on the SDL thread (thread-safe; matches HIDE/UPDATE_CONFIGS). No direct native call from `onReceive`, no mutex.
- **No rollback toggle:** POI applies at all four recenter sites unconditionally; there is no runtime flag or `#ifdef` fallback (git history is the only recourse).

---

## File Structure

| File | Responsibility | Action |
| --- | --- | --- |
| `src/city/wallpaper_poi.h` | Public API: `wallpaper_poi_invalidate()`, `wallpaper_poi_next()` | Create |
| `src/city/wallpaper_poi.c` | Scan/score/dedup/sample/cycle + fallback | Create |
| `CMakeLists.txt` | Register the new source in the city file list | Modify (after `src/city/view.c`, line ~407) |
| `src/game/game.c` | First-frame seed → invalidate + next | Modify (line 216, add include) |
| `src/platform/SDL2/augustus.c` | 3 recenter sites → `wallpaper_poi_next()`; `NEXT_POI` handler | Modify (lines 214, 243, 410; USEREVENT switch; add include) |
| `src/platform/android/android.h` | `WALLPAPER_EVENT_NEXT_POI` code | Modify |
| `android/augustus/src/main/java/org/libsdl/app/SDLActivity.java` | Java event constant + debug-only broadcast receiver | Modify |
| `.claude/skills/wallpaper-qa/wallpaper_qa.py` | `next_poi` / `poi_count` / `assert_poi_changed` ops + `poi_cycle` scenario | Modify |
| `.claude/skills/wallpaper-qa/SKILL.md` | Document the POI ops + ADB command | Modify |

---

### Task 1: POI module (scan, score, dedup, sample, cycle)

Self-contained module that compiles standalone (no callers yet). Reviewer gate: the algorithm.

**Files:**
- Create: `src/city/wallpaper_poi.h`
- Create: `src/city/wallpaper_poi.c`
- Modify: `CMakeLists.txt` (city file list, after `${PROJECT_SOURCE_DIR}/src/city/view.c`)

**Interfaces:**
- Consumes (existing engine API): `building_get(unsigned int)`, `building_count(void)`, `struct building` fields `state/type/size/grid_offset/house_population`, `BUILDING_STATE_IN_USE`, `map_grid_offset_to_x/y(int)`, `map_grid_is_valid_offset(int)`, `map_grid_add_delta(int,int,int)`, `map_grid_width/height(void)`, `GRID_SIZE`, `city_view_go_to_grid_offset(int)`, `city_view_go_to_random_tile(void)`, `random_between_from_stdlib(int,int)`, `log_info(const char*,const char*,int)`.
- Produces: `void wallpaper_poi_invalidate(void);` and `void wallpaper_poi_next(void);`.

- [ ] **Step 1: Create the header**

Create `src/city/wallpaper_poi.h`:

```c
#ifndef CITY_WALLPAPER_POI_H
#define CITY_WALLPAPER_POI_H

/**
 * @file
 * Point-of-interest camera for the live wallpaper. Scans the city for
 * interesting locations (landmarks, dense neighborhoods, industry clusters),
 * scores and samples them, and cycles the camera through them on each recenter.
 */

// Force a rescan of the POI list on the next advance (call when a city loads).
void wallpaper_poi_invalidate(void);

// Advance to the next POI and center the camera on it (instant jump). Rescans on
// first use and each time the list is fully cycled; falls back to a random tile
// when the city has no interesting locations.
void wallpaper_poi_next(void);

#endif // CITY_WALLPAPER_POI_H
```

- [ ] **Step 2: Create the implementation**

Create `src/city/wallpaper_poi.c`:

```c
#include "city/wallpaper_poi.h"

#include "building/building.h"
#include "building/type.h"
#include "city/view.h"
#include "core/log.h"
#include "core/random.h"
#include "map/grid.h"

#include <stdlib.h>
#include <string.h>

#define POI_SAMPLE_COUNT 20
#define POI_POOL_MAX 50
#define POI_MAX_CANDIDATES 1024
#define POI_EDGE_MARGIN 20
#define POI_MIN_SPACING 10
#define POI_CELL_TILES 15
#define POI_POP_SCORE_CAP 5
#define POI_POP_PER_POINT 500
#define POI_INDUSTRY_MIN 4
#define POI_INDUSTRY_DENSE 8
#define POI_INDUSTRY_SCORE 3
#define POI_INDUSTRY_SCORE_DENSE 5

#define POI_GRID_MAX ((GRID_SIZE + POI_CELL_TILES - 1) / POI_CELL_TILES)

typedef struct {
    int grid_offset;
    int score;
} wallpaper_poi;

typedef struct {
    int population;
    int best_pop;
    int best_pop_offset;
    int industry_count;
    int industry_offset;
} poi_cell;

static wallpaper_poi poi_list[POI_SAMPLE_COUNT];
static int poi_count;
static int poi_index;

static wallpaper_poi candidates[POI_MAX_CANDIDATES];
static int candidate_count;
static poi_cell cells[POI_GRID_MAX][POI_GRID_MAX];

static int landmark_score(building_type type)
{
    switch (type) {
        case BUILDING_COLOSSEUM:
        case BUILDING_HIPPODROME:
        case BUILDING_ARENA:
            return 8;
        case BUILDING_GRAND_TEMPLE_CERES:
        case BUILDING_GRAND_TEMPLE_NEPTUNE:
        case BUILDING_GRAND_TEMPLE_MERCURY:
        case BUILDING_GRAND_TEMPLE_MARS:
        case BUILDING_GRAND_TEMPLE_VENUS:
        case BUILDING_PANTHEON:
        case BUILDING_ORACLE:
            return 7;
        case BUILDING_SENATE:
        case BUILDING_GOVERNORS_PALACE:
            return 6;
        case BUILDING_GOVERNORS_VILLA:
        case BUILDING_LARGE_MAUSOLEUM:
        case BUILDING_CARAVANSERAI:
        case BUILDING_LIGHTHOUSE:
        case BUILDING_CITY_MINT:
        case BUILDING_MILITARY_ACADEMY:
            return 5;
        case BUILDING_THEATER:
        case BUILDING_AMPHITHEATER:
        case BUILDING_GLADIATOR_SCHOOL:
        case BUILDING_LARGE_TEMPLE_CERES:
        case BUILDING_LARGE_TEMPLE_NEPTUNE:
        case BUILDING_LARGE_TEMPLE_MERCURY:
        case BUILDING_LARGE_TEMPLE_MARS:
        case BUILDING_LARGE_TEMPLE_VENUS:
        case BUILDING_GOVERNORS_HOUSE:
        case BUILDING_TRIUMPHAL_ARCH:
        case BUILDING_OBELISK:
            return 4;
        case BUILDING_FORUM:
        case BUILDING_FORT_LEGIONARIES:
        case BUILDING_FORT_JAVELIN:
        case BUILDING_FORT_MOUNTED:
        case BUILDING_FORT_AUXILIA_INFANTRY:
        case BUILDING_FORT_ARCHERS:
            return 3;
        default:
            return 0;
    }
}

static int is_industry(building_type type)
{
    // Contiguous enum range: farms (100-105), raw materials (106-109),
    // basic workshops (110-114).
    if (type >= BUILDING_WHEAT_FARM && type <= BUILDING_POTTERY_WORKSHOP) {
        return 1;
    }
    switch (type) {
        case BUILDING_WAREHOUSE:
        case BUILDING_GRANARY:
        case BUILDING_DOCK:
        case BUILDING_WHARF:
        case BUILDING_GOLD_MINE:
        case BUILDING_SAND_PIT:
        case BUILDING_STONE_QUARRY:
        case BUILDING_CONCRETE_MAKER:
        case BUILDING_BRICKWORKS:
            return 1;
        default:
            return 0;
    }
}

static int footprint_center(const building *b)
{
    int centered = map_grid_add_delta(b->grid_offset, b->size / 2, b->size / 2);
    if (map_grid_is_valid_offset(centered)) {
        return centered;
    }
    return b->grid_offset;
}

static void add_candidate(int grid_offset, int score)
{
    if (candidate_count >= POI_MAX_CANDIDATES || score <= 0) {
        return;
    }
    if (!map_grid_is_valid_offset(grid_offset)) {
        return;
    }
    candidates[candidate_count].grid_offset = grid_offset;
    candidates[candidate_count].score = score;
    candidate_count++;
}

static int chebyshev(int a_offset, int b_offset)
{
    int ax = map_grid_offset_to_x(a_offset);
    int ay = map_grid_offset_to_y(a_offset);
    int bx = map_grid_offset_to_x(b_offset);
    int by = map_grid_offset_to_y(b_offset);
    int dx = ax > bx ? ax - bx : bx - ax;
    int dy = ay > by ? ay - by : by - ay;
    return dx > dy ? dx : dy;
}

static int compare_score_desc(const void *a, const void *b)
{
    const wallpaper_poi *pa = a;
    const wallpaper_poi *pb = b;
    return pb->score - pa->score;
}

static void collect_candidates(void)
{
    candidate_count = 0;
    memset(cells, 0, sizeof(cells));

    int total = building_count();
    for (int id = 1; id < total; id++) {
        building *b = building_get(id);
        if (b->state != BUILDING_STATE_IN_USE) {
            continue;
        }
        add_candidate(footprint_center(b), landmark_score(b->type));

        int cx = map_grid_offset_to_x(b->grid_offset) / POI_CELL_TILES;
        int cy = map_grid_offset_to_y(b->grid_offset) / POI_CELL_TILES;
        if (cx < 0 || cx >= POI_GRID_MAX || cy < 0 || cy >= POI_GRID_MAX) {
            continue;
        }
        poi_cell *cell = &cells[cx][cy];
        if (b->house_population > 0) {
            cell->population += b->house_population;
            if (b->house_population > cell->best_pop) {
                cell->best_pop = b->house_population;
                cell->best_pop_offset = b->grid_offset;
            }
        } else if (is_industry(b->type)) {
            if (cell->industry_count == 0) {
                cell->industry_offset = b->grid_offset;
            }
            cell->industry_count++;
        }
    }

    for (int cx = 0; cx < POI_GRID_MAX; cx++) {
        for (int cy = 0; cy < POI_GRID_MAX; cy++) {
            poi_cell *cell = &cells[cx][cy];
            if (cell->best_pop_offset) {
                int score = cell->population / POI_POP_PER_POINT;
                if (score > POI_POP_SCORE_CAP) {
                    score = POI_POP_SCORE_CAP;
                }
                add_candidate(cell->best_pop_offset, score);
            }
            if (cell->industry_count >= POI_INDUSTRY_MIN) {
                int dense = cell->industry_count >= POI_INDUSTRY_DENSE;
                add_candidate(cell->industry_offset,
                    dense ? POI_INDUSTRY_SCORE_DENSE : POI_INDUSTRY_SCORE);
            }
        }
    }
}

static int drop_edge_candidates(void)
{
    int width = map_grid_width();
    int height = map_grid_height();
    int kept = 0;
    for (int i = 0; i < candidate_count; i++) {
        int x = map_grid_offset_to_x(candidates[i].grid_offset);
        int y = map_grid_offset_to_y(candidates[i].grid_offset);
        int near_edge = x < POI_EDGE_MARGIN || y < POI_EDGE_MARGIN
            || x >= width - POI_EDGE_MARGIN || y >= height - POI_EDGE_MARGIN;
        if (!near_edge) {
            candidates[kept++] = candidates[i];
        }
    }
    candidate_count = kept;
    return kept;
}

static int build_pool(wallpaper_poi *pool)
{
    int pool_count = 0;
    for (int i = 0; i < candidate_count && pool_count < POI_POOL_MAX; i++) {
        int too_close = 0;
        for (int j = 0; j < pool_count; j++) {
            if (chebyshev(candidates[i].grid_offset, pool[j].grid_offset) < POI_MIN_SPACING) {
                too_close = 1;
                break;
            }
        }
        if (!too_close) {
            pool[pool_count++] = candidates[i];
        }
    }
    return pool_count;
}

static void sample_pool(wallpaper_poi *pool, int pool_count)
{
    if (pool_count <= POI_SAMPLE_COUNT) {
        for (int i = 0; i < pool_count; i++) {
            poi_list[i] = pool[i];
        }
        poi_count = pool_count;
    } else {
        for (int i = 0; i < POI_SAMPLE_COUNT; i++) {
            int r = random_between_from_stdlib(i, pool_count - 1);
            wallpaper_poi tmp = pool[i];
            pool[i] = pool[r];
            pool[r] = tmp;
            poi_list[i] = pool[i];
        }
        poi_count = POI_SAMPLE_COUNT;
    }
    poi_index = 0;
}

static void scan(void)
{
    collect_candidates();
    drop_edge_candidates();
    qsort(candidates, candidate_count, sizeof(wallpaper_poi), compare_score_desc);
    wallpaper_poi pool[POI_POOL_MAX];
    int pool_count = build_pool(pool);
    sample_pool(pool, pool_count);
}

void wallpaper_poi_invalidate(void)
{
    poi_count = 0;
}

void wallpaper_poi_next(void)
{
    int fully_cycled = poi_index + 1 >= poi_count;
    if (poi_count == 0 || fully_cycled) {
        scan();
    } else {
        poi_index++;
    }
    if (poi_count == 0) {
        city_view_go_to_random_tile();
        return;
    }
    city_view_go_to_grid_offset(poi_list[poi_index].grid_offset);
    log_info("Wallpaper POI grid offset", 0, poi_list[poi_index].grid_offset);
}
```

**Before moving on (RNG bound check):** open `src/core/random.c` and confirm `random_between_from_stdlib(min, max)` returns a value in the **inclusive** range `[min, max]`. If it is exclusive of `max` (returns `[min, max-1]`), change the sample call in `sample_pool()` from `random_between_from_stdlib(i, pool_count - 1)` to `random_between_from_stdlib(i, pool_count)` so the last pool entry can still be selected.

- [ ] **Step 3: Register the source in CMake**

In `CMakeLists.txt`, immediately after the line `    ${PROJECT_SOURCE_DIR}/src/city/view.c` (around line 407), add:

```cmake
    ${PROJECT_SOURCE_DIR}/src/city/wallpaper_poi.c
```

- [ ] **Step 4: Build to verify the module compiles**

Run: `cmake --build build 2>&1 | tail -20`
Expected: build completes with no errors mentioning `wallpaper_poi`. (If the `build/` dir is stale, run `cmake -S . -B build` first.)

- [ ] **Step 5: Commit**

```bash
git add src/city/wallpaper_poi.h src/city/wallpaper_poi.c CMakeLists.txt
git commit -m "feat(wallpaper): POI scanner module (landmarks/population/industry)"
```

---

### Task 2: Wire the POI picker into the recenter call sites

Replace all four `city_view_go_to_random_tile()` recenter sites so the wallpaper uses POIs. Reviewer gate: correct wiring + seed-on-load.

**Files:**
- Modify: `src/game/game.c` (add include; line 216 seed)
- Modify: `src/platform/SDL2/augustus.c` (add include; lines 214, 243, 410)

**Interfaces:**
- Consumes: `wallpaper_poi_invalidate()`, `wallpaper_poi_next()` (Task 1).

- [ ] **Step 1: Add the include to game.c**

In `src/game/game.c`, add after the existing `#include "city/view.h"` (line 5):

```c
#include "city/wallpaper_poi.h"
```

- [ ] **Step 2: Replace the first-frame seed in game.c**

In `src/game/game.c`, replace line 216:

```c
    city_view_go_to_random_tile();
```

with:

```c
    wallpaper_poi_invalidate();
    wallpaper_poi_next();
```

- [ ] **Step 3: Add the include to augustus.c**

In `src/platform/SDL2/augustus.c`, add after the existing `#include "platform/android/android.h"` (line 18):

```c
#include "city/wallpaper_poi.h"
```

- [ ] **Step 4: Replace the three recenter sites in augustus.c**

There are exactly three occurrences of `city_view_go_to_random_tile();` in this file (FOCUS_LOST line 214, WINDOW_HIDDEN line 243, WALLPAPER_EVENT_HIDE line 410). Replace **each** with:

```c
                    wallpaper_poi_next();
```

(Preserve each site's existing indentation. The line-410 site stays inside its `if (wallpaper_should_recenter())` guard — that gate is unchanged.)

- [ ] **Step 5: Build to verify**

Run: `cmake --build build 2>&1 | tail -20`
Expected: build completes with no errors.

- [ ] **Step 6: (Optional) desktop smoke check**

If a desktop wallpaper run is available, launch with `--wallpaper` and confirm the log emits `Wallpaper POI grid offset` lines on load and on window hide/focus-loss (grep the app log for `Wallpaper POI`). Deferred to QA otherwise.

- [ ] **Step 7: Commit**

```bash
git add src/game/game.c src/platform/SDL2/augustus.c
git commit -m "feat(wallpaper): drive recenter via POI picker at all four sites"
```

---

### Task 3: Android `NEXT_POI` trigger + QA support

Bundles the native event, the Java broadcast receiver, and the wallpaper-qa additions into **one task** (merged per plan-confidence Q1.4 — none is device-testable without the others). `NEXT_POI` routes through the thread-safe `pushWallpaperEvent` → `SDL_PushEvent` bridge (Q1.2); the receiver registers in the service `onCreate`/`onDestroy` (Q1.3). Reviewer gate: end-to-end `NEXT_POI` advance + QA harness.

**Part 1 — native event**

**Files:**
- Modify: `src/platform/android/android.h`
- Modify: `src/platform/SDL2/augustus.c` (USEREVENT switch)

**Interfaces:**
- Consumes: `wallpaper_poi_next()` (Task 1), `game_wallpaper_mode()`.
- Produces: `#define WALLPAPER_EVENT_NEXT_POI (WALLPAPER_EVENT_CODE_BASE + 3)`.

- [ ] **Step 1: Add the event code**

In `src/platform/android/android.h`, after the line
`#define WALLPAPER_EVENT_RESIZE_DISPLAY (WALLPAPER_EVENT_CODE_BASE + 2)` add:

```c
#define WALLPAPER_EVENT_NEXT_POI (WALLPAPER_EVENT_CODE_BASE + 3)
```

- [ ] **Step 2: Handle the event in augustus.c**

In `src/platform/SDL2/augustus.c`, in the `case SDL_USEREVENT:` chain, immediately after the closing brace of the `WALLPAPER_EVENT_UPDATE_CONFIGS` branch, add:

```c
            } else if (event->user.code == WALLPAPER_EVENT_NEXT_POI) {
                // Debug/QA broadcast: advance immediately, deliberately NOT gated
                // by wallpaper_should_recenter().
                if (game_wallpaper_mode()) {
                    wallpaper_poi_next();
                }
```

- [ ] **Step 3: Build to verify**

Run: `cmake --build build 2>&1 | tail -20`
Expected: build completes with no errors.

- [ ] **Step 4: Commit**

```bash
git add src/platform/android/android.h src/platform/SDL2/augustus.c
git commit -m "feat(wallpaper-android): NEXT_POI native event -> wallpaper_poi_next"
```

---

**Part 2 — Android debug-only broadcast receiver**

Register a runtime receiver that turns an ADB broadcast into `pushWallpaperEvent(WALLPAPER_EVENT_NEXT_POI)`. Registered in the service `onCreate`/`onDestroy` (Q1.3).

**Files:**
- Modify: `android/augustus/src/main/java/org/libsdl/app/SDLActivity.java`

**Interfaces:**
- Consumes: native `pushWallpaperEvent(int)`, `WALLPAPER_EVENT_NEXT_POI` code (= 3).
- Produces: broadcast action string `com.github.Keriew.augustus.NEXT_POI`.

- [ ] **Step 1: Add imports**

In `SDLActivity.java`, add alongside the other `android.content.*` imports (near line 11):

```java
import android.content.BroadcastReceiver;
import android.content.IntentFilter;
import com.github.Keriew.augustus.BuildConfig;
```

- [ ] **Step 2: Add the Java event constant**

After the line `public static final int WALLPAPER_EVENT_RESIZE_DISPLAY = 2;` (line 217), add:

```java
    // Kept in sync with WALLPAPER_EVENT_NEXT_POI in src/platform/android/android.h
    // (native adds WALLPAPER_EVENT_CODE_BASE, so 3 -> 103).
    public static final int WALLPAPER_EVENT_NEXT_POI = 3;
    public static final String ACTION_NEXT_POI = "com.github.Keriew.augustus.NEXT_POI";
```

- [ ] **Step 3: Add receiver field + register/unregister helpers**

Add a field near the other instance fields of `SDLActivity` (e.g. just below the class-level declarations), and two helper methods:

```java
    private BroadcastReceiver mPoiReceiver;

    private void registerPoiReceiver() {
        if (!BuildConfig.DEBUG || mPoiReceiver != null) {
            return; // QA-only hook; never exposed in release builds
        }
        mPoiReceiver = new BroadcastReceiver() {
            @Override
            public void onReceive(Context context, Intent intent) {
                Log.v(TAG, "NEXT_POI broadcast received");
                pushWallpaperEvent(WALLPAPER_EVENT_NEXT_POI);
            }
        };
        IntentFilter filter = new IntentFilter(ACTION_NEXT_POI);
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            registerReceiver(mPoiReceiver, filter, Context.RECEIVER_EXPORTED);
        } else {
            registerReceiver(mPoiReceiver, filter);
        }
    }

    private void unregisterPoiReceiver() {
        if (mPoiReceiver != null) {
            unregisterReceiver(mPoiReceiver);
            mPoiReceiver = null;
        }
    }
```

- [ ] **Step 4: Wire the helpers into the service lifecycle**

In `onCreate()` (line 350), after `super.onCreate();`, add:

```java
        registerPoiReceiver();
```

In `onDestroy()` (line 515), before `super.onDestroy();`, add:

```java
        unregisterPoiReceiver();
```

- [ ] **Step 5: Build the debug APK**

Run: `cd android && ./gradlew :augustus:assembleDebug 2>&1 | tail -20`
Expected: `BUILD SUCCESSFUL`.

- [ ] **Step 6: Commit**

```bash
git add android/augustus/src/main/java/org/libsdl/app/SDLActivity.java
git commit -m "feat(wallpaper-android): debug-only NEXT_POI broadcast receiver"
```

---

**Part 3 — wallpaper-qa skill support for POI**

Add the `next_poi` op, an assertion, a scenario, and docs so QA can step POIs via ADB.

**Files:**
- Modify: `.claude/skills/wallpaper-qa/wallpaper_qa.py`
- Modify: `.claude/skills/wallpaper-qa/SKILL.md`

**Interfaces:**
- Consumes: `PKG` (= `com.github.Keriew.augustus.debug`), `self._adb`, `self._sh`, `self._log`; the `com.github.Keriew.augustus.NEXT_POI` action (Task 3 Part 2); the native log line `Wallpaper POI` (Task 1).

- [ ] **Step 1: Add the POI marker constant**

In `wallpaper_qa.py`, after the line `RECENTER_MARK = "recenter to grid offset"` (line 61), add:

```python
POI_MARK = "Wallpaper POI"
```

- [ ] **Step 2: Add the `next_poi` atomic op**

In the `Wallpaper` class, after the `unlock(self)` method (around line 195), add:

```python
    def next_poi(self, n=1):
        """Advance to the next point of interest via the debug ADB broadcast."""
        for _ in range(n):
            self._adb("shell", "am", "broadcast",
                      "-a", "com.github.Keriew.augustus.NEXT_POI", "-p", PKG)
            time.sleep(1)
        return self._log(f"next_poi x{n}")
```

- [ ] **Step 3: Add the assertion helpers**

After the `assert_recentered(self, at_least=1)` method (around line 237), add:

```python
    def poi_count(self):
        return self._sh("logcat -d").count(POI_MARK)

    def assert_poi_changed(self, at_least=1):
        n = self.poi_count()
        if n < at_least:
            raise AssertionError(f"expected >= {at_least} POI jumps, saw {n}")
        return self._log(f"assert_poi_changed OK ({n} >= {at_least})")
```

- [ ] **Step 4: Add the `poi_cycle` scenario**

Locate the `SCENARIOS` dict in `wallpaper_qa.py`. Add an entry (match the existing scenario-function style in that dict):

```python
    "poi_cycle": lambda: (Wallpaper()
        .kill().set_wallpaper().assert_set()
        .show().wait(2)
        .logcat_clear()
        .next_poi().screenshot("poi_1")
        .next_poi().screenshot("poi_2")
        .assert_poi_changed(2)),
```

- [ ] **Step 5: Verify the harness parses (syntax + op wiring)**

Run: `cd .claude/skills/wallpaper-qa && python3 -c "import wallpaper_qa; w=wallpaper_qa.Wallpaper; assert hasattr(w,'next_poi') and hasattr(w,'assert_poi_changed'); print('ok')"`
Expected: prints `ok` with no import/syntax error.

Run: `cd .claude/skills/wallpaper-qa && python3 wallpaper_qa.py list`
Expected: the scenario list includes `poi_cycle`.

- [ ] **Step 6: Document in SKILL.md**

In `.claude/skills/wallpaper-qa/SKILL.md`:

a) Extend the frontmatter `description` — append before the closing quote:
`, stepping the point-of-interest camera via next_poi (ADB NEXT_POI broadcast)`.

b) In the Atomic operations table, add a row after the `Assert` row:

```
| POI | `next_poi(n)`, `poi_count()`, `assert_poi_changed(n)` |
```

c) In the "Quick reference (CLI)" bash block, add:

```bash
# jump to the next point of interest (debug build only):
adb shell am broadcast -a com.github.Keriew.augustus.NEXT_POI -p com.github.Keriew.augustus.debug
python3 wallpaper_qa.py run poi_cycle   # step through POIs + screenshot
```

- [ ] **Step 7: Commit**

```bash
git add .claude/skills/wallpaper-qa/wallpaper_qa.py .claude/skills/wallpaper-qa/SKILL.md
git commit -m "skill(wallpaper-qa): next_poi op, assert_poi_changed, poi_cycle scenario"
```

---

## Deferred device QA (after Task 3)

Run on an emulator/device with the debug APK + C3 data provisioned:

1. `python3 wallpaper_qa.py run poi_cycle` — passes `assert_poi_changed(2)`; `poi_1.png` and `poi_2.png` differ and each shows an interesting location (landmark or dense district), not open terrain.
2. Step `next_poi` ~25 times; confirm it cycles ~20 distinct locations then reshuffles (variety returns), and never crashes.
3. Hide/show repeatedly; confirm the interval gate still throttles hide-driven advances while `next_poi` (bypassing the gate) always advances.
4. Load a sparse/near-empty map; confirm graceful random-tile fallback (`Wallpaper POI` absent, `recenter to grid offset` present), no crash.

## Self-Review (completed)

- **Spec coverage:** §A module → Task 1; §B scanners/framing → Task 1 (`landmark_score`, `is_industry`, cell pass, `footprint_center`); §C post-processing → Task 1 (`drop_edge_candidates`/`build_pool`/`sample_pool`); §D integration → Task 2 (all four sites) + CMake (Task 1); §E `NEXT_POI` → Task 3 (parts 1–2); §F QA → Task 3 (part 3); tunables → Task 1 constants verbatim. No gaps.
- **Placeholder scan:** no TBD/TODO; every code step shows complete code.
- **Type consistency:** `wallpaper_poi_invalidate`/`wallpaper_poi_next` names identical across Tasks 1–3; `WALLPAPER_EVENT_NEXT_POI` = 3 (Java) / `BASE+3` (C); action string `com.github.Keriew.augustus.NEXT_POI` identical in Task 3 parts 1–3; log marker `Wallpaper POI` matches `POI_MARK`.

## Confidence Survey

_All 6 iteration-1 questions resolved and folded into the plan body — see the Reconciliation Log below. No open questions._

## Reconciliation Log

Append-only. Newest entry at the bottom.

### Iteration 1 — 2026-07-12 (initial)
- **Confidence:** 80% (cap from Readiness)
- **Resolved:** none (first pass)
- **Still uncertain:** Readiness — the implementation-plan TDD cap (no failing-test step precedes any impl step) applies because the repo has no unit-test harness; Q1.1 decides whether that's an accepted project constraint or warrants a test. Minor Unknowns: `random_between_from_stdlib` inclusivity (Q1.5).
- **New questions:** Q1.1 (verification approach), Q1.2 (cross-thread safety), Q1.3 (receiver lifecycle), Q1.4 (task split), Q1.5 (random bound), Q1.6 (rollback)

### Iteration 1 — 2026-07-12 (resolved)
- **Confidence:** 90% (was 80%; Readiness TDD cap consciously waived — see below)
- **Resolved:**
  - Q1.1 → accept build-compile + `wallpaper-qa` device pass as the verification bar (no unit test) → Global Constraints (verification bar)
  - Q1.2 → keep the `pushWallpaperEvent` → `SDL_PushEvent` event-loop bridge (thread-safe) → Global Constraints (cross-thread safety), Task 3 intro
  - Q1.3 → register receiver in service `onCreate`/`onDestroy` (unchanged) → Task 3 Part 2
  - Q1.4 → **merge** the native event + Java receiver + wallpaper-qa into a single Task 3 (parts 1–3) → Task 3 restructure, Self-Review cross-refs
  - Q1.5 → verify `random_between_from_stdlib` inclusivity in `src/core/random.c` during Task 1 and adjust the sample bound if needed → Task 1 RNG-bound note
  - Q1.6 → **no rollback mechanism**; POI applies at all four sites unconditionally (git history only) → Global Constraints (no rollback toggle)
- **Readiness cap note:** the implementation-plan "failing-test step" cap is not satisfiable here — the repo has no unit-test harness and behavior is only observable on-device. Q1.1 explicitly accepts build-compile + scripted device QA as the DoD (the same bar that shipped phases 1–3), so the gap is a deliberate, documented project constraint rather than an oversight. Confidence raised to 90% on that basis.
- **Still uncertain:** none at the implementation-plan layer.
- **New questions:** none
