# Augustus Live Wallpaper — Point-of-Interest Camera Design

Date: 2026-07-12
Status: Ready for implementation plan
**Plan-confidence status:** Ready for implementation plan (iteration 1, confidence 91%)

Parent spec: `docs/superpowers/specs/2026-07-11-augustus-live-wallpaper-design.md`
Prior phase: `docs/superpowers/specs/2026-07-12-augustus-live-wallpaper-phase3-settings-design.md`
(Phases 1–3 are implemented and merged to `master`; the Android live wallpaper is device-verified.)

Reference implementation being ported: OpenTTD live-wallpaper fork at `~/work/openTTD`,
`src/video/gles_poi.cpp` / `gles_poi.h` (the POI scanner), with cadence in
`src/video/sdl2_gles_v.cpp` and `android/.../OpenTTDWallpaperService.java`.

## Goal

Replace the wallpaper's random-tile recenter with a **scored point-of-interest
(POI) picker**: each time the wallpaper switches to a new location it lands on an
*interesting* part of the city (a landmark, a dense neighborhood, a busy industry
cluster) instead of a random tile. Faithfully port OpenTTD's POI algorithm
structure, adapted to Caesar III objects. Also add an ADB-triggerable "jump to
next POI" broadcast for QA, and wire it into the wallpaper-qa skill.

## Scope & definition of done

- **Camera motion model:** instant jump on hide (reuses the existing
  `WALLPAPER_EVENT_HIDE` path). The jump happens while the wallpaper is hidden, so
  the user only ever sees a fresh, interesting location — matching OpenTTD.
- **Selection model:** faithful port of OpenTTD's pipeline —
  scan → score → edge-drop → sort → spatial-dedup → random-sample → cycle.
- **Always on, no user setting.** The POI picker replaces
  `city_view_go_to_random_tile()` at the recenter call sites. Random-tile becomes
  the **internal fallback** when a scan yields zero POIs (e.g. a nearly-empty map).
- **ADB trigger:** a `NEXT_POI` broadcast advances to the next POI on demand,
  usable while the wallpaper is visible (so the jump is observable for QA).
- **wallpaper-qa skill:** a `next_poi` atomic op + assertion + scenario + docs.
- **Definition of done = it builds** — desktop `cmake --build build` clean, and
  the APK builds (`./gradlew :augustus:assembleDebug`). On-device behavior is a
  **deferred QA pass** (same model as Phases 1–3), now scriptable via `next_poi`.

Single-city note: Augustus loads one city (`wallpaper.svx`); there is no
multi-map rotation, so OpenTTD's "list wraps → rotate to a different map" step is
**not** ported. When the list wraps, we simply continue cycling.

## Architecture

### §A — New module `src/city/wallpaper_poi.{c,h}`

Mirrors OpenTTD's `gles_poi`. Public API (only two entry points):

```c
void wallpaper_poi_invalidate(void);  // force a rescan on the next advance
void wallpaper_poi_next(void);         // ensure scanned, advance, jump camera (or fallback)
```

Internal state (file-static):

```c
typedef struct { int grid_offset; int score; } wallpaper_poi;
static wallpaper_poi poi_list[POI_SAMPLE_COUNT];   // final cycled set
static int poi_count;
static int poi_index;
```

`wallpaper_poi_next()` (mirrors OpenTTD `PrepareBackground`):

1. If invalidated (`poi_count == 0`) **or** the list has been fully cycled
   (advancing would move past the last entry) → `scan()` + re-sample and set
   `poi_index = 0`. Otherwise `poi_index += 1`. A fresh random sample is thus drawn
   on load and once per full cycle — no reliance on `building_count()`.
2. If `poi_count == 0` (scan found nothing) → `city_view_go_to_random_tile()`
   (fallback) and return; the next advance re-scans, so it self-heals once the
   city has buildings.
3. Else center on the POI's footprint-center tile (§B):
   `city_view_go_to_grid_offset(poi_list[poi_index].grid_offset)` — the engine's
   instant, boundary-clamped centering (`src/city/view.c:531`).
