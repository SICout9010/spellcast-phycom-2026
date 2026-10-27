"""Moves and clicks the laptop's real mouse pointer, for the wand's mouse mode. Standard library only (ctypes).

macOS: allow the app running the server (e.g. Terminal) in System Settings > Privacy & Security > Accessibility,
or macOS silently ignores the moves and clicks.
"""
import ctypes
import ctypes.util
import sys

if sys.platform == "darwin":
    _cg = ctypes.cdll.LoadLibrary(ctypes.util.find_library("CoreGraphics")
                                  or "/System/Library/Frameworks/CoreGraphics.framework/CoreGraphics")
    _cf = ctypes.cdll.LoadLibrary(ctypes.util.find_library("CoreFoundation"))
    _ax = ctypes.cdll.LoadLibrary(ctypes.util.find_library("ApplicationServices"))

    class _Point(ctypes.Structure):
        _fields_ = [("x", ctypes.c_double), ("y", ctypes.c_double)]

    class _Size(ctypes.Structure):
        _fields_ = [("width", ctypes.c_double), ("height", ctypes.c_double)]

    class _Rect(ctypes.Structure):
        _fields_ = [("origin", _Point), ("size", _Size)]

    _cg.CGEventCreate.restype = ctypes.c_void_p
    _cg.CGEventCreate.argtypes = [ctypes.c_void_p]
    _cg.CGEventGetLocation.restype = _Point
    _cg.CGEventGetLocation.argtypes = [ctypes.c_void_p]
    _cg.CGEventCreateMouseEvent.restype = ctypes.c_void_p
    _cg.CGEventCreateMouseEvent.argtypes = [ctypes.c_void_p, ctypes.c_uint32, _Point, ctypes.c_uint32]
    _cg.CGEventSetIntegerValueField.argtypes = [ctypes.c_void_p, ctypes.c_uint32, ctypes.c_int64]
    _cg.CGEventPost.argtypes = [ctypes.c_uint32, ctypes.c_void_p]
    _cg.CGMainDisplayID.restype = ctypes.c_uint32
    _cg.CGDisplayBounds.restype = _Rect
    _cg.CGDisplayBounds.argtypes = [ctypes.c_uint32]
    _cf.CFRelease.argtypes = [ctypes.c_void_p]
    _ax.AXIsProcessTrusted.restype = ctypes.c_bool

    MOVED, LEFT_DOWN, LEFT_UP, RIGHT_DOWN, RIGHT_UP = 5, 1, 2, 3, 4  # Quartz event types
    CLICK_STATE = 1  # event field: 1 = single click, 2 = second click of a double click

    def position():
        e = _cg.CGEventCreate(None)
        p = _cg.CGEventGetLocation(e)
        _cf.CFRelease(e)
        return p.x, p.y

    def screen_size():
        r = _cg.CGDisplayBounds(_cg.CGMainDisplayID())
        return r.size.width, r.size.height

    def _post(kind, x, y, button=0, click_state=1):
        e = _cg.CGEventCreateMouseEvent(None, kind, _Point(x, y), button)
        _cg.CGEventSetIntegerValueField(e, CLICK_STATE, click_state)
        _cg.CGEventPost(0, e)  # 0 = as if from the hardware
        _cf.CFRelease(e)

    def _move_to(x, y):
        _post(MOVED, x, y)

    def _click(button, times):
        x, y = position()
        down, up = (RIGHT_DOWN, RIGHT_UP) if button == "right" else (LEFT_DOWN, LEFT_UP)
        for n in range(1, times + 1):
            _post(down, x, y, 1 if button == "right" else 0, n)
            _post(up, x, y, 1 if button == "right" else 0, n)

    def allowed():
        return _ax.AXIsProcessTrusted()

elif sys.platform == "win32":
    _u = ctypes.windll.user32

    class _P(ctypes.Structure):
        _fields_ = [("x", ctypes.c_long), ("y", ctypes.c_long)]

    def position():
        p = _P()
        _u.GetCursorPos(ctypes.byref(p))
        return p.x, p.y

    def screen_size():
        return _u.GetSystemMetrics(0), _u.GetSystemMetrics(1)

    def _move_to(x, y):
        _u.SetCursorPos(int(x), int(y))

    def _click(button, times):
        down, up = (0x08, 0x10) if button == "right" else (0x02, 0x04)  # MOUSEEVENTF_* flags
        for _ in range(times):
            _u.mouse_event(down, 0, 0, 0, 0)
            _u.mouse_event(up, 0, 0, 0, 0)

    def allowed():
        return True

else:
    def position():
        return 0, 0

    def screen_size():
        return 1, 1

    def _move_to(x, y):
        pass

    def _click(button, times):
        pass

    def allowed():
        print("mouse.py: mouse mode is set up for macOS and Windows only")
        return False


def move(dx, dy):
    """Move the pointer by (dx, dy) pixels, kept on the main screen."""
    x, y = position()
    w, h = screen_size()
    _move_to(min(max(x + dx, 0), w - 1), min(max(y + dy, 0), h - 1))


def click(kind):
    """kind: "left", "right" or "double" (a left double click)."""
    _click("right" if kind == "right" else "left", 2 if kind == "double" else 1)
