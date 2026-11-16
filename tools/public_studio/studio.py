"""The portable public Studio desktop application."""
from pathlib import Path
import os
import json
import queue
from collections import deque
from tkinter.scrolledtext import ScrolledText
import sys
import threading
import webbrowser
import tkinter as tk
from tkinter import filedialog, messagebox, ttk

import backend
import dropfiles
import selection
import settings

HELP = (
    "One game: select its image, all of its discs together, or its .m3u playlist — "
    "a file listing that game's discs in order.\n\n"
    "Multiple games: select one image for each single-disc game. For each multi-disc "
    "game, select its .m3u playlist. You can mix images and playlists.\n\n"
    "If there is a .cue file, choose it instead of its .bin tracks. Add matching "
    ".sbi files in the same selection. Each game gets its own output folder."
)


class Studio:
    def __init__(self, root, data_root=None):
        self.root = root
        self.data_root = data_root or Path(__file__).resolve().parent / "public-studio-data"
        self.preferences = settings.load(self.data_root)
        self.files = []
        self.jobs = []
        self.runner = None
        self.progress_phase = 0
        self.messages = queue.Queue()
        self.log_lines = deque(maxlen=5000)
        self.log_window = None
        self.log_text = None
        self.multiple = tk.BooleanVar(value=False)
        self.brand = tk.PhotoImage(file=str(Path(__file__).parent / "assets" / "retro-porting-toolkit.png")).subsample(2, 2)
        self.status = tk.StringVar(value="Choose your game files")
        self.main = None
        self.help_popup = None
        self.root.title("Workbench Studio")
        self.root.geometry("920x640")
        self.root.minsize(720, 560)
        self.root.protocol("WM_DELETE_WINDOW", self.close)
        self.style = ttk.Style()
        self.style.theme_use("clam")
        self.apply_theme()
        self.show_main()
        self.remove_drop = dropfiles.accept_files(root, self.add_files)
        self.root.after(100, self.poll)

    def apply_theme(self):
        dark = self.preferences.theme == "dark"
        self.colors = dict(bg="#141415" if dark else "#fafafa",
                           surface="#19191b" if dark else "#ffffff",
                           raised="#232325" if dark else "#efeff0",
                           text="#ededed" if dark else "#171717",
                           second="#b0b0b5" if dark else "#525252",
                           line="#353537" if dark else "#d4d4d7",
                           green="#65b68b" if dark else "#246b44",
                           greenink="#0d2318" if dark else "#ffffff")
        c = self.colors
        self.root.configure(bg=c["bg"])
        self.style.configure(".", font=("Segoe UI", 11), background=c["bg"], foreground=c["text"])
        self.style.configure("TFrame", background=c["bg"])
        self.style.configure("TLabel", background=c["bg"], foreground=c["text"])
        self.style.configure("Muted.TLabel", foreground=c["second"])
        self.style.configure("Title.TLabel", font=("Segoe UI", 14))
        self.style.configure("TButton", background=c["raised"], bordercolor=c["line"], padding=(12, 8), relief="flat")
        self.style.map("TButton", background=[("active", c["surface"])])
        self.style.configure("TRadiobutton", background=c["bg"], foreground=c["text"], padding=(8, 8))
        self.style.map("TRadiobutton", background=[("active", c["raised"])])
        self.style.configure("TCheckbutton", background=c["bg"], foreground=c["text"])
        self.style.configure("TEntry", fieldbackground=c["raised"], foreground=c["text"], bordercolor=c["line"])
        self.style.configure("Treeview", background=c["surface"], fieldbackground=c["surface"], foreground=c["text"], rowheight=40, bordercolor=c["line"])
        self.style.map("Treeview", background=[("selected", c["raised"])], foreground=[("selected", c["text"])])
        self.style.configure("TCombobox", fieldbackground=c["raised"], foreground=c["text"])

    def clear_page(self):
        if self.main:
            self.main.destroy()
        self.main = ttk.Frame(self.root, padding=32)
        self.main.pack(fill="both", expand=True)
        self.main.columnconfigure(0, weight=1)
        return self.main

    def show_main(self):
        page = self.clear_page()
        page.rowconfigure(2, weight=1)
        header = ttk.Frame(page)
        header.grid(row=0, column=0, sticky="ew", pady=(0, 24))
        ttk.Label(header, text="Workbench Studio", style="Title.TLabel").pack(side="left")
        ttk.Label(header, image=self.brand).pack(side="left", before=header.winfo_children()[0], padx=(0, 12))
        ttk.Label(header, text="PlayStation · PS1", style="Muted.TLabel").pack(side="left", padx=24)
        ttk.Button(header, text="Settings", command=self.show_settings).pack(side="right")
        modes = ttk.Frame(page)
        modes.grid(row=1, column=0, sticky="w", pady=(0, 24))
        ttk.Radiobutton(modes, text="One Game", variable=self.multiple, value=False, command=self.refresh_selection).pack(side="left")
        ttk.Radiobutton(modes, text="Multiple Games", variable=self.multiple, value=True, command=self.refresh_selection).pack(side="left", padx=(8, 8))
        help_button = ttk.Button(modes, text="?", width=3, command=lambda: messagebox.showinfo("Choosing game files", HELP, parent=self.root))
        help_button.pack(side="left")
        help_button.bind("<Enter>", lambda event: self.show_help(help_button))
        help_button.bind("<Leave>", lambda event: self.hide_help())
        self.tray = ttk.Frame(page)
        self.tray.grid(row=2, column=0, sticky="nsew")
        self.tray.rowconfigure(0, weight=1)
        self.tray.columnconfigure(0, weight=1)
        self.tree = ttk.Treeview(self.tray, show="tree", selectmode="extended")
        self.tree.grid(row=0, column=0, sticky="nsew")
        self.tree.bind("<Delete>", self.remove_selected)
        self.empty = ttk.Frame(self.tray)
        ttk.Label(self.empty, text="Drop your game files here", style="Muted.TLabel").pack(pady=(0, 16))
        ttk.Button(self.empty, text="Choose files", command=self.choose_files).pack()
        ttk.Label(self.empty, text="Disc images, playlists and matching SBI files", style="Muted.TLabel").pack(pady=(16, 0))
        trayfooter = ttk.Frame(self.tray, padding=(8, 8))
        trayfooter.grid(row=1, column=0, sticky="ew")
        self.count = ttk.Label(trayfooter, style="Muted.TLabel")
        self.count.pack(side="left")
        ttk.Button(trayfooter, text="Add files", command=self.choose_files).pack(side="right")
        ttk.Button(trayfooter, text="Clear", command=self.clear_files).pack(side="right", padx=8)
        footer = ttk.Frame(page)
        footer.grid(row=3, column=0, sticky="ew", pady=(24, 0))
        self.track = tk.Canvas(footer, width=36, height=36, background=self.colors["bg"], highlightthickness=0)
        for inset in (3, 9, 15):
            self.track.create_oval(inset, inset, 36-inset, 36-inset, outline=self.colors["line"])
        self.track.pack(side="left", anchor="s", pady=8)
        info = ttk.Frame(footer)
        info.pack(side="left", anchor="s", padx=12, pady=8)
        ttk.Label(info, textvariable=self.status, style="Muted.TLabel", wraplength=480).pack(anchor="w", pady=(0, 12))
        links = ttk.Frame(info)
        links.pack(anchor="w")
        for label, url in [("GitHub", "https://github.com/RetroPortingToolKit"),
                           ("Discord", "https://discord.gg/Jn6ZgtC85u"),
                           ("Website", "https://retroportingtoolkit.com/")]:
            link = tk.Label(links, text=label, bg=self.colors["bg"], fg=self.colors["second"],
                            font=("Segoe UI", 9), cursor="hand2", takefocus=1)
            link.pack(side="left", padx=(0, 16))
            link.bind("<Button-1>", lambda event, url=url: webbrowser.open(url))
            link.bind("<Return>", lambda event, url=url: webbrowser.open(url))
        ttk.Button(info, text="Build log", command=self.show_log).pack(anchor="w", pady=(8, 0))
        self.build_canvas = tk.Canvas(footer, width=112, height=112, background=self.colors["bg"], highlightthickness=0, takefocus=1)
        self.build_canvas.pack(side="right")
        self.build_canvas.bind("<Button-1>", lambda event: self.start_build())
        self.build_canvas.bind("<Return>", lambda event: self.start_build())
        self.build_canvas.bind("<space>", lambda event: self.start_build())
        self.build_canvas.bind("<FocusIn>", lambda event: self.draw_build(focused=True))
        self.build_canvas.bind("<FocusOut>", lambda event: self.draw_build())
        self.refresh_selection()

    def show_help(self, widget):
        self.hide_help()
        popup = tk.Toplevel(self.root)
        popup.overrideredirect(True)
        popup.configure(bg=self.colors["line"])
        popup.geometry(f"+{widget.winfo_rootx()}+{widget.winfo_rooty()+widget.winfo_height()+8}")
        tk.Label(popup, text=HELP, wraplength=360, justify="left", padx=20, pady=20,
                 font=("Segoe UI", 10), background=self.colors["raised"], foreground=self.colors["text"]).pack(padx=1, pady=1)
        self.help_popup = popup

    def hide_help(self):
        if self.help_popup:
            self.help_popup.destroy()
            self.help_popup = None

    def choose_files(self):
        self.add_files(filedialog.askopenfilenames(parent=self.root, title="Choose game files",
            filetypes=[("Game files", "*.cue *.iso *.chd *.m3u *.sbi"), ("All files", "*.*")]))

    def add_files(self, paths):
        if self.runner:
            return
        self.files = list(dict.fromkeys(self.files + list(paths)))
        if hasattr(self, "tree") and self.tree.winfo_exists():
            self.refresh_selection()

    def clear_files(self):
        if not self.runner:
            self.files = []
            self.refresh_selection()

    def remove_selected(self, event=None):
        if not self.runner:
            selected = {self.tree.item(row, "values")[0] for row in self.tree.selection() if self.tree.item(row, "values")}
            self.files = [p for p in self.files if str(Path(p).resolve()) not in selected]
            self.refresh_selection()

    def refresh_selection(self):
        self.tree.delete(*self.tree.get_children())
        for path in self.files:
            file = Path(path)
            self.tree.insert("", "end", text=file.name, values=[str(file.resolve())])
        self.empty.place_forget()
        if not self.files:
            self.empty.place(relx=.5, rely=.5, anchor="center")
        self.count.configure(text=f"{len(self.files)} files selected" if self.files else "No files selected")
        self.ready = False
        try:
            self.jobs = selection.group_files(self.files, self.multiple.get())
            if self.jobs:
                if not self.preferences.output_folder:
                    raise ValueError("Choose a default output folder in Settings")
                if not self.preferences.skip_bios and not any(Path(p).is_file() for p in self.preferences.bios_files):
                    raise ValueError("Add your BIOS files in Settings")
                for game in self.jobs:
                    bios = next((p for p in self.preferences.bios_files if Path(p).is_file()), "")
                    backend.validate(backend.BuildOptions([str(p) for p in game.discs], bios,
                        self.preferences.output_folder, [str(p) for p in game.sbi.values()],
                        skip_bios=self.preferences.skip_bios))
                self.ready = True
                self.status.set(f"{len(self.jobs)} game" + ("s" if len(self.jobs) != 1 else "") + " ready to build")
            else:
                self.status.set("Choose your game files")
        except (ValueError, OSError) as error:
            self.status.set(str(error))
        self.draw_build()

    def draw_build(self, focused=False):
        c = self.colors
        active = self.ready and not self.runner
        canvas = self.build_canvas
        canvas.delete("all")
        fill = c["green"] if active else c["raised"]
        canvas.create_polygon(24,1,88,1,111,1,111,24,111,88,111,111,88,111,24,111,1,111,1,88,1,24,1,1,
                              smooth=True, splinesteps=24, fill=fill, outline=c["second"] if focused else c["line"])
        canvas.create_text(56,56,text="Cancel" if self.runner else "BUILD",font=("Segoe UI",12),
                           fill=c["greenink"] if active else c["second"])

    def show_settings(self):
        if self.runner:
            return
        self.hide_help()
        page = self.clear_page()
        top = ttk.Frame(page)
        top.grid(row=0, column=0, sticky="ew", pady=(0, 24))
        ttk.Label(top, text="Settings", style="Title.TLabel").pack(side="left")
        ttk.Button(top, text="Done", command=self.save_settings).pack(side="right")
        page.rowconfigure(1, weight=1)
        scroller = tk.Canvas(page, bg=self.colors["bg"], highlightthickness=0)
        scroller.grid(row=1, column=0, sticky="nsew")
        scrollbar = ttk.Scrollbar(page, orient="vertical", command=scroller.yview)
        scrollbar.grid(row=1, column=1, sticky="ns")
        scroller.configure(yscrollcommand=scrollbar.set)
        body = ttk.Frame(scroller)
        body.columnconfigure(0, weight=1)
        content = scroller.create_window(0, 0, window=body, anchor="nw")
        body.bind("<Configure>", lambda event: scroller.configure(scrollregion=scroller.bbox("all")))
        scroller.bind("<Configure>", lambda event: scroller.itemconfigure(content, width=event.width))
        scroller.bind("<MouseWheel>", lambda event: scroller.yview_scroll(-int(event.delta / 120), "units"))
        page = body
        self.setting_vars = {}
        self.bios_regions = dict(self.preferences.bios_regions)
        ttk.Label(page, text="BIOS files").grid(row=1,column=0,sticky="w")
        self.bios_list = tk.Listbox(page, height=3, bg=self.colors["surface"], fg=self.colors["text"],
                                   selectbackground=self.colors["raised"], highlightthickness=1, relief="flat", font=("Segoe UI",10))
        self.bios_list.grid(row=2,column=0,sticky="ew",pady=(8,8))
        for path in self.preferences.bios_files:
            self.bios_list.insert("end", path)
        actions=ttk.Frame(page)
        actions.grid(row=3,column=0,sticky="w",pady=(0,24))
        ttk.Button(actions,text="Add BIOS files",command=lambda: [self.bios_list.insert("end",p) for p in filedialog.askopenfilenames(parent=self.root,title="Choose BIOS files")]).pack(side="left")
        ttk.Button(actions,text="Remove",command=lambda: [self.bios_list.delete(i) for i in reversed(self.bios_list.curselection())]).pack(side="left",padx=8)
        ttk.Label(actions, text="Region").pack(side="left", padx=(16, 8))
        region = tk.StringVar(value="Auto")
        region_picker = ttk.Combobox(actions, textvariable=region, values=["Auto", "USA", "Europe", "Japan"], state="readonly", width=9)
        region_picker.pack(side="left")
        def picked_region(event=None):
            chosen = self.bios_list.curselection()
            if chosen:
                self.bios_regions[self.bios_list.get(chosen[0])] = region.get()
        def picked_bios(event=None):
            chosen = self.bios_list.curselection()
            if chosen:
                region.set(self.bios_regions.get(self.bios_list.get(chosen[0]), "Auto"))
        self.bios_list.bind("<<ListboxSelect>>", picked_bios)
        region_picker.bind("<<ComboboxSelected>>", picked_region)
        self.setting_entry(page,4,"Default output folder","output_folder",folder=True)
        appearance=ttk.Frame(page)
        appearance.grid(row=5,column=0,sticky="ew",pady=(16,16))
        ttk.Label(appearance,text="Appearance").pack(side="left",padx=(0,16))
        self.setting_vars["theme"]=tk.StringVar(value=self.preferences.theme)
        ttk.Combobox(appearance,textvariable=self.setting_vars["theme"],values=["dark","light"],state="readonly",width=12).pack(side="left")
        expanded = tk.BooleanVar(value=False)
        advanced=ttk.Frame(page,padding=(0,16))
        def toggle_advanced():
            if expanded.get():
                advanced.grid(row=7,column=0,sticky="nsew")
            else:
                advanced.grid_remove()
        ttk.Checkbutton(page,text="Advanced",variable=expanded,command=toggle_advanced).grid(row=6,column=0,sticky="w",pady=(8,0))
        advanced.columnconfigure(0,weight=1)
        options=ttk.Frame(advanced)
        options.grid(row=0,column=0,sticky="w",pady=(0,12))
        for name,label in [("portable","Portable"),("skip_bios","Skip BIOS"),("pgo","Optimize (PGO)"),("diagnostics","Diagnostics")]:
            self.setting_vars[name]=tk.BooleanVar(value=getattr(self.preferences,name))
            ttk.Checkbutton(options,text=label,variable=self.setting_vars[name]).pack(side="left",padx=(0,16))
        self.setting_entry(advanced,1,"Core pin file","core_pin",file=True)
        self.setting_entry(advanced,2,"UI pin file","ui_pin",file=True)
        for row,name,label in [(3,"core_ref","Online core version"),(4,"ui_ref","Online UI version"),(5,"custom_core","Custom core folder"),(6,"custom_ui","Custom UI folder")]:
            self.setting_entry(advanced,row,label,name,folder=name.startswith("custom"))
        ttk.Label(advanced,text="A core pin file uses the existing pin builder. Leave blank for online sources.\nSources download and cache only when you press Build",style="Muted.TLabel").grid(row=7,column=0,sticky="w",pady=(12,0))

    def setting_entry(self,parent,row,label,name,folder=False,file=False):
        frame=ttk.Frame(parent)
        frame.grid(row=row,column=0,sticky="ew",pady=4)
        ttk.Label(frame,text=label,width=22).pack(side="left")
        var=tk.StringVar(value=getattr(self.preferences,name))
        self.setting_vars[name]=var
        ttk.Entry(frame,textvariable=var).pack(side="left",fill="x",expand=True)
        if folder or file:
            def choose():
                chosen=(filedialog.askopenfilename(parent=self.root,title=label,filetypes=[("Pin records","*.json"),("All files","*.*")])
                        if file else filedialog.askdirectory(parent=self.root,title=label))
                if chosen:
                    var.set(chosen)
            ttk.Button(frame,text="Browse",command=choose).pack(side="right",padx=(8,0))

    def save_settings(self):
        values={name:var.get() for name,var in self.setting_vars.items()}
        values["bios_files"]=list(self.bios_list.get(0,"end"))
        values["bios_regions"]={p: self.bios_regions.get(p,"Auto") for p in values["bios_files"]}
        self.preferences=settings.Settings(**values)
        try:
            settings.save(self.data_root,self.preferences)
        except OSError as error:
            messagebox.showerror("Could not save Settings",str(error),parent=self.root)
            return
        self.apply_theme()
        self.show_main()

    def show_log(self):
        if self.log_window and self.log_window.winfo_exists():
            self.log_window.lift()
            return
        self.log_window = tk.Toplevel(self.root)
        self.log_window.title("Build log — Workbench Studio")
        self.log_window.geometry("820x480")
        self.log_text = ScrolledText(self.log_window, wrap="none", font=("Consolas", 10),
                                    bg=self.colors["surface"], fg=self.colors["text"])
        self.log_text.pack(fill="both", expand=True)
        self.log_text.insert("end", "\n".join(self.log_lines))
        self.log_text.configure(state="disabled")

    def append_log(self, value):
        lines = str(value).splitlines() or [""]
        self.log_lines.extend(line[:4096] for line in lines)
        if self.log_text and self.log_text.winfo_exists():
            self.log_text.configure(state="normal")
            self.log_text.delete("1.0", "end")
            self.log_text.insert("end", "\n".join(self.log_lines))
            self.log_text.see("end")
            self.log_text.configure(state="disabled")

    def start_build(self):
        if self.runner:
            self.runner.cancel()
            self.status.set("Cancelling…")
            return
        if not self.ready:
            return
        self.runner=backend.Runner(lambda line:self.messages.put(("log",line)))
        self.draw_build()
        runner=self.runner
        jobs=list(self.jobs)
        preferences=self.preferences
        def work():
            try:
                result=backend.build_batch(jobs,preferences,self.data_root,runner)
                self.messages.put(("done",result))
            except Exception as error:
                self.messages.put(("error",str(error)))
        threading.Thread(target=work,daemon=True).start()

    def poll(self):
        if hasattr(self, "track") and self.track.winfo_exists():
            self.track.delete("progress")
            if self.runner:
                self.progress_phase = (self.progress_phase + 7) % 360
                self.track.create_arc(3, 3, 33, 33, start=self.progress_phase, extent=65,
                                      style="arc", outline=self.colors["green"],
                                      width=2, tags="progress")
        try:
            while True:
                kind,value=self.messages.get_nowait()
                if kind=="log":
                    self.append_log(value)
                    line = (str(value).splitlines() or [""])[-1]
                    if line.startswith("WORKBENCH_EVENT "):
                        try:
                            event = json.loads(line.removeprefix("WORKBENCH_EVENT "))
                            line = f"{event.get('label', 'Build')}: {event.get('status', '')}"
                        except ValueError:
                            pass
                    self.status.set(line[-150:])
                else:
                    self.runner=None
                    if kind == "error":
                        self.append_log(value)
                    self.status.set("Build complete" if kind=="done" and value["state"]=="completed"
                                    else "Build failed — open Build log for details")
                    self.draw_build()
        except queue.Empty:
            pass
        self.root.after(100,self.poll)

    def close(self):
        if self.runner:
            self.runner.cancel()
            self.status.set("Cancelling before closing…")
            self.root.after(200,self.close)
            return
        self.remove_drop()
        self.hide_help()
        self.root.destroy()


