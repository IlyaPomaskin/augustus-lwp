# Wallpaper "Always Positive" Guardian — Design

**Date:** 2026-07-13
**Status:** Approved (design), pending spec review
**Branch context:** `feature/wallpaper-poi-camera`

## Goal

In **live-wallpaper mode only**, the displayed city must always look positive: no
defeat, no invasions, no enemy/animal overrun, no fires or collapses, no plague,
no rioters, no bankruptcy. Achieved **preventively** — the *causes* are clamped so
nothing destructive ever appears on screen (as opposed to letting a threat appear
and reacting to it).

Normal gameplay is completely unaffected. Every change is gated on the existing
`game_wallpaper_mode()` predicate (`src/game/game.c:87`).

## Non-goals

- No reactive "autopilot" that plays the city via real mechanics (build prefectures,
  fund legions, adjust taxes). Rejected: more code, brittle, and visually worse
  (threats briefly appear before being fixed).
- No new user-facing setting/toggle. Always-positive is intrinsic to wallpaper mode.
- Normal (non-wallpaper) gameplay difficulty/behavior is not touched.

## Approach

**Approach #2 (chosen): one policy module + a few source-level guards.**

A single `wallpaper_guardian` module owns the "clamp the positive metrics" policy.
A small number of subsystems get one-line `game_wallpaper_mode()` guards where
*disabling the subsystem* is cleaner than clamping a value after the fact.

Rejected alternatives:
- **Scattered inline clamps** across ~7 files — policy smeared everywhere, easy to
  miss one.
- **Pure per-tick guardian clamping everything** — loops all buildings every tick
  (battery/perf cost on a wallpaper) and must undo a loss *after* it fires (hacky).

## Components

### 1. New module: `src/city/wallpaper_guardian.{c,h}`

Mirrors the existing `src/city/wallpaper_poi.{c,h}` convention. Single entry point:

```c
void wallpaper_guardian_update(void);
```

Per invocation it clamps the "soft" city metrics using existing public APIs
(idempotent — safe to call repeatedly):

| Threat | Clamp (existing API) |
|---|---|
| Unrest / rioters | `city_sentiment_set_happiness(100)` (`sentiment.h:10`) + `city_sentiment_reset_protesters_criminals()` (`sentiment.h:17`) |
| Disease / plague | `city_health_set(100)` (`health.h` / `health.c:34`) |
| Bankruptcy | `if (city_finance_treasury() < TREASURY_FLOOR) city_finance_treasury_add(...)` (`finance.h:34,36`) |

`TREASURY_FLOOR` is a named constant (small positive value, e.g. 1000) — no magic
numbers.

**Cadence & hook:** called from `advance_day()` in `src/game/tick.c`, immediately
after the existing `city_sentiment_update()` block (`tick.c:131-133`), behind
`if (game_wallpaper_mode())`. Rationale: sentiment recomputes on days 0 & 8; health
and finance recompute monthly (`advance_month` runs first on day 0). Running the
guardian daily, *after* those recomputes, guarantees no metric dips for more than a
day, so a rioter/sickness event never gets a chance to trigger. Cost is a handful of
setter calls per day — no building loops.

### 2. Source-level guards (skip subsystem — cleaner than clamping)

Each is a single early `game_wallpaper_mode()` check:

- **Loss & victory dialogs (hard safety net)** — `city_victory_check()`
  (`src/city/victory.c:122`): add `if (game_wallpaper_mode()) return;` at the top. No
  FIRED/defeat *or* victory dialog ever pops in wallpaper mode. This is the deliberate
  **independent backstop**: it disables the game-over path itself, so even if some
  cause ever slipped past the preventive clamps (§1/§3/§4), game-over still cannot
  fire. It also neutralizes the scenario-scripted `force_lose` and cheat paths, since
  those only take effect *inside* `city_victory_check()` (`victory.c:132-136`) — the
  early return runs before they are read.
- **Fire & collapse** — `building_maintenance_check_fire_collapse()`
  (`src/building/maintenance.c:170`): add `if (game_wallpaper_mode()) return;` at the
  top. `fire_risk`/`damage_risk` never accrue, so nothing ignites (>100) or collapses
  (>200).

### 3. Invasions

- **No new invasions launch:** guard the monthly call at `src/game/tick.c:88` —
  `if (!game_wallpaper_mode()) scenario_invasion_process();`
