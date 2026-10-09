#!/usr/bin/env python3
"""Run focused app checks on an owned silent private display."""

import argparse
import ctypes
import hashlib
import json
import os
from pathlib import Path
import re
import signal
import shlex
import stat
import subprocess
import sys
import tempfile
import time

from PIL import Image
from private_x11 import PrivateX11, wait_until


class XImage(ctypes.Structure):
    _fields_ = [
        ("width", ctypes.c_int),
        ("height", ctypes.c_int),
        ("xoffset", ctypes.c_int),
        ("format", ctypes.c_int),
        ("data", ctypes.c_void_p),
        ("byte_order", ctypes.c_int),
        ("bitmap_unit", ctypes.c_int),
        ("bitmap_bit_order", ctypes.c_int),
        ("bitmap_pad", ctypes.c_int),
        ("depth", ctypes.c_int),
        ("bytes_per_line", ctypes.c_int),
        ("bits_per_pixel", ctypes.c_int),
        ("red_mask", ctypes.c_ulong),
        ("green_mask", ctypes.c_ulong),
        ("blue_mask", ctypes.c_ulong),
        ("obdata", ctypes.c_void_p),
    ]


class XError(ctypes.Structure):
    _fields_ = [
        ("type", ctypes.c_int),
        ("display", ctypes.c_void_p),
        ("resource", ctypes.c_ulong),
        ("serial", ctypes.c_ulong),
        ("code", ctypes.c_ubyte),
        ("request", ctypes.c_ubyte),
        ("minor", ctypes.c_ubyte),
    ]


# X11 errors tolerated while SDL replaces its window: BadWindow and BadDrawable.
TRANSIENT_X_ERRORS = (3, 9)


def stop_group(process):
    if process is None:
        return
    try:
        os.killpg(process.pid, signal.SIGTERM)
    except ProcessLookupError:
        pass
    try:
        process.wait(timeout=5)
    except subprocess.TimeoutExpired:
        os.killpg(process.pid, signal.SIGKILL)
        process.wait(timeout=5)


def clean_log(path):
    text = path.read_text(errors="replace")
    if any(value in text for value in ("Sanitizer:", "runtime error:", "AddressSanitizer", "FAIL ", "moorhuhn:")):
        raise RuntimeError(f"Failure or sanitizer diagnostic in {path}")
    return text


