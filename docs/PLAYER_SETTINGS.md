# Private Windows player settings

The PSX launcher opens Settings on **General** (BIOS, startup, audio,
controllers and hotkeys). **Display** holds presentation and graphics,
including internal resolution. These built-in preferences retain their real
provider and persisted values; installed game and third-party plugins stay
in Mods. The legacy embedded launcher and the v2 module use the same pages.

The inventory found45 absent option controls:35 newly presented typed controls,
6 existing controls made reachable, and4 unsupported legacy fields. Those4
are `video.adaptive_view` and `parappa_timing.mode`, `extra_early`, `extra_late`.
They have no ordinary runtime consumer. Their values survive saves, but no
working enhancement or checkbox is claimed. VRR is an additional value on the
existing VSync option, not another distinct setting. Host keyboard actions
are counted separately:8 unreachable actions were7 absent catalog entries and
one hidden OpenLauncher entry. Debug-only CaptureMark and build-dependent
Rewind still require their actual runtime capability.

Advanced settings save immediately to `settings.toml` through the existing
typed provider interface and apply on the next game launch. The title remains
the default; explicit player preferences override it; existing command-line
and environment overrides retain priority. Per-card paths still override the
default card directory. Invalid values are refused. `fast_boot` skips the BIOS
animation; `bios_hle` selects kernel services; `runtime.bios_hle_keep_intro`
preserves the intro with that service tier. These settings reach ordinary
startup, including launches that skip the GUI. They do not restore Studio's
retired build-time FastBoot option and do not repair BIOS execution speed.

The **Advanced** page holds `runtime.gpu_draw_timing` ("GPU drawing time",
default off; environment override `PSX_GPU_DRAW_TIMING`). On, the ordinary
renderer charges each GPU command its drawing time and holds GPU DMA while the
command FIFO is full, so a display-list transfer lasts as long as drawing it.
Team Buddies needs it to finish loading (PS1G-160). It changes guest timing, so
replays recorded with it off may not play the same with it on.

Hotkeys accept up to4 alternatives, individual removal, explicit unbind and
Reset defaults. Current defaults remain: F1 menu,F3 analog,F5 quick-save,
Shift+F5 quick-load,F6 scanlines,F7 states,F11 fullscreen,F12 record.
Ctrl+L/F10 also open the attached game menu. Fullscreen aliases F11,
Alt+Return and Ctrl+F survive reset. Conflicting menu/quick-state bindings
refuse both actions and display feedback; they never silently erase a binding.

Disc controls are visible for empty,single and multi-disc rosters in the
attached provider and software fallback. A roster supplies choices. Empty
selections cannot insert; actual operations retain path/index/callback and
netplay checks. The F7 menu uses Tab/L1/R1 for States/Replays. Replay actions
are Play,Export,confirmed Delete and F2 Rename. Header hints occupy separate
rows. Navigation does not weaken replay verification.

Known original015 recording: actual MediEvil EXEbf3c5d56/core7869 replays slot2
for70frames with59 differing RAM pages, zero end-cycle difference and a core/ext
digest mismatch already at frame0. The original remains a verification failure;
its cause is not isolated. Slot1's92-frame RAM/cycle match under headless
software rendering has an AV-only difference and proves no AV determinism.

Display aspect preserves the saved selection at startup and lobby return. Trusted plugins and the netplay viewport retain their later session overrides. Obsolete quality aliases remain ignored; restored geometry, perspective, supersample and automatic-resolution-floor preferences round-trip with their actual values.

Frame interpolation and its target rate survive ordinary startup without the launcher. In-game Apply updates the existing session baseline, so a later lobby session restores the player choice before trusted plugins activate. The environment override still follows saved preferences at startup.

The existing input/presentation environment overrides are re-applied after launcher write-back and preference restore, before trusted plugin activation. They affect the current process and do not replace the stored interpolation preference during a rematch.