4. `log_info("Wallpaper POI", <score>, grid_offset)` so QA and the `next_poi`
   assertion can observe picks in logcat.

`wallpaper_poi_invalidate()` sets `poi_count = 0` (forces a rescan next call).

### §B — Scanners → candidate list

`scan()` collects candidates `{ grid_offset, score }` into a fixed buffer
(`POI_MAX_CANDIDATES = 1024`; extras beyond the cap are dropped — acceptable, we
only keep the top ~50). Three scanners map OpenTTD's four:

| OpenTTD scanner | Augustus equivalent | Method |
| --- | --- | --- |
| Stations (facility + pop + cluster) | **Landmarks** | Iterate notable types via `building_first_of_type(t)` → `next_of_type`; add each `BUILDING_STATE_IN_USE` building at its footprint-center tile with the type's table score. |
| Towns (pop ≥ threshold, spaced) | **Population density** | Single O(n) pass bucketing houses into `POI_CELL_TILES`-square cells by summed `b->house_population`; each cell over threshold → one candidate at the cell's representative tile, `score = min(POI_POP_SCORE_CAP, cell_pop / POI_POP_PER_POINT)`. |
| Rail junctions (activity clusters) | **Industry / activity clusters** | Same cell pass counts farms/workshops/warehouses/granaries/docks; cells with ≥ `POI_INDUSTRY_MIN` such buildings → candidate, `score = POI_INDUSTRY_SCORE` (or `POI_INDUSTRY_SCORE_DENSE` when ≥ `POI_INDUSTRY_DENSE`). |
| Lighthouses (rare 5% high-score pick) | *dropped* | The rare-random-pick mechanic is not ported; those landmarks (incl. the lighthouse) are covered by the Landmarks scanner. |

**Landmark score table** (concrete defaults, fine-tuned in QA). Only
`BUILDING_STATE_IN_USE`:

| Score | Building types |
| --- | --- |
| 8 | `COLOSSEUM`, `HIPPODROME`, `ARENA` |
| 7 | Grand temples (`GRAND_TEMPLE_*`), `PANTHEON`, `ORACLE` |
| 6 | `SENATE`, `GOVERNORS_PALACE` |
| 5 | `GOVERNORS_VILLA`, `LARGE_MAUSOLEUM`, `CARAVANSERAI`, `LIGHTHOUSE`, `CITY_MINT`, `MILITARY_ACADEMY` |
| 4 | `THEATER`, `AMPHITHEATER`, `GLADIATOR_SCHOOL`, large temples (`LARGE_TEMPLE_*`), `GOVERNORS_HOUSE`, `TRIUMPHAL_ARCH`, `OBELISK` |
| 3 | `FORUM`, forts (`FORT_*`) |

Multi-tile buildings are added once at their footprint-center tile — `b->grid_offset`
shifted by half the footprint (`b->size / 2`) via `map_grid_add_delta`, falling back
to `b->grid_offset` if the shifted tile is invalid — so `city_view_go_to_grid_offset`
frames the structure's middle rather than a corner.

Cell bucketing helpers: `map_grid_offset_to_x/y(offset)`, `map_grid_offset(x,y)`,
`map_grid_is_valid_offset`, `map_grid_width/height`. A cell's representative tile
is the `grid_offset` of its highest-scoring qualifying building — for a population
cell the house with the greatest `house_population`, for an industry cell the first
industry building encountered — so the tile is always valid and lands on real
content (no geometric-center clamping). The single bucketing pass tracks that best
building per cell alongside the running population / industry counts.

### §C — Post-processing (identical to OpenTTD `ScanMapPOIs`)

Applied to the candidate buffer, in order:

1. **Edge drop:** discard candidates whose tile is within `POI_EDGE_MARGIN = 20`
   tiles of any map edge.
2. **Sort** by `score` descending. Landmarks (3–8) intentionally outrank
   population/industry cells (≤5); lower-scored cells still surface via the random
   sample from the top-50 pool — matching OpenTTD's pure score-sort.
