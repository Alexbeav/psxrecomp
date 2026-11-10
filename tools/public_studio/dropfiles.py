"""Windows file drops using the platform API; no Tk extension is required."""
import ctypes
import os


def accept_files(window, callback):
    if os.name != "nt":
        return lambda: None
    from ctypes import wintypes
    user = ctypes.WinDLL("user32", use_last_error=True)
    shell = ctypes.WinDLL("shell32", use_last_error=True)
    result_type = ctypes.c_ssize_t
    proc_type = ctypes.WINFUNCTYPE(result_type, wintypes.HWND, wintypes.UINT, wintypes.WPARAM, wintypes.LPARAM)
    user.GetAncestor.argtypes = [wintypes.HWND, wintypes.UINT]
    user.GetAncestor.restype = wintypes.HWND
    setter = user.SetWindowLongPtrW if ctypes.sizeof(ctypes.c_void_p) == 8 else user.SetWindowLongW
    setter.argtypes = [wintypes.HWND, ctypes.c_int, ctypes.c_void_p]
    setter.restype = ctypes.c_void_p
    user.CallWindowProcW.argtypes = [ctypes.c_void_p, wintypes.HWND, wintypes.UINT, wintypes.WPARAM, wintypes.LPARAM]
    user.CallWindowProcW.restype = result_type
    shell.DragAcceptFiles.argtypes = [wintypes.HWND, wintypes.BOOL]
    shell.DragQueryFileW.argtypes = [wintypes.HANDLE, wintypes.UINT, wintypes.LPWSTR, wintypes.UINT]
    shell.DragQueryFileW.restype = wintypes.UINT
    shell.DragFinish.argtypes = [wintypes.HANDLE]
    window.update_idletasks()
    hwnd = user.GetAncestor(window.winfo_id(), 2)
    previous = None
    @proc_type
    def handle(hwnd, message, wparam, lparam):
        if message == 0x0233:  # WM_DROPFILES
            paths = []
            try:
                count = shell.DragQueryFileW(wparam, 0xffffffff, None, 0)
                for index in range(count):
                    length = shell.DragQueryFileW(wparam, index, None, 0)
                    value = ctypes.create_unicode_buffer(length + 1)
                    shell.DragQueryFileW(wparam, index, value, length + 1)
                    paths.append(value.value)
            finally:
                shell.DragFinish(wparam)
            window.after_idle(lambda: callback(paths))
            return 0
        return user.CallWindowProcW(previous, hwnd, message, wparam, lparam)
    previous = setter(hwnd, -4, ctypes.cast(handle, ctypes.c_void_p))
    if not previous:
        raise ctypes.WinError(ctypes.get_last_error())
    shell.DragAcceptFiles(hwnd, True)
    def close():
        # Keep handle alive until the original procedure is restored.
        shell.DragAcceptFiles(hwnd, False)
        setter(hwnd, -4, previous)
        _ = handle
    return close
