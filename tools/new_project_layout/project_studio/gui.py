"""CustomTkinter GUI for Project Studio.

CLI audit/plan/apply stay stdlib-only. The GUI auto-bootstraps a local
``.venv`` and installs ``requirements-gui.txt`` on first launch.
"""

from __future__ import annotations

import hashlib
import os
import shutil
import subprocess
import sys
import tkinter as tk
from pathlib import Path
from tkinter import filedialog, messagebox

from .detect import audit_project
from .models import CheckStatus, MigrateOptions
from .ops import apply_plan
from .plan import build_plan

_STATUS_COLORS = {
    CheckStatus.PASS: "#3dd68c",
    CheckStatus.FAIL: "#f07178",
    CheckStatus.WARN: "#e6b450",
    CheckStatus.SKIP: "#8a9199",
}

_TOOLKIT = Path(__file__).resolve().parent.parent
_VENV_DIR = _TOOLKIT / ".venv"
_REQS = _TOOLKIT / "requirements-gui.txt"
_STAMP = _VENV_DIR / ".project_studio_gui_deps"
_BOOTSTRAP_ENV = "PROJECT_STUDIO_GUI_BOOTSTRAPPED"


def _pick_directory(*, title: str, parent=None, initialdir: str | None = None) -> str:
    """Native directory picker when available; Tk fallback otherwise."""
    start = initialdir or os.path.expanduser("~")
    if sys.platform == "darwin":
        picked = _macos_pick(directory=True, title=title, initial=start)
        if picked is not None:
            return picked
    elif sys.platform != "win32":
        picked = _linux_pick(directory=True, title=title, initial=start, filetypes=None)
        if picked is not None:
            return picked
    # Windows (and fallbacks): Tk uses the OS common dialog.
    return filedialog.askdirectory(parent=parent, title=title, initialdir=start) or ""


def _pick_open_file(
    *,
    title: str,
    parent=None,
    initialdir: str | None = None,
    filetypes: list[tuple[str, str]] | None = None,
) -> str:
    """Native open-file picker when available; Tk fallback otherwise."""
    start = initialdir or os.path.expanduser("~")
    ftypes = filetypes or [("All", "*.*")]
    if sys.platform == "darwin":
        picked = _macos_pick(
            directory=False, title=title, initial=start, filetypes=ftypes
        )
        if picked is not None:
            return picked
    elif sys.platform != "win32":
        picked = _linux_pick(
            directory=False, title=title, initial=start, filetypes=ftypes
        )
        if picked is not None:
            return picked
    return (
        filedialog.askopenfilename(
            parent=parent, title=title, initialdir=start, filetypes=ftypes
        )
        or ""
    )


def _linux_pick(
    *,
    directory: bool,
    title: str,
    initial: str,
    filetypes: list[tuple[str, str]] | None,
) -> str | None:
    """Prefer Zenity/KDialog (real desktop dialogs). None → caller should fall back."""
    desk = (
        os.environ.get("XDG_CURRENT_DESKTOP")
        or os.environ.get("DESKTOP_SESSION")
        or ""
    ).lower()
    prefer_kde = "kde" in desk or "plasma" in desk
    order = ("kdialog", "zenity") if prefer_kde else ("zenity", "kdialog")

    for tool in order:
        if not shutil.which(tool):
            continue
        try:
            if tool == "zenity":
                cmd = [
                    "zenity",
                    "--file-selection",
                    f"--title={title}",
                    f"--filename={initial.rstrip('/')}/",
                ]
                if directory:
                    cmd.append("--directory")
                else:
                    for label, pattern in filetypes or []:
                        cmd.append(f"--file-filter={label} | {pattern}")
            else:
                cmd = ["kdialog", "--title", title]
                if directory:
                    cmd.extend(["--getexistingdirectory", initial])
                else:
                    # kdialog filter: "Cue sheet (*.cue)|*.cue\nAll (*)|*"
                    filt = "\n".join(
                        f"{label} ({pattern})|{pattern}"
                        for label, pattern in (filetypes or [("All", "*")])
                    )
                    cmd.extend(["--getopenfilename", initial, filt])
            r = subprocess.run(cmd, capture_output=True, text=True, check=False)
        except OSError:
            continue
        if r.returncode == 0:
            return (r.stdout or "").strip()
        if r.returncode in (1, 5):  # cancel / no input
            return ""
        # Other codes: try next tool / fall back
    return None