3. **Spatial dedup into a pool** (`POI_POOL_MAX = 50`): walk the sorted
   candidates; add one only if it is at least `POI_MIN_SPACING = 10` tiles
   (Chebyshev distance on tile x/y) from every candidate already in the pool.
   Stop at `POI_POOL_MAX`.
4. **Random sample:** if the pool has ≤ `POI_SAMPLE_COUNT = 20` entries, copy all
   into `poi_list`; otherwise partial Fisher–Yates to pick 20 (higher-scoring
   candidates are more likely to be in the pool, but shown order is random).
5. `poi_count` = chosen count; `poi_index = 0`.

**Explicitly dropped from the port** (present in `gles_poi` but not carried over):
per-POI `zoom` hint (OpenTTD computes but never applies it — the user's scale
setting governs zoom here too), per-POI `delay_ms` (cadence is the hide event, not
a stored delay), and the debug influence-line overlay (`DrawPOIMarkers`).

### §D — Integration (call-site edits)

- `src/platform/SDL2/augustus.c` — in the `WALLPAPER_EVENT_HIDE` handler
  (currently `augustus.c:407-415`), replace `city_view_go_to_random_tile();`
  inside the `wallpaper_should_recenter()` guard with `wallpaper_poi_next();`.
  The interval gating is unchanged — POI advance inherits it.
- `src/game/game.c` — the first-frame seed (currently
  `city_view_go_to_random_tile()` at `game.c:216` in `game_init_wallpaper`) becomes
  `wallpaper_poi_invalidate(); wallpaper_poi_next();` (scans and shows POI[0] on
  load). Call `wallpaper_poi_invalidate()` at any other point a city save is
  (re)loaded in wallpaper mode, so a stale list is never reused.
