#!/usr/bin/env python3
"""
Composable QA toolkit for the Augustus Android live wallpaper.

Every wallpaper QA action is an *atomic operation* — a small, single-purpose,
chainable method on `Wallpaper`. Compose them into *flows* to test scenarios;
each flow is just a sequence of atomic ops. No scenario needs its own bespoke
script — you assemble it from the vocabulary below.

    from wallpaper_qa import Wallpaper

    # a flow = a chain of atomic ops
    (Wallpaper()
        .kill()                     # clean state (force-stop app + chooser + HOME)
        .set(scale=3, brightness=40)# write settings into augustus.ini
        .set_wallpaper()            # activate via the app's own settings screen
        .assert_set()               # dumpsys: our service is the wallpaper
        .show().wait(4)             # visibility cycle -> UPDATE_CONFIGS -> apply
        .screenshot("scale3_dim"))  # capture

    # a scenario testing recenter cadence
    (Wallpaper()
        .kill().set(map=0).set_wallpaper()
        .logcat_clear()
        .hide().show()              # one home-screen switch
        .assert_recentered(1))      # camera jumped to a new tile

CLI:
    python3 wallpaper_qa.py list
    python3 wallpaper_qa.py run scale_dim
    python3 wallpaper_qa.py run recenter_every_switch
    python3 wallpaper_qa.py run all
    python3 wallpaper_qa.py op kill set:scale=2 set_wallpaper show screenshot:adhoc

Env: ANDROID_SERIAL (default emulator-5554), ADB (default SDK adb).
Assumes the debug APK is installed and C3 data is provisioned in files/c3
(see .install()/.provision() to bootstrap those).
"""

import os
import re
import subprocess
import sys
import tempfile
import time

PKG = "com.github.Keriew.augustus.debug"
SERVICE = "org.libsdl.app.SDLActivity"
SETTINGS_ACTIVITY = f"{PKG}/com.github.Keriew.augustus.AssetSelectionActivity"
CHOOSER_PKG = "com.android.wallpaper.livepicker"
INI_REMOTE = "files/c3/augustus.ini"        # relative to run-as home
INI_STAGE = "/sdcard/Download/augustus.ini"

# friendly setting alias -> real augustus.ini key (int values)
SETTINGS = {
    "scale": "ui_wallpaper_scale",             # 1..5 (x1..x5)
    "brightness": "ui_wallpaper_brightness",   # 0..100 (100 = no dim)
    "speed": "ui_wallpaper_speed",             # game-speed %, 0 = no override
    "map": "ui_wallpaper_map_change_minutes",  # 0 = every switch
}
RECENTER_MARK = "recenter to grid offset"
PAUSE_MARK = "nativePause()"
POI_MARK = "Wallpaper POI"