class OwnedDisplay:
    def __init__(self, number, environment, output):
        self.number = number
        self.name = f":{number}"
        self.socket = Path(f"/tmp/.X11-unix/X{number}")
        self.lock = Path(f"/tmp/.X{number}-lock")
        self.output = output
        self.process = None
        self.window_manager = None
        self.window_manager_log = None
        self.x11 = None
        self.log = None
        self.environment = environment
        self.x_errors = []

    def verify(self):
        if self.process is None or self.process.poll() is not None:
            raise RuntimeError("Owned Xvfb process is not running")
        if not self.lock.exists() or self.lock.read_text().strip() != str(self.process.pid):
            raise RuntimeError("Private display lock PID does not match the spawned Xvfb")
        if not self.socket.exists() or not stat.S_ISSOCK(self.socket.lstat().st_mode):
            raise RuntimeError("Private display socket is missing or not a socket")
        if self.window_manager is not None and self.window_manager.poll() is not None:
            raise RuntimeError("Private window manager stopped")

    def __enter__(self):
        if os.path.lexists(self.socket) or os.path.lexists(self.lock):
            raise RuntimeError("Private display already exists; refusing connection")
        self.log = (self.output / "xvfb.log").open("w")
        self.process = subprocess.Popen(
            ["Xvfb", self.name, "-screen", "0", "640x480x24", "-extension", "GLX", "-nolisten", "tcp"],
            env=self.environment,
            stdout=self.log,
            stderr=subprocess.STDOUT,
            start_new_session=True,
        )
        try:
            wait_until(lambda: self.socket.exists() and self.lock.exists(), "Private Xvfb startup", self.process)
            self.verify()  # Verify PID ownership before the first XOpenDisplay.
            self.environment.update(DISPLAY=self.name, SDL_VIDEODRIVER="x11", SDL_VIDEO_DRIVER="x11")
            self.x11 = PrivateX11(self.name)
            p, w = ctypes.c_void_p, ctypes.c_ulong
            # SDL may replace its native XID while creating the renderer. Keep
            # transient BadWindow/BadDrawable errors from bypassing finally cleanup.
            handler_type = ctypes.CFUNCTYPE(ctypes.c_int, p, ctypes.POINTER(XError))

            def handle_error(_, event):
                self.x_errors.append((event.contents.code, event.contents.request))
                return 0

            self.error_handler = handler_type(handle_error)
            self.x11.x.XSetErrorHandler.argtypes = [handler_type]
            self.x11.x.XSetErrorHandler.restype = p
            self.x11.x.XSetErrorHandler(self.error_handler)
            self.x11.x.XGetGeometry.argtypes = [
                p,
                w,
                ctypes.POINTER(w),
                ctypes.POINTER(ctypes.c_int),
                ctypes.POINTER(ctypes.c_int),
                ctypes.POINTER(ctypes.c_uint),
                ctypes.POINTER(ctypes.c_uint),
                ctypes.POINTER(ctypes.c_uint),
                ctypes.POINTER(ctypes.c_uint),
            ]
            self.x11.x.XGetImage.argtypes = [
                p,
                w,
                ctypes.c_int,
                ctypes.c_int,
                ctypes.c_uint,
                ctypes.c_uint,
                w,
                ctypes.c_int,
            ]
            self.x11.x.XGetImage.restype = ctypes.POINTER(XImage)
            self.x11.x.XDestroyImage.argtypes = [ctypes.POINTER(XImage)]
            self.x11.x.XSync.argtypes = [p, ctypes.c_int]
            self.window_manager_log = (self.output / "openbox.log").open("w")
            self.window_manager = subprocess.Popen(
                ["openbox", "--sm-disable"], env=self.environment,
                stdout=self.window_manager_log, stderr=subprocess.STDOUT,
                start_new_session=True,
            )

            def ready():
                result = subprocess.run(
                    ["xprop", "-root", "_NET_SUPPORTING_WM_CHECK"], env=self.environment,
                    capture_output=True, text=True, timeout=2,
                )
                return result.returncode == 0 and "window id #" in result.stdout

            wait_until(ready, "Private window manager startup", self.window_manager)
            return self
        except BaseException:
            self.__exit__(None, None, None)
            raise

    def __exit__(self, *_):
        if self.x11 is not None:
            self.x11.close()
        stop_group(self.window_manager)
        stop_group(self.process)
        if self.window_manager_log is not None:
            self.window_manager_log.close()
        if self.log is not None:
            self.log.close()

    def key(self, name, duration=0.16):
        self.verify()
        x = self.x11
        x.focus(self.current_window())
        code = x.x.XKeysymToKeycode(x.display, x.x.XStringToKeysym(name.encode()))
        if not code:
            raise RuntimeError("Unknown private test key")
        x.xt.XTestFakeKeyEvent(x.display, code, 1, 0)
        x.x.XFlush(x.display)
        time.sleep(duration)
        x.xt.XTestFakeKeyEvent(x.display, code, 0, 0)
        x.x.XFlush(x.display)
        time.sleep(0.06)

    # Input and capture always target the current window because SDL may replace its XID.
    def click(self, button=1):
        window = self.current_window()
        self.x11.move(window, 320, 240)
        self.x11.xt.XTestFakeButtonEvent(self.x11.display, button, 1, 0)
        self.x11.x.XFlush(self.x11.display)
        time.sleep(0.12)
        self.x11.xt.XTestFakeButtonEvent(self.x11.display, button, 0, 0)
        self.x11.x.XFlush(self.x11.display)
        time.sleep(0.08)

    def capture(self, path=None):
        x = self.x11
        window = self.current_window()
        root, left, top = ctypes.c_ulong(), ctypes.c_int(), ctypes.c_int()
        width, height, border, depth = (ctypes.c_uint() for _ in range(4))
        if not x.x.XGetGeometry(
            x.display,
            window,
            ctypes.byref(root),
            ctypes.byref(left),
            ctypes.byref(top),
            ctypes.byref(width),
            ctypes.byref(height),
            ctypes.byref(border),
            ctypes.byref(depth),
        ):
            raise RuntimeError("Cannot read owned window geometry")
        if (width.value, height.value) != (640, 480):
            raise RuntimeError("Expected unscaled 640x480 private game window")
        x.x.XSync(x.display, 0)
        raw = x.x.XGetImage(x.display, window, 0, 0, width.value, height.value, ctypes.c_ulong(-1).value, 2)
        if not raw:
            raise RuntimeError("Cannot capture owned private window")
        try:
            item = raw.contents
            if (item.bits_per_pixel, item.byte_order, item.red_mask, item.green_mask, item.blue_mask) != (
                32,
                0,
                0xFF0000,
                0xFF00,
                0xFF,
            ):
                raise RuntimeError("Unexpected private Xvfb pixel layout")
            data = ctypes.string_at(item.data, item.bytes_per_line * item.height)
            image = Image.frombytes("RGB", (item.width, item.height), data, "raw", "BGRX", item.bytes_per_line)
        finally:
            x.x.XDestroyImage(raw)
        if path:
            image.save(path)
        return image

    def current_window(self):
        self.verify()
        window = wait_until(self.x11.find, "Current owned SDL window", timeout=5)
        if any(code not in TRANSIENT_X_ERRORS for code, _ in self.x_errors):
            raise RuntimeError(f"Unexpected private X11 error: {self.x_errors}")
        return window


