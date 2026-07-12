# Wallpaper "Always Positive" Guardian — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** In live-wallpaper mode only, prevent every negative outcome (defeat, invasions, fires/collapses, plague, unrest/rioters, bankruptcy, wolf attacks) so the displayed city always looks positive.

**Architecture:** A single new policy module (`city/wallpaper_guardian`) clamps the soft city metrics (sentiment, health, treasury) once per game-day when `game_wallpaper_mode()` is set. A handful of one-line `game_wallpaper_mode()` guards at existing call sites disable the "hard" subsystems (defeat check, fire/collapse, invasions, wolf aggression). Normal gameplay is untouched — every change is gated on the existing `game_wallpaper_mode()` predicate.

**Tech Stack:** C (C99), CMake (desktop build already configured at `build/`), Android NDK/CMake build via Gradle, `wallpaper-qa` Python harness for on-device behavior verification.

## Global Constraints

- Every behavior change MUST be gated on `game_wallpaper_mode()` (`src/game/game.c:87`, declared in `src/game/game.h:17`). Normal (non-wallpaper) gameplay must be byte-for-byte unchanged.
- No new user-facing setting or toggle. Always-positive is intrinsic to wallpaper mode.
- No magic numbers — use named constants.
- Prefer early-exit / guard clauses; extract compound conditions into named booleans.
- Includes are ordered alphabetically by path, grouped after the module's own header (follow the existing style in each file).
- **Testing reality:** this codebase has **no unit-test harness** (no `ctest`/`add_test`) for these native simulation paths. Each task is verified by a clean desktop compile (`cmake --build build`), which enforces strict warnings (`-Wmissing-prototypes -Wmissing-declarations -Wshadow -Wundef`, etc.). Runtime behavior is verified once at the end (Task 6) via an on-device `wallpaper-qa` soak. Do not fabricate unit tests.
- Desktop compile check (used as the per-task gate), run from repo root:
  `cmake --build build -j 2>&1 | tail -n 20` → expected: builds to completion, no errors, no new warnings referencing the edited files.

---

## File Structure

**Created:**
- `src/city/wallpaper_guardian.h` — declares `void wallpaper_guardian_update(void);`
- `src/city/wallpaper_guardian.c` — the daily metric-clamp policy. Depends only on `city/sentiment.h`, `city/health.h`, `city/finance.h`.

**Modified:**
- `CMakeLists.txt` — add `wallpaper_guardian.c` to `CITY_FILES`.
- `src/game/tick.c` — call the guardian daily (wallpaper-gated); gate `scenario_invasion_process()`.
- `src/city/victory.c` — early-return `city_victory_check()` in wallpaper mode (defeat/victory backstop).
- `src/building/maintenance.c` — early-return `building_maintenance_check_fire_collapse()` in wallpaper mode.
- `src/game/game.c` — clear pre-scheduled invasions once in `game_init_wallpaper()`.
- `src/figure/combat.c` — pacify aggressive animals (wolves) in wallpaper mode.
- `src/figure/formation_herd.c` — stop wolf packs from hunting in wallpaper mode.

Tasks 1–5 are independent of each other and may be implemented/reviewed in any order. Task 6 depends on all of them.

---

### Task 1: Guardian module (create, wire into build, hook into daily tick)

**Files:**
- Create: `src/city/wallpaper_guardian.h`
- Create: `src/city/wallpaper_guardian.c`
- Modify: `CMakeLists.txt` (CITY_FILES block, ends at the line `${PROJECT_SOURCE_DIR}/src/city/warning.c`)
- Modify: `src/game/tick.c` (add include; add call after the sentiment-update block at `:131-133`)

**Interfaces:**
- Produces: `void wallpaper_guardian_update(void);` — clamps sentiment→100 (+reset protesters/criminals), health→100, and tops a negative treasury up to a small floor. Idempotent; assumes it is only called when `game_wallpaper_mode()` is true (caller gates it).
- Consumes: `city_sentiment_set_happiness(int)`, `city_sentiment_reset_protesters_criminals(void)` (`city/sentiment.h:10,17`); `city_health_set(int)` (`city/health.h:20`); `city_finance_treasury(void)`, `city_finance_treasury_add(int)` (`city/finance.h:34,36`); `game_wallpaper_mode(void)` (`game/game.h:17`).