def configure_runtime():
    runtime = Path(sys.executable).parent
    if (runtime / "tcl" / "tcl8.6").is_dir():
        os.environ["TCL_LIBRARY"] = str(runtime / "tcl" / "tcl8.6")
        os.environ["TK_LIBRARY"] = str(runtime / "tcl" / "tk8.6")


def main():
    configure_runtime()
    if "--check-runtime" in sys.argv:
        print(f"Python {sys.version.split()[0]}; Tk {tk.TkVersion}; public Studio runtime present")
        return
    data_root = None
    if "--data-root" in sys.argv:
        index = sys.argv.index("--data-root")
        if index + 1 >= len(sys.argv):
            raise ValueError("--data-root needs a folder")
        data_root = Path(sys.argv[index + 1]).expanduser().resolve()
    if "--cache-root" in sys.argv:
        index = sys.argv.index("--cache-root")
        if index + 1 >= len(sys.argv):
            raise ValueError("--cache-root needs a folder")
        os.environ["WORKBENCH_PUBLIC_CACHE_ROOT"] = str(Path(sys.argv[index + 1]).expanduser().resolve())
    root=tk.Tk()
    try:
        Studio(root, data_root=data_root)
        root.mainloop()
    except Exception as error:
        messagebox.showerror("Workbench Studio",str(error),parent=root)
        root.destroy()
        raise


if __name__=="__main__":
    main()