def _macos_pick(
    *,
    directory: bool,
    title: str,
    initial: str,
    filetypes: list[tuple[str, str]] | None = None,
) -> str | None:
    """NSOpenPanel via osascript. None → Tk fallback."""
    if not shutil.which("osascript"):
        return None
    # Escape for AppleScript strings
    def esc(s: str) -> str:
        return s.replace("\\", "\\\\").replace('"', '\\"')

    choose = "choose folder" if directory else "choose file"
    props = [f'with prompt "{esc(title)}"']
    if initial and Path(initial).is_dir():
        props.append(f'default location POSIX file "{esc(initial)}"')
    if not directory and filetypes:
        exts: list[str] = []
        for _, pattern in filetypes:
            for part in pattern.replace(";", " ").split():
                part = part.strip()
                if part.startswith("*.") and part != "*.*":
                    exts.append(part[2:])
        if exts:
            listed = ", ".join(f'"{e}"' for e in dict.fromkeys(exts))
            props.append(f"of type {{{listed}}}")
    script = f'set p to {choose} {" ".join(props)}\nPOSIX path of p'
    try:
        r = subprocess.run(
            ["osascript", "-e", script],
            capture_output=True,
            text=True,
            check=False,
        )
    except OSError:
        return None
    if r.returncode == 0:
        return (r.stdout or "").strip().rstrip("/")
    if r.returncode == 1:  # user cancel
        return ""
    return None


def run_gui(*, initial_root: Path | None = None) -> int:
    err = _ensure_gui_deps()
    if err is not None:
        print(err, file=sys.stderr)
        return 2

    try:
        import customtkinter as ctk
    except ImportError:
        print(
            "Project Studio GUI still cannot import customtkinter after bootstrap.\n"
            "CLI still works: migrate_project.py audit|plan|apply",
            file=sys.stderr,
        )
        return 2

    ctk.set_appearance_mode("dark")
    ctk.set_default_color_theme("dark-blue")
    app = ProjectStudioApp(ctk, initial_root=initial_root)
    app.mainloop()
    return 0


def _venv_python(venv_dir: Path = _VENV_DIR) -> Path:
    if os.name == "nt":
        return venv_dir / "Scripts" / "python.exe"
    return venv_dir / "bin" / "python"


def _reqs_stamp() -> str:
    raw = _REQS.read_bytes() if _REQS.is_file() else b"customtkinter>=5.2\n"
    return hashlib.sha256(raw).hexdigest()


def _running_in_venv(venv_dir: Path = _VENV_DIR) -> bool:
    """True when this process is the managed GUI venv (not the system python).

    Do not compare ``sys.executable.resolve()`` to the venv launcher — on Linux
    the venv ``bin/python`` often symlinks to the system interpreter, so
    resolve() falsely reports they are the same.
    """
    try:
        return Path(sys.prefix).resolve() == venv_dir.resolve()
    except OSError:
        return False


def _ctk_importable() -> bool:
    try:
        import customtkinter  # noqa: F401

        return True
    except ImportError:
        return False


def _venv_can_import_ctk(venv_py: Path) -> bool:
    try:
        r = subprocess.run(
            [str(venv_py), "-c", "import customtkinter"],
            check=False,
            capture_output=True,
            text=True,
        )
        return r.returncode == 0
    except OSError:
        return False


def _run(cmd: list[str], *, label: str) -> str | None:
    try:
        subprocess.run(cmd, check=True)
        return None
    except subprocess.CalledProcessError as e:
        return (
            f"Project Studio GUI bootstrap failed ({label}, exit {e.returncode}).\n"
            f"  cmd: {' '.join(cmd)}\n"
            "CLI still works: migrate_project.py audit|plan|apply"
        )
    except OSError as e:
        return (
            f"Project Studio GUI bootstrap failed ({label}): {e}\n"
            "CLI still works: migrate_project.py audit|plan|apply"
        )


def _ensure_gui_deps() -> str | None:
    """Create/fill ``.venv`` if needed, then re-exec into it.

    Returns an error message on failure, or None when customtkinter is ready
    in the current interpreter (possibly after re-exec).
    """
    in_venv = _running_in_venv()
    if _ctk_importable():
        # Refresh managed venv when the stamp drifts and we are already in it.
        if in_venv:
            if _STAMP.is_file() and _STAMP.read_text(encoding="utf-8").strip() == _reqs_stamp():
                return None
        else:
            return None

    if not _REQS.is_file():
        return (
            f"Missing {_REQS} — cannot bootstrap GUI deps.\n"
            "CLI still works: migrate_project.py audit|plan|apply"
        )

    venv_py = _venv_python()
    stamp = _reqs_stamp()
    need_venv = not venv_py.is_file()
    stamp_ok = _STAMP.is_file() and _STAMP.read_text(encoding="utf-8").strip() == stamp
    need_install = need_venv or not stamp_ok or not _venv_can_import_ctk(venv_py)

    if need_venv:
        print(
            "Project Studio GUI: creating local .venv (first run)…",
            file=sys.stderr,
        )
        err = _run([sys.executable, "-m", "venv", str(_VENV_DIR)], label="python -m venv")
        if err:
            return err
        venv_py = _venv_python()
        if not venv_py.is_file():
            return (
                f"venv created but interpreter missing: {venv_py}\n"
                "CLI still works: migrate_project.py audit|plan|apply"
            )

    if need_install:
        print(
            "Project Studio GUI: installing deps from requirements-gui.txt…",
            file=sys.stderr,
        )
        err = _run(
            [str(venv_py), "-m", "pip", "install", "-r", str(_REQS)],
            label="pip install",
        )
        if err:
            return err
        _STAMP.write_text(stamp + "\n", encoding="utf-8")

    if _running_in_venv():
        if _ctk_importable():
            return None
        return (
            "Installed into .venv but customtkinter still missing.\n"
            "CLI still works: migrate_project.py audit|plan|apply"
        )

    if os.environ.get(_BOOTSTRAP_ENV) == "1":
        return (
            "Re-exec into .venv did not pick up customtkinter.\n"
            "CLI still works: migrate_project.py audit|plan|apply"
        )

    os.environ[_BOOTSTRAP_ENV] = "1"
    os.execv(str(venv_py), [str(venv_py), *sys.argv])
    return "exec failed"  # pragma: no cover