- [ ] **Step 1: Create the header**

`src/city/wallpaper_guardian.h`:
```c
#ifndef CITY_WALLPAPER_GUARDIAN_H
#define CITY_WALLPAPER_GUARDIAN_H

/**
 * Live-wallpaper "always positive" guardian. When the game runs in wallpaper
 * mode, clamps the soft city metrics (sentiment, health, treasury) once per
 * game-day so unrest, plague and bankruptcy can never develop. The caller is
 * responsible for invoking this only in wallpaper mode.
 */
void wallpaper_guardian_update(void);

#endif // CITY_WALLPAPER_GUARDIAN_H
```

- [ ] **Step 2: Create the implementation**

`src/city/wallpaper_guardian.c`:
```c
#include "wallpaper_guardian.h"

#include "city/finance.h"
#include "city/health.h"
#include "city/sentiment.h"

#define GUARDIAN_MAX_HAPPINESS 100
#define GUARDIAN_MAX_HEALTH 100
#define GUARDIAN_TREASURY_FLOOR 1000

void wallpaper_guardian_update(void)
{
    city_sentiment_set_happiness(GUARDIAN_MAX_HAPPINESS);
    city_sentiment_reset_protesters_criminals();
    city_health_set(GUARDIAN_MAX_HEALTH);

    int treasury = city_finance_treasury();
    if (treasury < GUARDIAN_TREASURY_FLOOR) {
        city_finance_treasury_add(GUARDIAN_TREASURY_FLOOR - treasury);
    }
}
```

- [ ] **Step 3: Register the source with CMake**

In `CMakeLists.txt`, inside `set(CITY_FILES ...)`, add the new file next to the existing wallpaper source. Change:
```cmake
    ${PROJECT_SOURCE_DIR}/src/city/wallpaper_poi.c
    ${PROJECT_SOURCE_DIR}/src/city/warning.c
```
to:
```cmake
    ${PROJECT_SOURCE_DIR}/src/city/wallpaper_guardian.c
    ${PROJECT_SOURCE_DIR}/src/city/wallpaper_poi.c
    ${PROJECT_SOURCE_DIR}/src/city/warning.c
```

Note: the Android build compiles `src/**` via its own CMake include and needs no separate list edit; confirm in Task 6's build.

- [ ] **Step 4: Hook the guardian into the daily tick**

In `src/game/tick.c`, add the include among the `city/` includes (alphabetical: it sorts just before `city/warning.h`, and after `city/view.h`):
```c
#include "city/wallpaper_guardian.h"
```
Then in `advance_day()`, immediately after the existing sentiment-update block (`tick.c:131-133`):
```c
    if (game_time_day() == 0 || game_time_day() == 8) {
        city_sentiment_update();
    }
```
add:
```c
    if (game_wallpaper_mode()) {
        wallpaper_guardian_update();
    }
```
(`game/game.h` is already included in `tick.c` at line 42, so `game_wallpaper_mode()` is in scope.)

- [ ] **Step 5: Compile**

Run: `cmake --build build -j 2>&1 | tail -n 20`
Expected: builds to completion; no errors; no warnings mentioning `wallpaper_guardian.c` or `tick.c`.

- [ ] **Step 6: Commit**

```bash
git add src/city/wallpaper_guardian.h src/city/wallpaper_guardian.c CMakeLists.txt src/game/tick.c
git commit -m "feat(wallpaper): add always-positive guardian (daily sentiment/health/treasury clamp)"
```

---

### Task 2: Disable the defeat/victory check in wallpaper mode (hard backstop)

**Files:**
- Modify: `src/city/victory.c` (add include; guard `city_victory_check()` at `:122-123`)

**Interfaces:**
- Consumes: `game_wallpaper_mode(void)` (`game/game.h:17`).

- [ ] **Step 1: Add the include**

In `src/city/victory.c`, add among the `game/` includes. The file has `#include "game/time.h"` at line 9; `game/game.h` sorts before it, so insert:
```c
#include "game/game.h"
#include "game/time.h"
```

