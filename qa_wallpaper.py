#!/usr/bin/env python3
"""
QA automation for the Augustus Android live wallpaper.

Per iteration it:
  1. force-stops the app (clean state — a live SDL process left running makes
     re-binds render black),
  2. writes the wallpaper settings into the app's on-disk config
     (files/c3/augustus.ini), which native re-reads on UPDATE_CONFIGS,
  3. triggers the wallpaper change *from the application settings screen*
     (launches AssetSelectionActivity, taps its "Set wallpaper" button, then
     confirms in the system chooser) — not the external preview intent,
  4. shows the home screen (a real visibility cycle -> UPDATE_CONFIGS -> apply),
  5. screenshots + captures the relevant logcat lines.

Requires: a booted device/emulator with the debug APK installed and the C3 data
already provisioned in files/c3 (see Phase-2 provisioning). Uiautomator taps by
button *text*, so it survives layout/coordinate changes.

Usage:
  python3 qa_wallpaper.py                      # run the built-in iteration set
  python3 qa_wallpaper.py --config scale=2,brightness=55,speed=150,map=10
  ANDROID_SERIAL=emulator-5554 python3 qa_wallpaper.py
"""

import argparse
import os
import re
import subprocess
import sys
import tempfile
import time

SERIAL = os.environ.get("ANDROID_SERIAL", "emulator-5554")
ADB = os.environ.get("ADB", os.path.expanduser("~/Library/Android/sdk/platform-tools/adb"))
PKG = "com.github.Keriew.augustus.debug"
SERVICE = "org.libsdl.app.SDLActivity"
SETTINGS_ACTIVITY = f"{PKG}/com.github.Keriew.augustus.AssetSelectionActivity"
INI_REMOTE = "files/c3/augustus.ini"          # relative to the run-as home dir
INI_STAGE = "/sdcard/Download/augustus.ini"    # shell-writable staging path
OUTDIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "qa_out")

# config-key aliases -> the real augustus.ini keys
KEY_ALIASES = {
    "scale": "ui_wallpaper_scale",              # 1..5 (x1..x5)
    "brightness": "ui_wallpaper_brightness",    # 0..100 (100 = no dim)
    "speed": "ui_wallpaper_speed",              # game-speed percent, 0 = no override
    "map": "ui_wallpaper_map_change_minutes",   # 0 = every switch
}

def adb(*args, check=False, capture=False):
    cmd = [ADB, "-s", SERIAL, *args]
    return subprocess.run(cmd, check=check, text=True,
                          capture_output=capture)


def adb_out(*args):
    return adb(*args, capture=True).stdout or ""


def shell(cmd):
    return adb_out("shell", cmd)


def wait_boot(timeout=120):
    print(f"[*] waiting for {SERIAL} to be ready ...")
    adb("wait-for-device")
    deadline = time.time() + timeout
    while time.time() < deadline:
        if shell("getprop sys.boot_completed").strip() == "1":
            return True
        time.sleep(1)
    raise SystemExit("device did not finish booting")


def require_installed():
    if PKG not in shell("pm list packages"):
        raise SystemExit(f"{PKG} is not installed on {SERIAL}")


CHOOSER_PKG = "com.android.wallpaper.livepicker"


def force_stop():
    print("[*] force-stopping app + chooser (clean state) ...")
    adb("shell", "am", "force-stop", PKG)
    adb("shell", "am", "force-stop", CHOOSER_PKG)   # clear a wedged live-wallpaper chooser
    adb("shell", "input", "keyevent", "KEYCODE_HOME")
    time.sleep(1.5)


def write_config(cfg):
    """cfg: {alias: int}. Writes ONLY these keys (native defaults the rest)."""
    lines = "".join(f"{KEY_ALIASES[k]}={int(v)}\n" for k, v in cfg.items())
    print(f"[*] writing config: {cfg}")
    with tempfile.NamedTemporaryFile("w", suffix=".ini", delete=False) as f:
        f.write(lines)
        local = f.name
    try:
        adb("push", local, INI_STAGE, capture=True)
        # shell (uid shell, can read /sdcard) pipes into run-as (uid app, can write files/)
        adb("shell", f"cat {INI_STAGE} | run-as {PKG} sh -c 'cat > {INI_REMOTE}'")
    finally:
        os.unlink(local)
    back = shell(f"run-as {PKG} sh -c 'cat {INI_REMOTE}'").strip()
    print("    augustus.ini now:\n      " + back.replace("\n", "\n      "))


def ui_dump():
    # uiautomator dumps to a device file; read it back.
    out = shell("uiautomator dump /sdcard/ui_dump.xml")
    if "ERROR" in out or "null root" in out:
        return ""
    return shell("cat /sdcard/ui_dump.xml")


def _center_of(bounds):
    m = re.match(r"\[(\d+),(\d+)\]\[(\d+),(\d+)\]", bounds)
    if not m:
        return None
    x1, y1, x2, y2 = map(int, m.groups())
    return (x1 + x2) // 2, (y1 + y2) // 2


def _nodes(xml):
    for m in re.finditer(r"<node\b[^>]*?>", xml):
        yield dict(re.findall(r'([\w-]+)="([^"]*)"', m.group(0)))


def find_tap(xml, texts=None, rid_sub=None, pkg=None):
    """Return the center of the first node matching the given criteria."""
    for a in _nodes(xml):
        if pkg and a.get("package") != pkg:
            continue
        if rid_sub is not None and rid_sub not in a.get("resource-id", ""):
            continue
        if texts is not None and a.get("text") not in texts and a.get("content-desc") not in texts:
            continue
        c = _center_of(a.get("bounds", ""))
        if c:
            return c
    return None