def validate_credits(manifest, screenshot):
    values = (manifest.parent / "tables.txt").read_text().split()
    offset = values.index("moorhuhn.pal") + 1
    palette = [int(value) for value in values[offset:offset + 1024]]
    with Image.open(manifest.parent / "indices/credits.png") as source:
        indices = source.convert("RGB").getchannel("R")
    expected = Image.frombytes("P", indices.size, indices.tobytes())
    expected.putpalette([value for i, value in enumerate(palette) if i % 4 != 3])
    with Image.open(screenshot) as captured:
        measured = captured.convert("RGB")
    if measured.size != (640, 480) or measured.tobytes() != expected.convert("RGB").tobytes():
        raise RuntimeError("Native credits differ from the source pixels")
    return {"pixels": 640 * 480, "rgba_sha256": hashlib.sha256(measured.convert("RGBA").tobytes()).hexdigest()}


def play(repository, build, owned, environment, output, temporary, scenario):
    log = output / "game.log"
    process = None
    screenshots = []

    def capture(name):
        screenshots.append(name)
        return owned.capture(output / name)

    tokens = (repository / "assets/tables.txt").read_text().split()
    offset = tokens.index("moorhuhn.pal") + 1
    palette = [int(value) for value in tokens[offset:offset + 1024]]

    def source(name):
        with Image.open(repository / f"assets/indices/{name}.png") as original:
            indices = original.convert("RGB").getchannel("R")
        image = Image.frombytes("P", indices.size, indices.tobytes())
        image.putpalette([value for i, value in enumerate(palette) if i % 4 != 3])
        return indices, image.convert("RGB")

    main_indices, _ = source("main")
    _, score_image = source("score")
    _, credits_image = source("credits")
    banner_indices, banner_image = source("mschiess")
    banner_samples = [(x + 140, y + 357, banner_image.getpixel((x, y)))
                      for y in range(0, banner_image.height, 12)
                      for x in range(0, banner_image.width, 12)
                      if banner_indices.getpixel((x, y)) != 18]
    with Image.open(repository / "assets/indices/ClutTrns.png") as clut_image:
        clut = clut_image.convert("RGB").getchannel("R").tobytes()
    panel_samples = []
    for x, y in ((182, 212), (458, 212), (182, 268), (458, 268)):
        color = clut[main_indices.getpixel((x, y))]
        panel_samples.append((x, y, tuple(palette[color * 4:color * 4 + 3])))
    score_samples = [(x, y, score_image.getpixel((x, y))) for x, y in ((10, 200), (630, 200), (10, 400), (630, 400))]

    def matches(image, samples):
        return all(image.getpixel((x, y)) == color for x, y, color in samples)

    def wait_screen(check, message):
        wait_until(lambda: check(owned.capture()), message, process, timeout=30)

    def wait_title():
        wait_screen(lambda image: matches(image, banner_samples), "Title screen")

    def wait_name():
        wait_screen(lambda image: matches(image, panel_samples), "Name entry")

    def wait_gameplay():
        def sky(image):
            red, green, blue = image.crop((0, 0, 640, 100)).resize((1, 1)).getpixel((0, 0))
            return (red + green + blue) / 3 > 90 and blue > red * 0.8
        wait_screen(sky, "Gameplay sky")

    def wait_scores():
        wait_screen(lambda image: matches(image, score_samples), "Highscore screen")

    def wait_credits():
        wait_screen(lambda image: image.tobytes() == credits_image.tobytes(), "Credits screen")

    with log.open("w") as stream:
        try:
            process = subprocess.Popen(
                [str(build / "moorhuhn")],
                cwd=temporary, env=environment, stdout=stream, stderr=subprocess.STDOUT,
                start_new_session=True,
            )
            window = wait_until(owned.x11.find, "Real game window", process)
            owned.x11.focus(window)
            wait_title()
            title = capture("title.png")
            if len(title.getcolors(640 * 480) or []) < 10:
                raise RuntimeError("Title screen is empty")
            if scenario == "startup":
                owned.key("Escape")
                wait_credits()
                owned.key("space")
                if process.wait(timeout=30) != 0:
                    raise RuntimeError("Game did not exit cleanly")
                rounds = 0
            else:
                owned.key("space")
                wait_name()
                capture("name.png")
                for letter in "native":
                    owned.key(letter, 0.06)
                owned.key("Return")
                wait_gameplay()
                game = capture("round-start.png")
                if game.tobytes() == title.tobytes():
                    raise RuntimeError("Space and name confirmation did not start a round")
                for _ in range(9):
                    owned.click()
                capture("round-empty.png")
                owned.click(3)
                owned.key("Right", 0.3)
                capture("round-camera.png")
                owned.key("Escape")
                wait_scores()
                capture("results.png")
                owned.key("space")
                wait_title()
                rounds = 1
                if scenario == "flow":
                    owned.key("space")
                    wait_name()
                    owned.key("Return")
                    wait_gameplay()
                    owned.click()
                    capture("round-restart.png")
                    owned.key("Escape")
                    wait_scores()
                    owned.key("space")
                    wait_title()
                    rounds = 2
                owned.key("Escape")
                wait_credits()
                capture("credits.png")
                owned.key("space")
                if process.wait(timeout=30) != 0:
                    raise RuntimeError("Game did not exit cleanly")
                if owned.x11.find() is not None:
                    raise RuntimeError("Game left a window after shutdown")
        finally:
            stop_group(process)
    clean_log(log)
    result = {"rounds": rounds, "screenshots": screenshots}
    if rounds:
        saves = list(Path(environment["XDG_DATA_HOME"]).rglob("highscores.txt"))
        if len(saves) != 1:
            raise RuntimeError("Expected one isolated high-score save")
        rows = saves[0].read_text().splitlines()
        if len(rows) != 7 or rows[0] != "MHSCORES 1" or shlex.split(rows[1]) != ["native"]:
            raise RuntimeError("Typed name did not survive persistence")
        scores = [int(shlex.split(row)[1]) for row in rows[2:]]
        if scores != sorted(scores, reverse=True):
            raise RuntimeError("High scores are not sorted")
        (output / "highscores.txt").write_text(saves[0].read_text())
    print(f"pass: {scenario}, fullscreen input and clean shutdown", flush=True)
    return result