- [ ] **Step 2: Guard the check**

Change the top of `city_victory_check()` (`victory.c:122`) from:
```c
void city_victory_check(void)
{
    if (scenario_is_open_play() && !data.force_win && !data.force_lose) {
        return;
    }
```
to:
```c
void city_victory_check(void)
{
    if (game_wallpaper_mode()) {
        return;
    }
    if (scenario_is_open_play() && !data.force_win && !data.force_lose) {
        return;
    }
```
This is the independent backstop: the game-over path (and the victory dialog, and the scenario `force_lose`/cheat paths, which are only read further down in this function) can never fire in wallpaper mode.

- [ ] **Step 3: Compile**

Run: `cmake --build build -j 2>&1 | tail -n 20`
Expected: builds clean; no warnings mentioning `victory.c`.

- [ ] **Step 4: Commit**

```bash
git add src/city/victory.c
git commit -m "feat(wallpaper): never trigger defeat/victory in wallpaper mode"
```

---

### Task 3: Disable fires and collapses in wallpaper mode

**Files:**
- Modify: `src/building/maintenance.c` (add include; guard `building_maintenance_check_fire_collapse()` at `:170`)

**Interfaces:**
- Consumes: `game_wallpaper_mode(void)` (`game/game.h:17`).

- [ ] **Step 1: Add the include**

In `src/building/maintenance.c`, add `#include "game/game.h"` in the `game/` include group (alphabetical position among the existing `#include "game/..."` lines).

- [ ] **Step 2: Guard the function**

Change the top of `building_maintenance_check_fire_collapse()` (`maintenance.c:170`) from:
```c
void building_maintenance_check_fire_collapse(void)
{
    city_sentiment_reset_protesters_criminals();
```
to:
```c
void building_maintenance_check_fire_collapse(void)
{
    if (game_wallpaper_mode()) {
        return;
    }
    city_sentiment_reset_protesters_criminals();
```
`fire_risk`/`damage_risk` never accumulate, so nothing ignites (>100) or collapses (>200). The `city_sentiment_reset_protesters_criminals()` call normally made here is redundant in wallpaper mode — the guardian (Task 1) resets protesters/criminals every day.

- [ ] **Step 3: Compile**

Run: `cmake --build build -j 2>&1 | tail -n 20`
Expected: builds clean; no warnings mentioning `maintenance.c`.

- [ ] **Step 4: Commit**

```bash
git add src/building/maintenance.c
git commit -m "feat(wallpaper): no fires or collapses in wallpaper mode"
```

---

### Task 4: Prevent invasions in wallpaper mode

**Files:**
- Modify: `src/game/tick.c` (guard `scenario_invasion_process()` at `:88`)
- Modify: `src/game/game.c` (add include; clear pre-scheduled invasions in `game_init_wallpaper()`)

**Interfaces:**
- Consumes: `game_wallpaper_mode(void)` (`game/game.h:17`); `scenario_invasion_process(void)`, `scenario_invasion_clear(void)` (`scenario/invasion.h:71,43`).

- [ ] **Step 1: Skip invasion processing (no new invasions launch)**

In `src/game/tick.c`, in `advance_month()`, change `:88` from:
```c
    scenario_invasion_process();
```
to:
```c
    if (!game_wallpaper_mode()) {
        scenario_invasion_process();
    }
```
(`scenario/invasion.h` and `game/game.h` are already included in `tick.c`.)

- [ ] **Step 2: Clear anything pre-scheduled in the loaded save**

In `src/game/game.c`, add `#include "scenario/invasion.h"` in the `scenario/` include group. Then in `game_init_wallpaper()`, after the save loads successfully — change:
```c
    if (game_file_load_saved_game(save_path) != FILE_LOAD_SUCCESS) {
        errlog("wallpaper mode: failed to load 'wallpaper.svx'");
        return 0;
    }
    formation_set_selected(0); // a loaded save may have a legion selected; keep the map view clean
```
to:
```c
    if (game_file_load_saved_game(save_path) != FILE_LOAD_SUCCESS) {
        errlog("wallpaper mode: failed to load 'wallpaper.svx'");
        return 0;
    }
    scenario_invasion_clear(); // wallpaper: drop any invasions scheduled in the save
    formation_set_selected(0); // a loaded save may have a legion selected; keep the map view clean
```

