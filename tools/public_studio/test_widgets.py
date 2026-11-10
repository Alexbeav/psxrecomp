"""Withdrawn native widget checks. No Tk window is mapped or focused."""
import ctypes
import json
import os
from pathlib import Path
import tempfile
try:
    import tkinter as tk
except ImportError:
    print("SKIP: optional tkinter is unavailable")
    raise SystemExit(77)
from unittest import mock

import studio


def main():
    if os.name != "nt":
        print("SKIP: Windows native widget check")
        return
    foreground = ctypes.windll.user32.GetForegroundWindow
    foreground.restype = ctypes.c_void_p
    before = foreground()
    studio.configure_runtime()
    root = tk.Tk()
    # Tk maps a newly created root during its idle processing. Withdraw it
    # immediately, before the first event/idle update, and never deiconify it.
    root.withdraw()
    mapped = []
    root.bind("<Map>", lambda event: mapped.append({"widget": str(event.widget),
        "root_mapped": root.winfo_ismapped(), "root_state": root.state()}))
    with tempfile.TemporaryDirectory() as tmp:
        data = Path(tmp) / "public-data"
        with mock.patch("urllib.request.urlopen", side_effect=AssertionError("network before Build")), \
             mock.patch("subprocess.Popen", side_effect=AssertionError("process before Build")):
            app = studio.Studio(root, data)
            try:
                root.update()
                assert not root.winfo_ismapped(), "Root became visible"
                assert not any(event["root_mapped"] or event["root_state"] != "withdrawn" for event in mapped), mapped
                assert not data.exists(), "Startup created data/cache"
                assert not app.ready
                app.show_settings()
                root.update()
                assert not root.winfo_ismapped()
                assert not any(event["root_mapped"] or event["root_state"] != "withdrawn" for event in mapped), mapped
                assert not data.exists(), "Opening Settings created data/cache"
                assert len(app.bios_list.get(0, "end")) == 0
                assert app.setting_vars["portable"].get() is True
                assert app.setting_vars["skip_bios"].get() is False
                assert app.setting_vars["pgo"].get() is False
                assert app.setting_vars["diagnostics"].get() is False
                app.setting_vars["output_folder"].set(str(Path(tmp) / "Games"))
                app.save_settings()
                root.update()
                assert data.joinpath("settings.json").is_file()
                assert not data.joinpath("cache").exists()
                assert not root.winfo_ismapped()
                assert not any(event["root_mapped"] or event["root_state"] != "withdrawn" for event in mapped), mapped
                print(json.dumps({"native_widget_workflows": 1, "assertions": 16, "mapped_events": mapped,
                                  "root_mapped": root.winfo_ismapped(), "cache_created": False,
                                  "foreground_unchanged": before == foreground(),
                                  "tk_version": str(tk.TkVersion)}))
            finally:
                app.remove_drop()
                root.destroy()


if __name__ == "__main__":
    main()