def isolated_environment(source, temporary):
    environment = source.copy()
    for name in ("DISPLAY", "WAYLAND_DISPLAY", "XAUTHORITY", "PULSE_SERVER", "PIPEWIRE_REMOTE"):
        environment.pop(name, None)
    private = Path(temporary)
    environment.update(
        SDL_AUDIODRIVER="dummy",
        SDL_AUDIO_DRIVER="dummy",
        SDL_RENDER_DRIVER="software",
        # Without this SDL backs the software window with Vulkan, which crashes in GPU drivers on GLX-less Xvfb.
        SDL_FRAMEBUFFER_ACCELERATION="0",
        XDG_RUNTIME_DIR=str(private),
        XDG_DATA_HOME=str(private / "data"),
        XDG_CONFIG_HOME=str(private / "config"),
        XDG_CACHE_HOME=str(private / "cache"),
    )
    return environment


def main(argv=None):
    parser = argparse.ArgumentParser()
    parser.add_argument("--build-dir", type=Path, default=Path("build/dev"))
    parser.add_argument("--manifest", type=Path, default=Path("assets/manifest.txt"))
    parser.add_argument("--display", default=":139")
    parser.add_argument("--output-dir", type=Path, help="Default: build/checks/round-native/SCENARIO")
    parser.add_argument(
        "--scenario",
        choices=("startup", "input", "flow"),
        default="flow",
        help="startup: title and shutdown; input: one round; flow: two rounds and restart (default)",
    )
    args = parser.parse_args(argv)
    match = re.fullmatch(r":([0-9]+)", args.display)
    if not match or int(match.group(1)) < 100:
        parser.error("Use a private display :N with N >=100")
    number = int(match.group(1))
    if os.path.lexists(f"/tmp/.X11-unix/X{number}") or os.path.lexists(f"/tmp/.X{number}-lock"):
        parser.error("Private display already exists; choose an unused display")
    repository = Path(__file__).resolve().parents[2]
    build, manifest, output = (
        (repository / path).resolve()
        for path in (
            args.build_dir,
            args.manifest,
            args.output_dir or Path("build/checks/round-native") / args.scenario,
        )
    )
    if not all((build / name).is_file() for name in ("moorhuhn", "images.pak", "audio.pak")) or not manifest.is_file():
        parser.error("Build the app and both asset packages first; provide an asset manifest")
    output.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="moorhuhn-round-") as temporary:
        environment = isolated_environment(os.environ, temporary)
        with OwnedDisplay(number, environment, output) as owned:
            # Refuse occupied socket/lock before any X11 connection in a second process.
            with (output / "occupied-display.log").open("w") as stream:
                refusal = subprocess.Popen(
                    [sys.executable, str(Path(__file__).resolve()), "--display", args.display],
                    cwd=temporary,
                    env=environment,
                    stdout=stream,
                    stderr=subprocess.STDOUT,
                    start_new_session=True,
                )
                try:
                    if refusal.wait(timeout=5) != 2:
                        raise RuntimeError("Occupied display was not refused")
                finally:
                    stop_group(refusal)
            if "Private display already exists" not in (output / "occupied-display.log").read_text():
                raise RuntimeError("Missing occupied-display refusal evidence")
            prior = play(repository, build, owned, environment, output, temporary, args.scenario)
            if args.scenario != "startup":
                prior["credits"] = validate_credits(manifest, output / "credits.png")
            screenshots = prior.pop("screenshots")
            (output / f"{args.scenario}-result.json").write_text(json.dumps(prior, indent=2) + "\n")
            result = {
                "scenario": args.scenario,
                "display": args.display,
                "xvfb_pid": owned.process.pid,
                "audio": "silent dummy playback",
                "game": prior,
            }
            result["screenshots"] = {
                name: hashlib.sha256((output / name).read_bytes()).hexdigest() for name in screenshots
            }
            (output / "summary.json").write_text(json.dumps(result, indent=2) + "\n")
    if os.path.lexists(f"/tmp/.X11-unix/X{number}") or os.path.lexists(f"/tmp/.X{number}-lock"):
        raise RuntimeError("Owned Xvfb left a socket or lock after shutdown")
    print("pass: private display PID guard, occupied refusal, private data, process/window cleanup", flush=True)


if __name__ == "__main__":
    try:
        main()
    except (RuntimeError, subprocess.TimeoutExpired) as error:
        print(f"FAIL {error}", file=sys.stderr)
        sys.exit(1)
