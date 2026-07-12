---
name: augustus-wallpaper-qa
description: Use when testing or reproducing behavior of the Augustus Android live wallpaper on a device/emulator — verifying it renders, that Scale/Brightness/Speed/Map-change settings apply, camera recenter on hide, pause-when-hidden, or triggering the set-wallpaper flow from adb.
---

# Augustus Wallpaper QA

## Overview

QA for the Android live wallpaper is expressed as **atomic operations** composed
into **flows**. Each operation is one small, chainable step on the `Wallpaper`
class in `wallpaper_qa.py`; a scenario is a sequence of them. You never write a
new bespoke script — you assemble the vocabulary. Operations are coordinate-free
(taps resolve by `uiautomator` text/resource-id) and start from a clean state
(kill the app *and* a wedged chooser) so runs are repeatable.

## When to use

- Verifying the wallpaper renders the city on an emulator/device.
- Checking a setting applies (scale zoom, brightness dim, sim speed, map-change interval).
- Checking camera recenter cadence, or pause-when-hidden.
- Reproducing a wallpaper bug with an exact, replayable sequence.

Prereqs: a booted device/emulator (`ANDROID_SERIAL`, default `emulator-5554`),
the debug APK installed, and C3 data in `files/c3` (use `.install()` / `.provision(src)` once).

## Atomic operations (chainable; each returns `self`)

| Group | Ops |
|---|---|
| Bootstrap | `boot()`, `install(apk?)`, `provision(src)` |
| Clean state | `kill()` — force-stop app **+ chooser** + HOME |
| Settings | `set(scale=, brightness=, speed=, map=)`, `clear_settings()` |
| Activate | `open_settings()`, `set_wallpaper()` — via the app's own settings screen |
| Visibility | `hide()`, `show()`, `lock()`, `unlock()`, `wait(s)` |
| Observe | `logcat_clear()`, `screenshot(name)`, `save_log(name, grep?)` |
| Assert | `assert_set()`, `assert_recentered(n)`, `assert_not_recentered()`, `assert_paused()`, `recenter_count()` |

Settings apply on the next **visibility cycle** (`UPDATE_CONFIGS`), so a flow that
changes a setting must `set_wallpaper()` (or already be set) then `hide().show()`
before asserting/screenshotting.

## Composing a flow

A flow is a chain of atomic ops. Build ad-hoc, or add a function to `SCENARIOS`.

```python
from wallpaper_qa import Wallpaper

# scenario: scale x3 + dim, verify set, screenshot
(Wallpaper()
    .kill()
    .set(scale=3, brightness=40)
    .set_wallpaper().assert_set()
    .show().wait(4)
    .screenshot("scale3_dim"))

# scenario: recenter only on the chosen interval, not every switch
(Wallpaper()
    .kill().set(map=10).set_wallpaper()
    .logcat_clear().hide().show().wait(2)
    .assert_not_recentered())
```

## Quick reference (CLI)

```bash
python3 wallpaper_qa.py list                 # named example scenarios
python3 wallpaper_qa.py run scale_dim        # one scenario
python3 wallpaper_qa.py run all              # the whole sweep
# ad-hoc flow from atomic ops (op:arg / op:k=v,k=v):
python3 wallpaper_qa.py op kill set:scale=2,brightness=70 set_wallpaper show screenshot:adhoc
```

Artifacts (screenshots, logs) land in `./qa_out/`. Assertions raise, so a broken
scenario fails loudly with the failing op.

## Common mistakes

- **Screenshotting before a visibility cycle** — settings apply on `show()` after
  `set_wallpaper()`, not while editing. Always `hide().show()` (or `show()` fresh) first.
- **Skipping `kill()`** — a live SDL process or a wedged system chooser makes re-binds
  render black. Every flow starts with `kill()`.
- **Using the raw preview intent** — `set_wallpaper()` drives the app's real
  settings-screen button; don't `am start` the preview directly (it doesn't persist and
  wedges the chooser).
- **Original C3 `.sav` for the bundled save** — Augustus only loads its own `.svx`-format saves.