- [ ] **Step 3: Compile**

Run: `cmake --build build -j 2>&1 | tail -n 20`
Expected: builds clean; no warnings mentioning `tick.c` or `game.c`.

- [ ] **Step 4: Commit**

```bash
git add src/game/tick.c src/game/game.c
git commit -m "feat(wallpaper): prevent invasions in wallpaper mode"
```

---

### Task 5: Pacify wolves in wallpaper mode

**Files:**
- Modify: `src/figure/combat.c` (add include; gate aggressive-animal attack initiation at `:461-472`)
- Modify: `src/figure/formation_herd.c` (add include; force `attacking_animals = 0` for wolves at `:214-223`)

**Interfaces:**
- Consumes: `game_wallpaper_mode(void)` (`game/game.h:17`).

- [ ] **Step 1: Stop wolves initiating attacks (combat.c)**

In `src/figure/combat.c`, add `#include "game/game.h"` in the `game/` include group.

In `figure_combat_attack_figure_at()`, just after `int attack = 0;` (`combat.c:449`), introduce a named guard:
```c
        int attack = 0;
        int animal_may_attack = (category & FIGURE_CATEGORY_AGGRESSIVE_ANIMAL) && !game_wallpaper_mode();
```
Then in the three aggressive-animal branches (`:466-472`), replace each `category & FIGURE_CATEGORY_AGGRESSIVE_ANIMAL` with `animal_may_attack`. The branches change from:
```c
        } else if (category & FIGURE_CATEGORY_AGGRESSIVE_ANIMAL && opponent_category & FIGURE_CATEGORY_CITIZEN) {
            attack = 1;
        } else if (category & FIGURE_CATEGORY_AGGRESSIVE_ANIMAL && opponent_category & FIGURE_CATEGORY_ARMED) {
            attack = 1;
        } else if (category & FIGURE_CATEGORY_AGGRESSIVE_ANIMAL && opponent_category & FIGURE_CATEGORY_HOSTILE &&
            !(opponent_category & FIGURE_CATEGORY_NATIVE)) {
            attack = 1;
```
to:
```c
        } else if (animal_may_attack && opponent_category & FIGURE_CATEGORY_CITIZEN) {
            attack = 1;
        } else if (animal_may_attack && opponent_category & FIGURE_CATEGORY_ARMED) {
            attack = 1;
        } else if (animal_may_attack && opponent_category & FIGURE_CATEGORY_HOSTILE &&
            !(opponent_category & FIGURE_CATEGORY_NATIVE)) {
            attack = 1;
```
In wallpaper mode `animal_may_attack` is always 0, so no wolf ever enters the attack state. Normal mode is unchanged (identical to `category & FIGURE_CATEGORY_AGGRESSIVE_ANIMAL`).

- [ ] **Step 2: Stop wolf packs hunting (formation_herd.c)**

In `src/figure/formation_herd.c`, add `#include "game/game.h"` in the `game/` include group.

In the `FIGURE_WOLF` case of the update function, after the `missile_attack_timeout` block and before `break;` (`:222-223`), change:
```c
            if (m->missile_attack_timeout) {
                attacking_animals = 1;
            }
            break;
```
to:
```c
            if (m->missile_attack_timeout) {
                attacking_animals = 1;
            }
            if (game_wallpaper_mode()) {
                attacking_animals = 0;
            }
            break;
```
Defense-in-depth: even if a wolf were somehow in an attack state, the pack never coordinates a hunt (`move_animals` is only called with `attacking_animals` set), so wolves only roam.

- [ ] **Step 3: Compile**

Run: `cmake --build build -j 2>&1 | tail -n 20`
Expected: builds clean; no warnings mentioning `combat.c` or `formation_herd.c`.

- [ ] **Step 4: Commit**

```bash
git add src/figure/combat.c src/figure/formation_herd.c
git commit -m "feat(wallpaper): pacify wolves in wallpaper mode"
```

---

### Task 6: On-device behavior verification (wallpaper-qa soak)

