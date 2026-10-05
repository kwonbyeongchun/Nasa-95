"""SARibbon 제목 표시줄 위쪽에 Windows 크기 조절 영역을 보장한다."""
import sys

if sys.platform == "win32":
    import ctypes
    from ctypes import wintypes

    _user32 = ctypes.WinDLL("user32", use_last_error=True)
    _user32.GetWindowRect.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.RECT)]
    _user32.GetWindowRect.restype = wintypes.BOOL


def top_resize_hit(window, event_type, message):
    if sys.platform != "win32" or event_type != b"windows_generic_MSG":
        return None
    msg = wintypes.MSG.from_address(int(message))
    if msg.message != 0x0084:  # WM_NCHITTEST
        return None
    if window.isMaximized() or window.isFullScreen() or window.minimumHeight() >= window.maximumHeight():
        return None
    rect = wintypes.RECT()
    if not _user32.GetWindowRect(msg.hWnd, ctypes.byref(rect)):
        return None
    # lParam은 부호 있는 화면 물리 좌표다. 왼쪽/위쪽 보조 모니터도 처리한다.
    x = ctypes.c_short(msg.lParam & 0xFFFF).value
    y = ctypes.c_short((msg.lParam >> 16) & 0xFFFF).value
    border = max(1, round(6 * window.devicePixelRatioF()))
    if not (rect.left <= x < rect.right and rect.top <= y < rect.top + border):
        return None
    if window.minimumWidth() < window.maximumWidth():
        if x < rect.left + border:
            return 13  # HTTOPLEFT
        if x >= rect.right - border:
            return 14  # HTTOPRIGHT
    return 12  # HTTOP: OS가 커서와 세로 크기 조절을 처리한다.
