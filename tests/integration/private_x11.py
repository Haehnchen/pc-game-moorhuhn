"""Input and wait helpers for owned private X11 displays."""

import ctypes
import time


def wait_until(check, message, process=None, timeout=20):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        value = check()
        if value:
            return value
        if process is not None and process.poll() is not None:
            raise RuntimeError(message + ": process exited")
        time.sleep(0.02)
    raise RuntimeError(message + ": timeout")


class PrivateX11:
    def __init__(self, name):
        self.x = ctypes.CDLL("libX11.so.6")
        self.xt = ctypes.CDLL("libXtst.so.6")
        p, w = ctypes.c_void_p, ctypes.c_ulong
        self.x.XOpenDisplay.argtypes, self.x.XOpenDisplay.restype = [ctypes.c_char_p], p
        self.x.XDefaultRootWindow.argtypes, self.x.XDefaultRootWindow.restype = [p], w
        self.x.XQueryTree.argtypes = [
            p,
            w,
            ctypes.POINTER(w),
            ctypes.POINTER(w),
            ctypes.POINTER(ctypes.POINTER(w)),
            ctypes.POINTER(ctypes.c_uint),
        ]
        self.x.XFetchName.argtypes = [p, w, ctypes.POINTER(ctypes.c_char_p)]
        self.x.XFree.argtypes = [p]
        self.x.XSetInputFocus.argtypes = [p, w, ctypes.c_int, w]
        self.x.XRaiseWindow.argtypes = [p, w]
        self.x.XTranslateCoordinates.argtypes = [
            p,
            w,
            w,
            ctypes.c_int,
            ctypes.c_int,
            ctypes.POINTER(ctypes.c_int),
            ctypes.POINTER(ctypes.c_int),
            ctypes.POINTER(w),
        ]
        self.x.XStringToKeysym.argtypes, self.x.XStringToKeysym.restype = [ctypes.c_char_p], w
        self.x.XKeysymToKeycode.argtypes, self.x.XKeysymToKeycode.restype = [p, w], ctypes.c_uint
        self.x.XFlush.argtypes = [p]
        self.x.XCloseDisplay.argtypes = [p]
        self.xt.XTestFakeMotionEvent.argtypes = [p, ctypes.c_int, ctypes.c_int, ctypes.c_int, w]
        self.xt.XTestFakeButtonEvent.argtypes = [p, ctypes.c_uint, ctypes.c_int, w]
        self.xt.XTestFakeKeyEvent.argtypes = [p, ctypes.c_uint, ctypes.c_int, w]
        self.display = self.x.XOpenDisplay(name.encode())
        if not self.display:
            raise RuntimeError("Cannot open owned private display")
        self.root = self.x.XDefaultRootWindow(self.display)

    def find(self, parent=None):
        parent = self.root if parent is None else parent
        name = ctypes.c_char_p()
        if self.x.XFetchName(self.display, parent, ctypes.byref(name)) and name.value:
            matches = name.value == b"Moorhuhn"
            self.x.XFree(name)
            if matches:
                return parent
        root, owner = ctypes.c_ulong(), ctypes.c_ulong()
        children = ctypes.POINTER(ctypes.c_ulong)()
        count = ctypes.c_uint()
        if self.x.XQueryTree(
            self.display, parent, ctypes.byref(root), ctypes.byref(owner), ctypes.byref(children), ctypes.byref(count)
        ):
            entries = [children[n] for n in range(count.value)]
            if children:
                self.x.XFree(children)
            for child in entries:
                found = self.find(child)
                if found:
                    return found
        return None

    def focus(self, window):
        self.x.XRaiseWindow(self.display, window)
        self.x.XSetInputFocus(self.display, window, 1, 0)
        self.x.XFlush(self.display)

    def move(self, window, x, y):
        root_x, root_y, child = ctypes.c_int(), ctypes.c_int(), ctypes.c_ulong()
        if not self.x.XTranslateCoordinates(
            self.display, window, self.root, x, y, ctypes.byref(root_x), ctypes.byref(root_y), ctypes.byref(child)
        ):
            raise RuntimeError("Cannot translate owned window coordinates")
        self.xt.XTestFakeMotionEvent(self.display, -1, root_x.value, root_y.value, 0)
        self.x.XFlush(self.display)

    def close(self):
        if self.display:
            self.x.XCloseDisplay(self.display)
            self.display = None