**Files:**
- No source changes. Uses the Android build + `.claude/skills/wallpaper-qa/wallpaper_qa.py`.

**Interfaces:**
- Consumes: everything from Tasks 1–5.

- [ ] **Step 1: Build the debug APK**

From repo root:
```bash
cd android && JAVA_HOME="/Applications/Android Studio.app/Contents/jbr/Contents/Home" ./gradlew :augustus:assembleDebug 2>&1 | tail -n 5
```
Expected: `BUILD SUCCESSFUL`. (Gradle needs `JAVA_HOME` set in a non-login shell.)

- [ ] **Step 2: Install to the emulator and start a clean, fast wallpaper session**

```bash
cd /Users/ilyapomaskin/work/augustus/.claude/skills/wallpaper-qa
ANDROID_SERIAL=emulator-5554 python3 wallpaper_qa.py op \
  kill install set:speed=100 set_wallpaper show logcat_clear
```
Expected: installs, sets the wallpaper, renders the city, clears logcat. (`speed=100` = fastest sim, so many game-days elapse per real second — the guardian runs on days 0 & 8 of each month, so a few minutes covers many game-years.)

- [ ] **Step 3: Soak, then screenshot**

```bash
ANDROID_SERIAL=emulator-5554 python3 wallpaper_qa.py op wait:300 screenshot:always_positive_soak
```
Expected: `qa_out/always_positive_soak.png` shows an intact, populated city — no burning/collapsed buildings, no enemy or rioter figures.

- [ ] **Step 4: Assert no negative events in the log**

```bash
adb -s emulator-5554 logcat -d | \
  grep -iE 'invasion|enemy|rioter|riot|fire|collaps|plague|sickness|fired|defeat|game over|bankrupt|in debt' \
  || echo "CLEAN: no negative-event log lines"
```
Expected: `CLEAN: no negative-event log lines` (or only benign matches — inspect any hit).

- [ ] **Step 5: Regression — the wallpaper itself still works**

```bash
ANDROID_SERIAL=emulator-5554 python3 wallpaper_qa.py run all
```
Expected: all existing wallpaper-qa scenarios pass (render, scale/brightness/speed/map-change settings, POI camera, recenter cadence, pause-when-hidden). Confirms the guardian/guards didn't regress wallpaper behavior.

- [ ] **Step 6: Sanity — normal gameplay still has threats**

Confirm the guards are wallpaper-gated: launch the app normally (not `--wallpaper`) on the emulator and load any scenario with an invasion/fire; verify invasions, fires and the defeat check still occur. (Spot check — no automation needed; every change is behind `game_wallpaper_mode()`.)

- [ ] **Step 7: Commit any QA artifacts worth keeping (optional)**

```bash
git add qa_out/always_positive_soak.png
git commit -m "test(wallpaper): always-positive soak evidence"
```

---

## Self-Review

**1. Spec coverage** — every spec section maps to a task:
- §1 guardian (sentiment/health/treasury) → Task 1.
- §2 loss/victory backstop → Task 2.
- §2 fire/collapse → Task 3.
- §3 invasions (skip process + clear at init) → Task 4.
- §4 wolves (combat.c + formation_herd.c) → Task 5.
- §5 overrun → transitive (no separate task, as designed; covered by Tasks 3–5 + guardian).
- §6 build wiring → Task 1 Step 3.
- Verification plan → Task 6.
No gaps.

**2. Placeholder scan** — no TBD/TODO; every code step shows complete before/after code and exact commands. The only "optional" step (Task 6 Step 7) is genuinely optional artifact housekeeping, not a deferred implementation.

**3. Type consistency** — `wallpaper_guardian_update(void)` is declared (Task 1 Step 1), defined (Step 2), and called (Step 4) with matching signature. All consumed APIs (`city_sentiment_set_happiness`, `city_sentiment_reset_protesters_criminals`, `city_health_set`, `city_finance_treasury`, `city_finance_treasury_add`, `scenario_invasion_process`, `scenario_invasion_clear`, `game_wallpaper_mode`) match the verified headers. The `animal_may_attack` local (Task 5) is `int` and replaces an `int`-context bitmask test — no type change.