class ProjectStudioApp:
    def __init__(self, ctk, *, initial_root: Path | None = None) -> None:
        self.ctk = ctk
        self.root = ctk.CTk()
        self.root.title("PSXRecomp Project Studio")
        self.root.geometry("1100x820")
        self.root.minsize(900, 640)

        self.root_var = tk.StringVar(value=str(initial_root) if initial_root else "")
        self.disc_var = tk.StringVar()
        self.players_var = tk.StringVar(value="2")
        self.zip_var = tk.StringVar()
        self.netplay_var = tk.BooleanVar(value=False)
        self.ci_var = tk.BooleanVar(value=True)
        self.probe_var = tk.BooleanVar(value=False)
        self.dry_run_var = tk.BooleanVar(value=True)
        self.force_var = tk.BooleanVar(value=False)
        self.status_var = tk.StringVar(value="Open a game repo and Audit.")

        self.git_branch_var = tk.StringVar()
        self.git_psx_branch_var = tk.StringVar(value="master")
        self.git_ui_branch_var = tk.StringVar(value="master")
        self.git_msg_var = tk.StringVar()
        self.git_remote_update_var = tk.BooleanVar(value=False)
        self.release_version_var = tk.StringVar()
        self.release_bump_var = tk.StringVar(value="patch")
        self.release_publish_var = tk.BooleanVar(value=True)
        self.release_reuse_var = tk.BooleanVar(value=True)

        self._report = None
        self._plan = None
        self._step_vars: dict[str, tk.BooleanVar] = {}
        self._git_status = None

        self._build()
        if initial_root:
            self.refresh_audit()

    def mainloop(self) -> None:
        self.root.mainloop()

    def _build(self) -> None:
        ctk = self.ctk
        root = self.root

        header = ctk.CTkFrame(root, fg_color="transparent")
        header.pack(fill="x", padx=16, pady=(16, 8))
        ctk.CTkLabel(
            header,
            text="Project Studio",
            font=ctk.CTkFont(size=22, weight="bold"),
        ).pack(side="left")
        ctk.CTkLabel(
            header,
            text="Migrate, audit, and GitHub ops for setup-host game repos",
            text_color=("gray40", "gray65"),
            font=ctk.CTkFont(size=13),
        ).pack(side="left", padx=(12, 0), pady=(6, 0))

        path_row = ctk.CTkFrame(root, fg_color="transparent")
        path_row.pack(fill="x", padx=16, pady=4)
        ctk.CTkLabel(path_row, text="Game repo", width=88, anchor="w").pack(side="left")
        ctk.CTkEntry(path_row, textvariable=self.root_var, height=34).pack(
            side="left", fill="x", expand=True, padx=(0, 8)
        )
        ctk.CTkButton(
            path_row, text="Browse…", width=90, height=34, command=self._browse_root
        ).pack(side="left")
        ctk.CTkButton(
            path_row,
            text="Audit",
            width=80,
            height=34,
            fg_color=("#3a7ebf", "#1f538d"),
            command=self.refresh_audit,
        ).pack(side="left", padx=(8, 0))

        tabs = ctk.CTkTabview(root, corner_radius=10)
        tabs.pack(fill="both", expand=True, padx=16, pady=8)
        tab_migrate = tabs.add("Migrate")
        tab_git = tabs.add("Git / GitHub")
        self._tabs = tabs

        self._build_migrate_tab(tab_migrate)
        self._build_git_tab(tab_git)

        log_wrap = ctk.CTkFrame(root, corner_radius=10)
        log_wrap.pack(fill="both", expand=False, padx=16, pady=(0, 16))
        ctk.CTkLabel(
            log_wrap,
            text="Log",
            font=ctk.CTkFont(size=13, weight="bold"),
        ).pack(anchor="w", padx=12, pady=(8, 2))
        self.log = ctk.CTkTextbox(log_wrap, height=120, font=ctk.CTkFont(size=12))
        self.log.pack(fill="both", expand=True, padx=10, pady=(0, 10))

    def _build_migrate_tab(self, tab) -> None:
        ctk = self.ctk

        opts = ctk.CTkFrame(tab, corner_radius=10)
        opts.pack(fill="x", padx=4, pady=4)
        ctk.CTkLabel(
            opts,
            text="Options  ·  setup-host only",
            font=ctk.CTkFont(size=13, weight="bold"),
        ).pack(anchor="w", padx=12, pady=(10, 4))

        disc_row = ctk.CTkFrame(opts, fg_color="transparent")
        disc_row.pack(fill="x", padx=12, pady=4)
        ctk.CTkLabel(disc_row, text="Disc .cue", width=88, anchor="w").pack(side="left")
        ctk.CTkEntry(disc_row, textvariable=self.disc_var, height=32).pack(
            side="left", fill="x", expand=True, padx=(0, 8)
        )
        ctk.CTkButton(
            disc_row, text="Browse…", width=90, height=32, command=self._browse_disc
        ).pack(side="left")

        row2 = ctk.CTkFrame(opts, fg_color="transparent")
        row2.pack(fill="x", padx=12, pady=4)
        ctk.CTkLabel(row2, text="Players", width=88, anchor="w").pack(side="left")
        ctk.CTkOptionMenu(
            row2,
            values=[str(i) for i in range(1, 9)],
            variable=self.players_var,
            width=72,
            height=32,
        ).pack(side="left")
        ctk.CTkLabel(row2, text="Zip prefix", width=80, anchor="e").pack(
            side="left", padx=(16, 8)
        )
        ctk.CTkEntry(row2, textvariable=self.zip_var, width=140, height=32).pack(
            side="left"
        )

        toggles = ctk.CTkFrame(opts, fg_color="transparent")
        toggles.pack(fill="x", padx=12, pady=(4, 4))
        for text, var in (
            ("Netplay", self.netplay_var),
            ("CI workflow", self.ci_var),
            ("Probe disc", self.probe_var),
            ("Dry-run", self.dry_run_var),
            ("Force", self.force_var),
        ):
            ctk.CTkSwitch(toggles, text=text, variable=var, width=120).pack(
                side="left", padx=(0, 16)
            )

        ctk.CTkLabel(
            opts,
            text="Wizard + recomp-ui are always enabled. Releases are setup-host only (no prebuilt game C).",
            text_color=("gray40", "gray60"),
            font=ctk.CTkFont(size=12),
            wraplength=900,
            justify="left",
        ).pack(anchor="w", padx=12, pady=(2, 10))

        mid = ctk.CTkFrame(tab, fg_color="transparent")
        mid.pack(fill="both", expand=True, padx=4, pady=4)
        mid.grid_columnconfigure(0, weight=1)
        mid.grid_columnconfigure(1, weight=1)
        mid.grid_rowconfigure(0, weight=1)

        audit_wrap = ctk.CTkFrame(mid, corner_radius=10)
        audit_wrap.grid(row=0, column=0, sticky="nsew", padx=(0, 6))
        ctk.CTkLabel(
            audit_wrap,
            text="Audit",
            font=ctk.CTkFont(size=13, weight="bold"),
        ).pack(anchor="w", padx=12, pady=(10, 4))
        self.audit_list = ctk.CTkScrollableFrame(audit_wrap, fg_color="transparent")
        self.audit_list.pack(fill="both", expand=True, padx=8, pady=(0, 10))

        plan_wrap = ctk.CTkFrame(mid, corner_radius=10)
        plan_wrap.grid(row=0, column=1, sticky="nsew", padx=(6, 0))
        ctk.CTkLabel(
            plan_wrap,
            text="Plan  ·  uncheck to skip",
            font=ctk.CTkFont(size=13, weight="bold"),
        ).pack(anchor="w", padx=12, pady=(10, 4))
        self.plan_checks = ctk.CTkScrollableFrame(plan_wrap, fg_color="transparent")
        self.plan_checks.pack(fill="both", expand=True, padx=8, pady=(0, 10))

        bottom = ctk.CTkFrame(tab, fg_color="transparent")
        bottom.pack(fill="x", padx=4, pady=8)
        ctk.CTkButton(
            bottom, text="Build plan", width=120, height=36, command=self.refresh_plan
        ).pack(side="left")
        ctk.CTkButton(
            bottom,
            text="Apply selected",
            width=140,
            height=36,
            fg_color=("#2ecc71", "#1e8449"),
            hover_color=("#27ae60", "#196f3d"),
            command=self.apply_selected,
        ).pack(side="left", padx=10)
        ctk.CTkLabel(
            bottom,
            textvariable=self.status_var,
            text_color=("gray30", "gray70"),
            anchor="w",
        ).pack(side="left", fill="x", expand=True)

    def _build_git_tab(self, tab) -> None:
        ctk = self.ctk

        top = ctk.CTkFrame(tab, corner_radius=10)
        top.pack(fill="x", padx=4, pady=4)
        head = ctk.CTkFrame(top, fg_color="transparent")
        head.pack(fill="x", padx=12, pady=(10, 4))
        ctk.CTkLabel(
            head, text="Repository", font=ctk.CTkFont(size=13, weight="bold")
        ).pack(side="left")
        ctk.CTkButton(
            head, text="Refresh", width=90, height=30, command=self.refresh_git
        ).pack(side="right")
        ctk.CTkSwitch(
            head, text="Dry-run", variable=self.dry_run_var, width=100
        ).pack(side="right", padx=12)

        self.git_summary_var = tk.StringVar(value="Open a game repo, then Refresh.")
        ctk.CTkLabel(
            top,
            textvariable=self.git_summary_var,
            text_color=("gray30", "gray70"),
            anchor="w",
            justify="left",
            wraplength=980,
        ).pack(fill="x", padx=12, pady=(0, 8))

        branch_row = ctk.CTkFrame(top, fg_color="transparent")
        branch_row.pack(fill="x", padx=12, pady=(0, 10))
        ctk.CTkLabel(branch_row, text="Game branch", width=100, anchor="w").pack(
            side="left"
        )
        ctk.CTkEntry(branch_row, textvariable=self.git_branch_var, width=160, height=30).pack(
            side="left", padx=(0, 8)
        )
        ctk.CTkButton(
            branch_row,
            text="Checkout",
            width=90,
            height=30,
            command=self._git_checkout_branch,
        ).pack(side="left")
        ctk.CTkButton(
            branch_row, text="Pull", width=70, height=30, command=self._git_pull
        ).pack(side="left", padx=6)
        ctk.CTkButton(
            branch_row, text="Push", width=70, height=30, command=self._git_push
        ).pack(side="left")

        sub_wrap = ctk.CTkFrame(tab, corner_radius=10)
        sub_wrap.pack(fill="both", expand=True, padx=4, pady=4)
        ctk.CTkLabel(
            sub_wrap,
            text="Submodules  ·  CI pins gitlink SHAs; branch is for --remote updates",
            font=ctk.CTkFont(size=13, weight="bold"),
        ).pack(anchor="w", padx=12, pady=(10, 4))

        cfg = ctk.CTkFrame(sub_wrap, fg_color="transparent")
        cfg.pack(fill="x", padx=12, pady=4)
        ctk.CTkLabel(cfg, text="psxrecomp branch", width=130, anchor="w").pack(side="left")
        ctk.CTkEntry(cfg, textvariable=self.git_psx_branch_var, width=120, height=28).pack(
            side="left", padx=(0, 12)
        )
        ctk.CTkLabel(cfg, text="recomp-ui branch", width=130, anchor="w").pack(side="left")
        ctk.CTkEntry(cfg, textvariable=self.git_ui_branch_var, width=120, height=28).pack(
            side="left", padx=(0, 12)
        )
        ctk.CTkButton(
            cfg,
            text="Ensure both",
            width=110,
            height=28,
            command=self._git_ensure_submodules,
        ).pack(side="left")
        ctk.CTkButton(
            cfg,
            text="Save branches",
            width=120,
            height=28,
            command=self._git_save_submodule_branches,
        ).pack(side="left", padx=8)

        actions = ctk.CTkFrame(sub_wrap, fg_color="transparent")
        actions.pack(fill="x", padx=12, pady=(0, 4))
        ctk.CTkSwitch(
            actions,
            text="Update to remote tip",
            variable=self.git_remote_update_var,
            width=160,
        ).pack(side="left")
        ctk.CTkButton(
            actions,
            text="Update submodules",
            width=150,
            height=30,
            command=self._git_update_submodules,
        ).pack(side="left", padx=12)

        self.git_sub_list = ctk.CTkScrollableFrame(sub_wrap, fg_color="transparent", height=140)
        self.git_sub_list.pack(fill="both", expand=True, padx=8, pady=(0, 10))

        commit_wrap = ctk.CTkFrame(tab, corner_radius=10)
        commit_wrap.pack(fill="x", padx=4, pady=4)
        ctk.CTkLabel(
            commit_wrap,
            text="Commit  ·  no force-push / amend",
            font=ctk.CTkFont(size=13, weight="bold"),
        ).pack(anchor="w", padx=12, pady=(10, 4))
        msg_row = ctk.CTkFrame(commit_wrap, fg_color="transparent")
        msg_row.pack(fill="x", padx=12, pady=(0, 10))
        ctk.CTkEntry(
            msg_row,
            textvariable=self.git_msg_var,
            placeholder_text="Commit message",
            height=32,
        ).pack(side="left", fill="x", expand=True, padx=(0, 8))
        ctk.CTkButton(
            msg_row, text="Commit all", width=110, height=32, command=self._git_commit
        ).pack(side="left")

        rel = ctk.CTkFrame(tab, corner_radius=10)
        rel.pack(fill="x", padx=4, pady=(4, 8))
        ctk.CTkLabel(
            rel,
            text="Release CI  ·  workflow_dispatch release.yml",
            font=ctk.CTkFont(size=13, weight="bold"),
        ).pack(anchor="w", padx=12, pady=(10, 4))
        rel_row = ctk.CTkFrame(rel, fg_color="transparent")
        rel_row.pack(fill="x", padx=12, pady=(0, 4))
        ctk.CTkLabel(rel_row, text="Version", width=70, anchor="w").pack(side="left")
        ctk.CTkEntry(
            rel_row,
            textvariable=self.release_version_var,
            placeholder_text="empty = auto-bump",
            width=140,
            height=28,
        ).pack(side="left", padx=(0, 10))
        ctk.CTkLabel(rel_row, text="Bump", width=50, anchor="w").pack(side="left")
        ctk.CTkOptionMenu(
            rel_row,
            values=["patch", "minor", "major"],
            variable=self.release_bump_var,
            width=90,
            height=28,
        ).pack(side="left", padx=(0, 12))
        ctk.CTkSwitch(
            rel_row, text="Publish", variable=self.release_publish_var, width=100
        ).pack(side="left", padx=(0, 10))
        ctk.CTkSwitch(
            rel_row, text="Reuse emitters", variable=self.release_reuse_var, width=130
        ).pack(side="left")
        ctk.CTkButton(
            rel,
            text="Run release workflow",
            width=180,
            height=34,
            fg_color=("#c0392b", "#922b21"),
            hover_color=("#a93226", "#7b241c"),
            command=self._git_run_release,
        ).pack(anchor="w", padx=12, pady=(4, 12))

    def _game_root(self) -> Path | None:
        root_s = self.root_var.get().strip()
        if not root_s:
            messagebox.showerror("Project Studio", "Choose a game repo root.", parent=self.root)
            return None
        root = Path(root_s).expanduser().resolve()
        if not root.is_dir():
            messagebox.showerror(
                "Project Studio", f"Not a directory:\n{root}", parent=self.root
            )
            return None
        return root

    def _git_dry(self) -> bool:
        return bool(self.dry_run_var.get())

    def _log_cmd(self, r) -> None:
        self._log(f"[{'OK' if r.ok else 'FAIL'}] {r.message}")
        if r.detail:
            for line in str(r.detail).splitlines()[:20]:
                self._log(f"  {line}")

    def _browse_root(self) -> None:
        start = self.root_var.get().strip() or None
        path = _pick_directory(
            title="Select game repository root",
            parent=self.root,
            initialdir=start,
        )
        if path:
            self.root_var.set(path)
            self.refresh_audit()

    def _browse_disc(self) -> None:
        start = self.disc_var.get().strip() or self.root_var.get().strip() or None
        if start and Path(start).is_file():
            start = str(Path(start).parent)
        path = _pick_open_file(
            title="Select Redump .cue",
            parent=self.root,
            initialdir=start,
            filetypes=[("Cue sheet", "*.cue"), ("All", "*.*")],
        )
        if path:
            self.disc_var.set(path)
            self.probe_var.set(True)

    def _migrate_options(self) -> MigrateOptions:
        return MigrateOptions(
            disc=self.disc_var.get().strip() or None,
            players=int(self.players_var.get() or "2"),
            zip_prefix=self.zip_var.get().strip() or None,
            enable_recomp_ui=True,
            enable_wizard=True,
            enable_netplay=bool(self.netplay_var.get()),
            enable_ci=bool(self.ci_var.get()),
            probe_disc=bool(self.probe_var.get()) and bool(self.disc_var.get().strip()),
            record_pins=True,
            dry_run=bool(self.dry_run_var.get()),
            force=bool(self.force_var.get()),
        )

    def _log(self, msg: str) -> None:
        self.log.insert("end", msg + "\n")
        self.log.see("end")

    def _clear_children(self, frame) -> None:
        for child in frame.winfo_children():
            child.destroy()

    def refresh_audit(self) -> None:
        ctk = self.ctk
        root = self._game_root()
        if root is None:
            return
        self._report = audit_project(root)
        self._clear_children(self.audit_list)
        for c in self._report.checks:
            color = _STATUS_COLORS.get(c.status, "#8a9199")
            row = ctk.CTkFrame(self.audit_list, fg_color=("gray90", "gray17"), corner_radius=8)
            row.pack(fill="x", pady=3, padx=2)
            badge = ctk.CTkLabel(
                row,
                text=c.status.value.upper(),
                text_color=color,
                font=ctk.CTkFont(size=11, weight="bold"),
                width=52,
            )
            badge.pack(side="left", padx=(10, 6), pady=8)
            body = ctk.CTkFrame(row, fg_color="transparent")
            body.pack(side="left", fill="x", expand=True, padx=(0, 10), pady=6)
            ctk.CTkLabel(
                body,
                text=c.title,
                font=ctk.CTkFont(size=13, weight="bold"),
                anchor="w",
            ).pack(fill="x")
            detail = c.detail or c.severity.value
            ctk.CTkLabel(
                body,
                text=detail,
                text_color=("gray40", "gray65"),
                font=ctk.CTkFont(size=11),
                anchor="w",
                wraplength=420,
                justify="left",
            ).pack(fill="x")
        self.status_var.set(
            f"Layout: {self._report.layout.value} · boot={self._report.boot_exe or '?'}"
        )
        self._log(f"Audited {root} → {self._report.layout.value}")
        self.refresh_plan()
        self.refresh_git(quiet=True)

    def refresh_plan(self) -> None:
        ctk = self.ctk
        root_s = self.root_var.get().strip()
        if not root_s:
            return
        root = Path(root_s).expanduser().resolve()
        opts = self._migrate_options()
        self._plan = build_plan(root, opts, self._report)
        self._clear_children(self.plan_checks)
        self._step_vars.clear()
        if not self._plan.steps:
            ctk.CTkLabel(
                self.plan_checks,
                text="No migration steps needed.",
                text_color=("gray40", "gray65"),
            ).pack(anchor="w", padx=4, pady=8)
            return
        for step in self._plan.steps:
            var = tk.BooleanVar(value=step.selected)
            self._step_vars[step.op_id] = var
            row = ctk.CTkFrame(self.plan_checks, fg_color=("gray90", "gray17"), corner_radius=8)
            row.pack(fill="x", pady=3, padx=2)
            ctk.CTkCheckBox(row, text="", variable=var, width=28).pack(
                side="left", padx=(10, 4), pady=10
            )
            body = ctk.CTkFrame(row, fg_color="transparent")
            body.pack(side="left", fill="x", expand=True, padx=(0, 10), pady=6)
            ctk.CTkLabel(
                body,
                text=step.title,
                font=ctk.CTkFont(size=13, weight="bold"),
                anchor="w",
            ).pack(fill="x")
            sub = step.op_id if not step.detail else f"{step.op_id}  ·  {step.detail}"
            ctk.CTkLabel(
                body,
                text=sub,
                text_color=("gray40", "gray65"),
                font=ctk.CTkFont(size=11),
                anchor="w",
                wraplength=420,
                justify="left",
            ).pack(fill="x")

    def apply_selected(self) -> None:
        if self._plan is None:
            self.refresh_plan()
        if self._plan is None or not self._plan.steps:
            messagebox.showinfo("Project Studio", "Nothing to apply.", parent=self.root)
            return
        opts = self._migrate_options()
        opts.enable_wizard = True
        opts.enable_recomp_ui = True
        self._plan.options = opts
        selected = [op for op, var in self._step_vars.items() if var.get()]
        if not selected:
            messagebox.showinfo("Project Studio", "No steps selected.", parent=self.root)
            return
        for step in self._plan.steps:
            step.selected = step.op_id in selected

        mode = "DRY-RUN" if opts.dry_run else "APPLY"
        if not opts.dry_run:
            if not messagebox.askyesno(
                "Project Studio",
                f"Apply {len(selected)} step(s) to:\n{self._plan.root}\n\n"
                "A backup CMakeLists.txt.pre_migrate.bak is written when rewriting CMake.",
                parent=self.root,
            ):
                return

        self._log(f"--- {mode} ({len(selected)} ops) ---")
        results = apply_plan(self._plan, selected=selected)
        failed = 0
        for r in results:
            self._log(f"[{'OK' if r.ok else 'FAIL'}] {r.op_id}: {r.message}")
            for p in r.changed_paths:
                self._log(f"  · {p}")
            if not r.ok:
                failed += 1
        self.status_var.set(f"{mode} done — {failed} failed, {len(results) - failed} ok")
        if not opts.dry_run:
            self.refresh_audit()
        if failed:
            messagebox.showwarning(
                "Project Studio",
                f"{failed} step(s) failed — see log.",
                parent=self.root,
            )
        else:
            messagebox.showinfo(
                "Project Studio",
                f"{mode} completed successfully.",
                parent=self.root,
            )

    def refresh_git(self, *, quiet: bool = False) -> None:
        from .gitops import repo_status

        ctk = self.ctk
        root = self._game_root()
        if root is None:
            return
        st = repo_status(root)
        self._git_status = st
        if not st.is_git:
            self.git_summary_var.set("Not a git repository.")
            self._clear_children(self.git_sub_list)
            ctk.CTkLabel(
                self.git_sub_list,
                text="Initialize git in this folder first.",
                text_color=("gray40", "gray65"),
            ).pack(anchor="w", padx=4, pady=8)
            return

        self.git_branch_var.set(st.branch)
        dirty = "dirty" if st.dirty else "clean"
        parts = [
            f"{st.branch or '?'}",
            f"{dirty} (staged={st.staged} unstaged={st.unstaged} untracked={st.untracked})",
            f"ahead={st.ahead} behind={st.behind}",
        ]
        if st.upstream:
            parts.insert(1, f"→ {st.upstream}")
        if st.gh_repo:
            parts.append(f"gh:{st.gh_repo}")
        elif st.remote_url:
            parts.append(st.remote_url)
        if not st.gh_available:
            parts.append("gh CLI missing")
        self.git_summary_var.set("  ·  ".join(parts))

        for s in st.submodules:
            if s.path == "psxrecomp" and s.branch:
                self.git_psx_branch_var.set(s.branch)
            if s.path == "recomp-ui" and s.branch:
                self.git_ui_branch_var.set(s.branch)

        self._clear_children(self.git_sub_list)
        for s in st.submodules:
            row = ctk.CTkFrame(self.git_sub_list, fg_color=("gray90", "gray17"), corner_radius=8)
            row.pack(fill="x", pady=3, padx=2)
            mark = "OK" if s.present else "MISS"
            color = "#3dd68c" if s.present else "#f07178"
            ctk.CTkLabel(
                row,
                text=mark,
                text_color=color,
                font=ctk.CTkFont(size=11, weight="bold"),
                width=48,
            ).pack(side="left", padx=(10, 6), pady=8)
            body = ctk.CTkFrame(row, fg_color="transparent")
            body.pack(side="left", fill="x", expand=True, padx=(0, 10), pady=6)
            ctk.CTkLabel(
                body,
                text=s.path,
                font=ctk.CTkFont(size=13, weight="bold"),
                anchor="w",
            ).pack(fill="x")
            detail = (
                f"branch={s.branch or '-'}  sha={s.sha or '-'}  "
                f"{s.url or '(no url)'}"
            )
            ctk.CTkLabel(
                body,
                text=detail,
                text_color=("gray40", "gray65"),
                font=ctk.CTkFont(size=11),
                anchor="w",
                wraplength=900,
                justify="left",
            ).pack(fill="x")
        if not quiet:
            self._log(f"Git status: {st.branch} ({dirty})")

    def _git_checkout_branch(self) -> None:
        from .gitops import set_repo_branch

        root = self._game_root()
        if root is None:
            return
        branch = self.git_branch_var.get().strip()
        if not branch:
            messagebox.showerror("Project Studio", "Enter a branch name.", parent=self.root)
            return
        r = set_repo_branch(root, branch, dry_run=self._git_dry())
        self._log_cmd(r)
        self.refresh_git()

    def _git_ensure_submodules(self) -> None:
        from .gitops import ensure_known_submodules

        root = self._game_root()
        if root is None:
            return
        results = ensure_known_submodules(
            root,
            psxrecomp_branch=self.git_psx_branch_var.get().strip() or "master",
            recomp_ui_branch=self.git_ui_branch_var.get().strip() or "master",
            dry_run=self._git_dry(),
        )
        for r in results:
            self._log_cmd(r)
        self.refresh_git()

    def _git_save_submodule_branches(self) -> None:
        from .gitops import set_submodule_branch

        root = self._game_root()
        if root is None:
            return
        for path, var in (
            ("psxrecomp", self.git_psx_branch_var),
            ("recomp-ui", self.git_ui_branch_var),
        ):
            branch = var.get().strip()
            if not branch:
                continue
            r = set_submodule_branch(root, path, branch, dry_run=self._git_dry())
            self._log_cmd(r)
        self.refresh_git()

    def _git_update_submodules(self) -> None:
        from .gitops import update_submodules

        root = self._game_root()
        if root is None:
            return
        remote = bool(self.git_remote_update_var.get())
        if remote and not self._git_dry():
            if not messagebox.askyesno(
                "Project Studio",
                "Update submodules to their remote tracking tips?\n\n"
                "This moves working trees; commit the gitlink changes afterward "
                "so CI picks up the new SHAs.",
                parent=self.root,
            ):
                return
        r = update_submodules(
            root,
            paths=["psxrecomp", "recomp-ui"],
            remote=remote,
            dry_run=self._git_dry(),
        )
        self._log_cmd(r)
        self.refresh_git()

    def _git_pull(self) -> None:
        from .gitops import pull

        root = self._game_root()
        if root is None:
            return
        r = pull(root, dry_run=self._git_dry())
        self._log_cmd(r)
        self.refresh_git()

    def _git_commit(self) -> None:
        from .gitops import commit_all

        root = self._game_root()
        if root is None:
            return
        msg = self.git_msg_var.get().strip()
        if not msg:
            messagebox.showerror("Project Studio", "Enter a commit message.", parent=self.root)
            return
        if not self._git_dry():
            if not messagebox.askyesno(
                "Project Studio",
                f"Commit all changes in:\n{root}\n\n{msg}",
                parent=self.root,
            ):
                return
        r = commit_all(root, msg, dry_run=self._git_dry())
        self._log_cmd(r)
        if r.ok and not self._git_dry():
            self.git_msg_var.set("")
        self.refresh_git()

    def _git_push(self) -> None:
        from .gitops import push

        root = self._game_root()
        if root is None:
            return
        if not self._git_dry():
            if not messagebox.askyesno(
                "Project Studio",
                f"Push HEAD to origin for:\n{root}\n\n"
                "(ff-only remote expected; no force-push)",
                parent=self.root,
            ):
                return
        r = push(root, dry_run=self._git_dry())
        self._log_cmd(r)
        self.refresh_git()

    def _git_run_release(self) -> None:
        from .gitops import run_release_workflow

        root = self._game_root()
        if root is None:
            return
        version = self.release_version_var.get().strip()
        bump = self.release_bump_var.get().strip() or "patch"
        publish = bool(self.release_publish_var.get())
        reuse = bool(self.release_reuse_var.get())
        if not self._git_dry():
            detail = (
                f"version={version or '(auto)'} bump={bump} "
                f"publish={publish} reuse_cached_emitters={reuse}"
            )
            if not messagebox.askyesno(
                "Project Studio",
                f"Dispatch release.yml on GitHub?\n\n{detail}",
                parent=self.root,
            ):
                return
        r = run_release_workflow(
            root,
            version=version,
            bump=bump,
            publish=publish,
            reuse_cached_emitters=reuse,
            dry_run=self._git_dry(),
        )
        self._log_cmd(r)
        if r.ok:
            messagebox.showinfo("Project Studio", r.message, parent=self.root)
        else:
            messagebox.showwarning("Project Studio", r.message, parent=self.root)