def tap_when(desc, tries=10, delay=1.0, **crit):
    """Dump the UI and tap the first node matching crit; retry while it settles."""
    for _ in range(tries):
        xml = ui_dump()
        if xml:
            c = find_tap(xml, **crit)
            if c:
                print(f"    tap {desc} at {c}")
                adb("shell", "input", "tap", str(c[0]), str(c[1]))
                return True
        time.sleep(delay)
    return False


def current_wallpaper_component():
    return shell("dumpsys wallpaper")


def _launch_settings():
    # HOME first to dismiss any stray chooser/dialog left from a prior run,
    # then bring up the app's settings screen fresh.
    adb("shell", "input", "keyevent", "KEYCODE_HOME")
    time.sleep(1)
    adb("shell", "am", "start", "-n", SETTINGS_ACTIVITY)
    time.sleep(3)


def set_wallpaper_from_app():
    print("[*] launching app settings screen ...")
    _launch_settings()
    print("[*] tapping the app's 'Set wallpaper' button ...")
    found = tap_when("app 'Set wallpaper'", texts={"Set wallpaper", "SET WALLPAPER"}, pkg=PKG, tries=6)
    if not found:
        # stray UI on top? clear and relaunch once.
        print("    button not visible; clearing UI and relaunching ...")
        _launch_settings()
        found = tap_when("app 'Set wallpaper'", texts={"Set wallpaper", "SET WALLPAPER"}, pkg=PKG, tries=6)
    if not found:
        raise SystemExit("could not find the app's 'Set wallpaper' button")
    time.sleep(3)  # let the system live-wallpaper chooser open
    print("[*] confirming in the system chooser (once) ...")
    # AOSP LiveWallpaperChange: single confirm button, matched by resource-id.
    ok = tap_when("chooser confirm", rid_sub="set_wallpaper_button", tries=10)
    if not ok:
        ok = tap_when("chooser confirm (by text)", texts={"Set wallpaper"},
                      pkg="com.android.wallpaper.livepicker", tries=6)
    # some OEM choosers add a "where to set" dialog; tap it only if it appears.
    time.sleep(2)
    for label in ("Home and lock screen", "Home screen", "Set on home screen"):
        if tap_when(f"'{label}'", texts={label}, tries=1, delay=0.5):
            break
    if not ok:
        print("    WARNING: chooser confirm not found — set it once by hand, then re-run.")


def show_home():
    # real visibility cycle so onVisibilityChanged(true) -> UPDATE_CONFIGS fires
    adb("shell", "input", "keyevent", "KEYCODE_HOME")
    time.sleep(4)


def screenshot(path):
    with open(path, "wb") as f:
        f.write(subprocess.run([ADB, "-s", SERIAL, "exec-out", "screencap", "-p"],
                               capture_output=True).stdout)
    print(f"[*] screenshot -> {path} ({os.path.getsize(path)} bytes)")


def capture_log(path):
    grep = ("nativeRunMain|Engine onVisibilityChanged|C3 files|SDLEngine|"
            "onSurfaceChanged|Config key|Savegame version|recenter|unable to|error")
    log = shell("logcat -d")
    keep = [ln for ln in log.splitlines()
            if re.search(grep, ln, re.I) and "WPMS" not in ln
            and "WallpaperManagerService" not in ln]
    with open(path, "w") as f:
        f.write("\n".join(keep[-60:]))
    print(f"[*] log ({len(keep)} matched lines) -> {path}")


def iteration(name, cfg):
    print(f"\n===== QA iteration: {name} =====")
    force_stop()                 # (2) kill before each new iteration
    adb("logcat", "-c")
    write_config(cfg)            # (1) settings for this run
    set_wallpaper_from_app()     # (3) trigger change from the app settings only
    set_ok = SERVICE in current_wallpaper_component()
    print(f"[*] wallpaper component = Augustus service: {set_ok}")
    show_home()                  # (4) visibility cycle -> UPDATE_CONFIGS -> apply
    time.sleep(6)                # let the cold boot + first render settle
    os.makedirs(OUTDIR, exist_ok=True)
    screenshot(os.path.join(OUTDIR, f"{name}.png"))
    capture_log(os.path.join(OUTDIR, f"{name}.log"))
    print(f"[{'PASS' if set_ok else 'CHECK'}] iteration '{name}'")
    return set_ok


def parse_config(s):
    cfg = {}
    for pair in s.split(","):
        if not pair.strip():
            continue
        k, v = pair.split("=", 1)
        k = k.strip()
        if k not in KEY_ALIASES:
            raise SystemExit(f"unknown config key '{k}'; valid: {list(KEY_ALIASES)}")
        cfg[k] = int(v)
    return cfg


def main():
    ap = argparse.ArgumentParser(description="Augustus live-wallpaper QA automation")
    ap.add_argument("--config", help="one iteration, e.g. scale=2,brightness=55,speed=150,map=10")
    ap.add_argument("--name", default="custom", help="output name for --config")
    args = ap.parse_args()

    wait_boot()
    require_installed()

    if args.config:
        iteration(args.name, parse_config(args.config))
    else:
        # built-in sweep: baseline, then one knob at a time
        iteration("01_default", {"scale": 1, "brightness": 100, "speed": 100, "map": 0})
        iteration("02_scale_x3", {"scale": 3, "brightness": 100, "speed": 100, "map": 0})
        iteration("03_dim", {"scale": 1, "brightness": 40, "speed": 100, "map": 0})
        iteration("04_fast", {"scale": 1, "brightness": 100, "speed": 150, "map": 0})
        iteration("05_interval_10m", {"scale": 1, "brightness": 100, "speed": 100, "map": 10})

    print(f"\n[done] artifacts in {OUTDIR}")


if __name__ == "__main__":
    main()