- Build: add `src/city/wallpaper_poi.c` to the source list that already contains
  `src/city/view.c` (`CMakeLists.txt` / the module's `CMakeLists`).

### §E — ADB-triggerable POI change

Path: `adb broadcast` → runtime `BroadcastReceiver` in the wallpaper service →
`pushWallpaperEvent` → JNI → new `SDL_USEREVENT` code → `wallpaper_poi_next()`.
Mirrors OpenTTD's `ACTION_JUMP_POI`.

- **`src/platform/android/android.h`:** add
  `#define WALLPAPER_EVENT_NEXT_POI (WALLPAPER_EVENT_CODE_BASE + 3)`.
- **`src/platform/SDL2/augustus.c`** `SDL_USEREVENT` switch: add
  `else if (event->user.code == WALLPAPER_EVENT_NEXT_POI) { if (game_wallpaper_mode()) wallpaper_poi_next(); }`.
  This calls `wallpaper_poi_next()` directly — deliberately **not** through the
  `wallpaper_should_recenter()` interval gate — so the QA trigger always advances
  immediately, regardless of the map-change interval.
- **`SDLActivity.java`:** add constant `WALLPAPER_EVENT_NEXT_POI = 3` (kept in
  sync with android.h; `pushWallpaperEvent` already offsets by
  `WALLPAPER_EVENT_CODE_BASE`, so Java `3` → C `103`). In `SDLEngine`, register a
  runtime `BroadcastReceiver` for action `com.github.Keriew.augustus.NEXT_POI`
  whose `onReceive` calls `pushWallpaperEvent(WALLPAPER_EVENT_NEXT_POI)`. Register
  it when the engine/surface starts and unregister on
  `onSurfaceDestroyed`/`onDestroy`. On API 33+ register with
  `Context.RECEIVER_EXPORTED` so an `adb`-sent broadcast can reach it. Register
  **only in debug builds** (`if (BuildConfig.DEBUG)`) — it is a QA-only hook and
  release builds must not expose it.

ADB command (targets the debug applicationId; the action string is
package-qualified but distinct from it):

```
adb shell am broadcast -a com.github.Keriew.augustus.NEXT_POI -p com.github.Keriew.augustus.debug
```

`wallpaper_poi_next()` auto-scans on its first call, so `NEXT_POI` works even
before any hide event.

### §F — wallpaper-qa skill additions

Land **with** the feature (the `NEXT_POI` broadcast does not exist until then).

- **`.claude/skills/wallpaper-qa/wallpaper_qa.py`:**
  - new atomic op `next_poi(n=1)` → `am broadcast -a com.github.Keriew.augustus.NEXT_POI -p PKG`, repeated `n` times; returns `self`.
  - `poi_count()` / `assert_poi_changed(at_least=1)` — parse logcat for the
    `"Wallpaper POI"` pick line (mirrors the existing
    `recenter_count()`/`assert_recentered()` implementation).
  - new scenario `poi_cycle` in `SCENARIOS`:
    `kill().set_wallpaper().show().wait(2).logcat_clear().next_poi().wait(1).screenshot("poi_1").next_poi().wait(1).screenshot("poi_2").assert_poi_changed(2)`.
- **`.claude/skills/wallpaper-qa/SKILL.md`:** add a **POI** row to the ops table
  (`next_poi(n)`, `poi_count()`, `assert_poi_changed(n)`), add the raw ADB command
  to the CLI quick-reference, add `poi_cycle` to the scenario examples, and extend
  the frontmatter `description` to mention the point-of-interest camera / `next_poi`.

## Tunable constants (single header block in `wallpaper_poi.c`)

| Constant | Value | Meaning |
| --- | --- | --- |
| `POI_SAMPLE_COUNT` | 20 | POIs in the final cycled set |
| `POI_POOL_MAX` | 50 | max spatially-deduped candidates sampled from |
| `POI_MAX_CANDIDATES` | 1024 | pre-sort candidate buffer cap |
| `POI_EDGE_MARGIN` | 20 | tiles from map edge a POI must clear |
| `POI_MIN_SPACING` | 10 | min tile spacing between pooled POIs (Chebyshev) |
| `POI_CELL_TILES` | 15 | density/industry bucket cell size |
| `POI_POP_SCORE_CAP` | 5 | max score from a population cell |
| `POI_POP_PER_POINT` | 500 | population per +1 score |
| `POI_INDUSTRY_MIN` | 4 | industry buildings in a cell to qualify |
| `POI_INDUSTRY_DENSE` | 8 | industry count for the dense bonus |
| `POI_INDUSTRY_SCORE` / `_DENSE` | 3 / 5 | industry cell scores |

## Verification model

- **Per task:** the relevant build succeeds — desktop `cmake --build build` for the
  shared C module (`wallpaper_poi.c`, `augustus.c`, `game.c`, `android.h`) and
  `./gradlew :augustus:assembleDebug` for the Java/JNI changes. No unit-test
  harness (as in prior phases).
- **Deferred device QA (scriptable via `next_poi`):** set the wallpaper; step
  `next_poi` repeatedly and confirm the camera lands on distinct interesting
  locations (landmarks favored), screenshots differ, `assert_poi_changed` passes;
  confirm hide/show still advances; confirm a sparse/empty map falls back to a
  random tile without crashing.

## Key file map (insertion points)

| Concern | File / anchor |
| --- | --- |
| POI module (scan/score/dedup/sample/cycle) | `src/city/wallpaper_poi.{c,h}` (new) |
| Build registration | `CMakeLists` listing `src/city/view.c` |
| Recenter-on-hide call site | `src/platform/SDL2/augustus.c` HIDE handler (`~:410`) |
| First-frame seed + invalidate-on-load | `src/game/game.c` `game_init_wallpaper` (`~:216`) |
| `NEXT_POI` event code | `src/platform/android/android.h` |
| `NEXT_POI` native handler | `src/platform/SDL2/augustus.c` `SDL_USEREVENT` switch |
| `NEXT_POI` Java constant + receiver | `android/augustus/src/main/java/org/libsdl/app/SDLActivity.java` |
| QA op + docs | `.claude/skills/wallpaper-qa/wallpaper_qa.py`, `.claude/skills/wallpaper-qa/SKILL.md` |
| Building/type/coord APIs used | `src/building/building.h`, `src/building/type.h`, `src/map/grid.h`, `src/city/view.h` |

## Risks

- **Scoring quality is subjective.** The table/thresholds may over- or
  under-favor a category; they are centralized as named constants and tuned during
  the deferred QA pass. Not a correctness risk.
- **Empty/sparse maps.** A scan can legitimately return zero POIs; the random-tile
  fallback (§A step 2) covers this. Must be explicitly exercised.
- **Cost of scanning.** `scan()` iterates all buildings once (a few thousand) plus
  the notable-type linked lists; it runs only on load / hide / `NEXT_POI` / list
  wrap (all infrequent), so per-frame cost is zero. Candidate buffer is a fixed
  array (no per-scan allocation).
- **Coordinate validity.** Cell representative tiles are real building offsets
  (always valid); the footprint-center shift falls back to `b->grid_offset` when
  the shifted tile is invalid, and `city_view_go_to_grid_offset` clamps to map
  bounds regardless.
- **Java/native event-code drift.** `WALLPAPER_EVENT_NEXT_POI` must stay `3` in
  Java and `BASE+3` in `android.h`; documented at both sites (same convention as
  the existing three codes).
- **Build-only validation.** Compiling does not prove runtime; deferred to QA
  (now scriptable).

## Out of scope

- Smooth/animated panning between POIs (instant jump only, by decision).
- Any user-facing setting to toggle or tune POI behavior (always on).
- Multi-map rotation on list wrap (Augustus loads a single city).
- Per-POI zoom changes (the user's scale setting governs zoom).
- Porting the debug influence-line overlay.

## Open questions

_None at the design layer. Exact landmark score weights, cell size, and the other
constants in the tunables table are implementation-plan detail, tuned in QA._

## Confidence Survey

_All iteration-1 questions resolved (all Recommended options) and folded into the plan body — see the Reconciliation Log below. No open questions._

## Reconciliation Log

Append-only. Newest entry at the bottom.

### Iteration 1 — 2026-07-12 (initial)
- **Confidence:** 78% (cap from Unknowns)
- **Resolved:** none (first pass)
- **Still uncertain:** Unknowns dimension — scan-refresh trigger relies on an unverified `building_count()` semantic; landmark weights marked "draft/tunable"; multi-tile framing and cell-representative-tile resolution underspecified; cross-scanner score parity is an open balance decision.
- **New questions:** Q1.1 (rescan trigger), Q1.2 (multi-tile framing), Q1.3 (cell representative tile), Q1.4 (score parity/quota), Q1.5 (empty-map rescan), Q1.6 (receiver debug-only), Q1.7 (NEXT_POI gate bypass)

### Iteration 1 — 2026-07-12 (resolved)
- **Confidence:** 91% (was 78%; cap from Unknowns lifted)
- **Resolved:**
  - Q1.1 → rescan on load + on full-cycle wrap (drop `building_count()` heuristic) → §A `wallpaper_poi_next()`, state struct, Risks (scanning cost)
  - Q1.2 → center on footprint-center tile (`b->size / 2` shift, fallback to origin) → §B multi-tile note, landmark row, §A step 3
  - Q1.3 → cell representative tile = highest-scoring building's `grid_offset` → §B cell-helpers paragraph, Risks (coordinate validity)
  - Q1.4 → landmarks dominate via pure score-sort (no quota) → §C sort step
  - Q1.5 → re-scan each advance while empty (self-heals) → §A step 2
  - Q1.6 → register `NEXT_POI` receiver in debug builds only → §E SDLActivity bullet
  - Q1.7 → `NEXT_POI` bypasses the interval gate (always advances) → §E native-handler bullet
- **Still uncertain:** none at the tech-spec layer; remaining details (exact `b->size/2` view-space delta, log format, precise `CMakeLists` file) are implementation-plan detail.
- **New questions:** none