- **Drop anything pre-scheduled in the save:** call `scenario_invasion_clear()`
  (`scenario/invasion.h:43`) once in `game_init_wallpaper()` (`src/game/game.c:180`),
  after the save loads.

### 4. Wolf pacification

Wolves (`FIGURE_CATEGORY_AGGRESSIVE_ANIMAL`) attack citizens/soldiers. Two guards:

- **Source (primary):** `figure_combat_attack_figure_at()`
  (`src/figure/combat.c:466-472`) — skip the three `FIGURE_CATEGORY_AGGRESSIVE_ANIMAL`
  attack branches when `game_wallpaper_mode()`. No wolf ever *initiates* an attack.
- **Defense-in-depth:** `formation_herd_update()`
  (`src/figure/formation_herd.c:214-222`) — force `attacking_animals = 0` for the
  `FIGURE_WOLF` case in wallpaper mode, so the pack/`missile_attack_timeout` path can
  never drive a coordinated hunt. Wolves keep roaming (their `FIGURE_ACTION_197`/`196`
  states) but never enter `FIGURE_ACTION_199_WOLF_ATTACKING`.

### 5. "Overrun" is prevented transitively

Overrun = enemies/rioters destroying buildings and killing population. With no
invasions (§3, no enemies spawn), sentiment floored (§1, no rioters spawn), and
wolves pacified (§4), nothing that attacks buildings/citizens ever exists. No
dedicated overrun mechanism is needed.

### 6. Build

Add `src/city/wallpaper_guardian.c` to `CMakeLists.txt` (and any file-list used by
the Android NDK/CMake build) alongside the other `src/city/*.c` entries.

## Data flow

```
advance_day()  (tick.c)
  ├─ [day 0] advance_month() → city_health_update / finance / invasion_process(guarded)
  ├─ [day 0|8] city_sentiment_update()
  └─ if game_wallpaper_mode(): wallpaper_guardian_update()
        ├─ city_sentiment_set_happiness(100) + reset_protesters_criminals()
        ├─ city_health_set(100)
        └─ treasury floor

game_tick_run() (tick.c)
  └─ city_victory_check()  → early-return in wallpaper mode

building_maintenance_check_fire_collapse() → early-return in wallpaper mode
figure_combat_attack_figure_at() → aggressive-animal branches skipped in wallpaper mode
formation_herd_update() → attacking_animals forced 0 for wolves in wallpaper mode

game_init_wallpaper() → scenario_invasion_clear() once
```

## Edge cases / known limitations

- **Pre-existing hostile figures in the loaded `wallpaper.svx`** (enemies or rioters
  already alive when the save was made): counts are reset and no new ones spawn, but
  already-instantiated figures are not force-despawned. A curated wallpaper save has
  none. If a save ever ships with live hostiles, add a one-time figure sweep at init
  — not built now (YAGNI).
- **Health/sentiment set to 100** may look "too perfect" (constant full happiness).
  Acceptable and intended for a wallpaper. If a subtler look is wanted later, floor to
  a high-but-not-max value instead — isolated to the guardian.

## Verification (via wallpaper-qa harness)

1. Launch the wallpaper on the emulator; provision a save known to *previously* misbehave
   (or inject an invasion) to prove suppression.
2. Fast-forward simulation time over a long accelerated soak (many sim-years).
3. Assert:
   - logcat shows **no** `MESSAGE_FIRED`, invasion, fire, collapse, plague, or riot
     messages;
   - screenshots show the city intact — no burning/collapsed buildings, no enemy or
     rioter figures;
   - population and treasury never collapse (both non-decreasing / above floor).
4. Regression: run the existing wallpaper-qa scenarios (render, settings apply, POI,
   pause-when-hidden) to confirm no behavior change to the wallpaper itself.
5. Sanity: normal (non-wallpaper) build still takes invasions/fires/losses (spot check
   — the guards are all `game_wallpaper_mode()`-gated).

## Risks

- **Update ordering:** the guardian must run *after* `city_sentiment_update()` each day,
  else its clamp is immediately overwritten. Enforced by hook placement (`tick.c` after
  line 133). The implementation plan must lock this ordering with a test/soak.
- **Native-only testing:** no lightweight C unit harness for these paths; verification
  leans on the wallpaper-qa device soak. Acceptable given the changes are small,
  gated, and observable.