class Wallpaper:
    """Chainable atomic operations. Every method returns self so ops compose."""

    def __init__(self, serial=None, outdir=None):
        self.serial = serial or os.environ.get("ANDROID_SERIAL", "emulator-5554")
        self.adb_bin = os.environ.get("ADB", os.path.expanduser(
            "~/Library/Android/sdk/platform-tools/adb"))
        self.outdir = outdir or os.path.join(os.getcwd(), "qa_out")
        os.makedirs(self.outdir, exist_ok=True)

    # ---- low-level adb ----
    def _adb(self, *args, capture=False):
        return subprocess.run([self.adb_bin, "-s", self.serial, *args],
                              text=True, capture_output=capture)

    def _sh(self, cmd):
        return (self._adb("shell", cmd, capture=True).stdout or "")

    def _log(self, msg):
        print(f"  · {msg}")
        return self

    # ================= atomic operations =================

    def boot(self, timeout=120):
        """Wait until the device has finished booting."""
        self._adb("wait-for-device")
        end = time.time() + timeout
        while time.time() < end:
            if self._sh("getprop sys.boot_completed").strip() == "1":
                return self._log(f"device {self.serial} ready")
            time.sleep(1)
        raise SystemExit("device did not boot")

    def install(self, apk="android/augustus/build/outputs/apk/debug/augustus-debug.apk"):
        """Install/replace the debug APK."""
        self._adb("install", "-r", apk, capture=True)
        return self._log(f"installed {os.path.basename(apk)}")

    def provision(self, src):
        """Push a C3 data folder to the device and copy it into files/c3 (picker-free)."""
        self._adb("push", src, "/sdcard/Download/augustus-c3", capture=True)
        self._adb("shell", f"run-as {PKG} mkdir -p files/c3")
        self._adb("shell",
                  f"tar cf - -C /sdcard/Download/augustus-c3 . | run-as {PKG} tar xf - -C files/c3")
        return self._log("provisioned C3 data into files/c3")

    def kill(self):
        """Clean state: force-stop the app AND a possibly-wedged chooser, then HOME."""
        self._adb("shell", "am", "force-stop", PKG)
        self._adb("shell", "am", "force-stop", CHOOSER_PKG)
        self._adb("shell", "input", "keyevent", "KEYCODE_HOME")
        time.sleep(1.5)
        return self._log("killed app + chooser (clean state)")

    def set(self, **settings):
        """Write settings into augustus.ini, preserving any other keys."""
        # read existing keys, then merge (native defaults the rest)
        existing = {}
        cur = self._sh(f"run-as {PKG} sh -c 'cat {INI_REMOTE} 2>/dev/null'")
        for line in cur.splitlines():
            if "=" in line:
                k, v = line.split("=", 1)
                existing[k.strip()] = v.strip()
        for alias, val in settings.items():
            key = SETTINGS.get(alias, alias)
            existing[key] = str(int(val))
        body = "".join(f"{k}={v}\n" for k, v in existing.items())
        with tempfile.NamedTemporaryFile("w", suffix=".ini", delete=False) as f:
            f.write(body)
            local = f.name
        try:
            self._adb("push", local, INI_STAGE, capture=True)
            self._adb("shell", f"cat {INI_STAGE} | run-as {PKG} sh -c 'cat > {INI_REMOTE}'")
        finally:
            os.unlink(local)
        return self._log(f"set {settings}")

    def clear_settings(self):
        """Remove augustus.ini so native uses defaults."""
        self._adb("shell", f"run-as {PKG} sh -c 'rm -f {INI_REMOTE}'")
        return self._log("cleared settings (defaults)")

    def open_settings(self):
        """Bring up the app's settings screen (HOME first to clear stray UI)."""
        self._adb("shell", "input", "keyevent", "KEYCODE_HOME")
        time.sleep(1)
        self._adb("shell", "am", "start", "-n", SETTINGS_ACTIVITY)
        time.sleep(3)
        return self._log("opened settings screen")

    def set_wallpaper(self):
        """Activate the wallpaper FROM the app's settings screen (never a raw preview)."""
        self.open_settings()
        if not self._tap(texts={"Set wallpaper", "SET WALLPAPER"}, pkg=PKG, tries=6):
            self.open_settings()
            if not self._tap(texts={"Set wallpaper", "SET WALLPAPER"}, pkg=PKG, tries=6):
                raise SystemExit("app 'Set wallpaper' button not found")
        time.sleep(3)
        ok = self._tap(rid_sub="set_wallpaper_button", tries=10) or \
            self._tap(texts={"Set wallpaper"}, pkg=CHOOSER_PKG, tries=6)
        time.sleep(2)
        for label in ("Home and lock screen", "Home screen", "Set on home screen"):
            if self._tap(texts={label}, tries=1, delay=0.5):
                break
        time.sleep(3)
        return self._log("set wallpaper via app settings")

    def hide(self):
        """Make the wallpaper hidden (open the settings app over it -> HIDE event)."""
        self._adb("shell", "am", "start", "-n", SETTINGS_ACTIVITY)
        time.sleep(3)
        return self._log("hidden (app in foreground)")

    def show(self):
        """Make the wallpaper visible (HOME -> onVisibilityChanged(true) -> UPDATE_CONFIGS)."""
        self._adb("shell", "input", "keyevent", "KEYCODE_HOME")
        time.sleep(4)
        return self._log("shown (home screen)")

    def lock(self):
        self._adb("shell", "input", "keyevent", "KEYCODE_SLEEP")
        time.sleep(2)
        return self._log("locked")

    def unlock(self):
        self._adb("shell", "input", "keyevent", "KEYCODE_WAKEUP")
        time.sleep(1)
        self._adb("shell", "input", "keyevent", "82")  # menu/unlock
        time.sleep(2)
        return self._log("unlocked")

    def next_poi(self, n=1):
        """Advance to the next point of interest via the debug ADB broadcast."""
        for _ in range(n):
            self._adb("shell", "am", "broadcast",
                      "-a", "com.github.Keriew.augustus.NEXT_POI", "-p", PKG)
            time.sleep(1)
        return self._log(f"next_poi x{n}")

    def wait(self, seconds):
        time.sleep(seconds)
        return self._log(f"waited {seconds}s")

    def logcat_clear(self):
        self._adb("logcat", "-c")
        return self._log("logcat cleared")

    def screenshot(self, name):
        path = os.path.join(self.outdir, f"{name}.png")
        with open(path, "wb") as f:
            f.write(subprocess.run(
                [self.adb_bin, "-s", self.serial, "exec-out", "screencap", "-p"],
                capture_output=True).stdout)
        return self._log(f"screenshot -> {path} ({os.path.getsize(path)} bytes)")

    def save_log(self, name, grep=None):
        pattern = grep or (r"SDL |Config key|C3 files|Savegame|recenter|"
                           r"onVisibilityChanged|nativePause|unable to|error")
        lines = [ln for ln in self._sh("logcat -d").splitlines()
                 if re.search(pattern, ln, re.I)
                 and "WPMS" not in ln and "WallpaperManagerService" not in ln]
        path = os.path.join(self.outdir, f"{name}.log")
        with open(path, "w") as f:
            f.write("\n".join(lines[-200:]))
        return self._log(f"log ({len(lines)} lines) -> {path}")

    # ---- assertions (raise on failure so a flow fails loudly) ----
    def assert_set(self):
        if SERVICE not in self._sh("dumpsys wallpaper"):
            raise AssertionError("wallpaper component is NOT the Augustus service")
        return self._log("assert_set OK (Augustus is the live wallpaper)")

    def recenter_count(self):
        return self._sh("logcat -d").count(RECENTER_MARK)

    def assert_recentered(self, at_least=1):
        n = self.recenter_count()
        if n < at_least:
            raise AssertionError(f"expected >= {at_least} recenters, saw {n}")
        return self._log(f"assert_recentered OK ({n} >= {at_least})")

    def poi_count(self):
        return self._sh("logcat -d").count(POI_MARK)

    def assert_poi_changed(self, at_least=1):
        n = self.poi_count()
        if n < at_least:
            raise AssertionError(f"expected >= {at_least} POI jumps, saw {n}")
        return self._log(f"assert_poi_changed OK ({n} >= {at_least})")

    def assert_not_recentered(self):
        n = self.recenter_count()
        if n != 0:
            raise AssertionError(f"expected no recenter, saw {n}")
        return self._log("assert_not_recentered OK (0)")

    def assert_paused(self):
        if PAUSE_MARK not in self._sh("logcat -d"):
            raise AssertionError("no nativePause() seen — wallpaper did not pause when hidden")
        return self._log("assert_paused OK (nativePause on hide)")

    # ---- uiautomator tap (by text/resource-id, coordinate-free) ----
    def _tap(self, texts=None, rid_sub=None, pkg=None, tries=8, delay=1.0):
        for _ in range(tries):
            out = self._sh("uiautomator dump /sdcard/ui.xml")
            xml = self._sh("cat /sdcard/ui.xml") if "dumped" in out or "hierchary" in out else ""
            for m in re.finditer(r"<node\b[^>]*?>", xml):
                a = dict(re.findall(r'([\w-]+)="([^"]*)"', m.group(0)))
                if pkg and a.get("package") != pkg:
                    continue
                if rid_sub is not None and rid_sub not in a.get("resource-id", ""):
                    continue
                if texts is not None and a.get("text") not in texts \
                        and a.get("content-desc") not in texts:
                    continue
                mm = re.match(r"\[(\d+),(\d+)\]\[(\d+),(\d+)\]", a.get("bounds", ""))
                if mm:
                    x1, y1, x2, y2 = map(int, mm.groups())
                    self._adb("shell", "input", "tap",
                              str((x1 + x2) // 2), str((y1 + y2) // 2))
                    return True
            time.sleep(delay)
        return False


# ===================== example flows (composed from atomic ops) =====================
# A flow is just a function that composes atomic ops. Add your own; that is the point.

def scenario_default(w):
    w.kill().clear_settings().set_wallpaper().assert_set().show().wait(4).screenshot("default")

def scenario_scale_dim(w):
    w.kill().set(scale=3, brightness=40).set_wallpaper().assert_set() \
        .show().wait(4).screenshot("scale3_dim")

def scenario_fast(w):
    w.kill().set(speed=150).set_wallpaper().assert_set().show().wait(4).screenshot("speed150")

def scenario_recenter_every_switch(w):
    w.kill().set(map=0).set_wallpaper().assert_set().show().logcat_clear() \
        .hide().show().wait(2).assert_recentered(1).save_log("recenter_every_switch")

def scenario_recenter_interval(w):
    w.kill().set(map=10).set_wallpaper().assert_set().show().logcat_clear() \
        .hide().show().wait(2).assert_not_recentered().save_log("recenter_interval")

def scenario_pause_when_hidden(w):
    w.kill().set_wallpaper().assert_set().show().logcat_clear() \
        .hide().wait(2).assert_paused().save_log("pause_when_hidden")

def scenario_poi_cycle(w):
    w.kill().set_wallpaper().assert_set().show().wait(2).logcat_clear() \
        .next_poi().screenshot("poi_1") \
        .next_poi().screenshot("poi_2") \
        .assert_poi_changed(2)

SCENARIOS = {
    "default": scenario_default,
    "scale_dim": scenario_scale_dim,
    "fast": scenario_fast,
    "recenter_every_switch": scenario_recenter_every_switch,
    "recenter_interval": scenario_recenter_interval,
    "pause_when_hidden": scenario_pause_when_hidden,
    "poi_cycle": scenario_poi_cycle,
}


def _cli():
    args = sys.argv[1:]
    if not args or args[0] in ("-h", "--help"):
        print(__doc__)
        return
    if args[0] == "list":
        print("scenarios:", ", ".join(SCENARIOS))
        return
    w = Wallpaper().boot()
    if args[0] == "run":
        names = list(SCENARIOS) if args[1:] == ["all"] else args[1:]
        for name in names:
            print(f"\n===== scenario: {name} =====")
            SCENARIOS[name](w)
            print(f"[PASS] {name}")
    elif args[0] == "op":
        # ad-hoc flow: op kill set:scale=2 set_wallpaper show screenshot:adhoc
        for token in args[1:]:
            name, _, rest = token.partition(":")
            method = getattr(w, name)
            if not rest:
                method()
            elif "=" in rest:
                kw = dict(p.split("=", 1) for p in rest.split(","))
                method(**{k: int(v) for k, v in kw.items()})
            else:
                method(int(rest) if rest.lstrip("-").isdigit() else rest)
        print("[done] ad-hoc flow")
    else:
        raise SystemExit(f"unknown command: {args[0]}")


if __name__ == "__main__":
    _cli()
