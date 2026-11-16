"""Focused public-builder checks, using synthetic inputs only."""
import importlib.util
import json
from pathlib import Path
import struct
import sys
import tempfile
import threading
import time
import tomllib
import unittest
from unittest import mock
import os
import stat

import backend
import compiler_options
import selection
import settings


def synthetic_disc(root):
    """A minimal authored ISO with SYSTEM.CNF and a two-instruction PS-X EXE."""
    data = bytearray(26 * 2048)
    pvd = 16 * 2048
    data[pvd:pvd + 7] = b"\x01CD001\x01"
    data[pvd + 40:pvd + 72] = b"STUDIO_TEST".ljust(32)
    struct.pack_into("<I", data, pvd + 158, 20)
    struct.pack_into("<I", data, pvd + 166, 2048)
    cnf = b"BOOT = cdrom:\\SLUS_000.01;1\r\nTCB = 4\r\nEVENT = 16\r\nSTACK = 801fff00\r\n"
    exe = bytearray(4096)
    exe[:8] = b"PS-X EXE"
    struct.pack_into("<I", exe, 0x10, 0x80010000)
    struct.pack_into("<I", exe, 0x18, 0x80010000)
    struct.pack_into("<I", exe, 0x1c, 2048)
    struct.pack_into("<I", exe, 0x30, 0x801fff00)
    struct.pack_into("<II", exe, 0x800, 0x08004000, 0)
    cursor = 20 * 2048
    for name, lba, payload in [("SYSTEM.CNF;1", 21, cnf), ("SLUS_000.01;1", 22, exe)]:
        raw = name.encode()
        size = 33 + len(raw) + (1 if len(raw) % 2 == 0 else 0)
        data[cursor] = size
        struct.pack_into("<I", data, cursor + 2, lba)
        struct.pack_into("<I", data, cursor + 10, len(payload))
        data[cursor + 32] = len(raw)
        data[cursor + 33:cursor + 33 + len(raw)] = raw
        data[lba * 2048:lba * 2048 + len(payload)] = payload
        cursor += size
    (root / "track.bin").write_bytes(data)
    cue = root / "test.cue"
    cue.write_text('FILE "track.bin" BINARY\n  TRACK 01 MODE1/2048\n    INDEX 01 00:00:00\n', encoding="utf-8")
    return cue


class BuilderChecks(unittest.TestCase):
    def test_source_product_media_survives_move_without_source_paths(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            cue = synthetic_disc(root)
            cue.with_suffix('.sbi').write_bytes(b'SBI fixture')
            bios = root / 'bios.bin'
            bios.write_bytes(b'BIOS fixture')
            config = root / 'game.toml'
            config.write_text('[game]\ndisc = "old.cue"\nserial = "SLUS-00001"\n')
            product = root / 'payload'
            product.mkdir()
            backend.stage_source_runtime(config, product, [cue], bios, ['SLUS-00001'], True, False)
            moved = root / 'moved'
            product.rename(moved)
            game = tomllib.loads((moved / 'game.toml').read_text())['game']
            selected = moved / game['disc']
            self.assertTrue(selected.is_file())
            self.assertEqual((selected.parent / 'track.bin').read_bytes(), (root / 'track.bin').read_bytes())
            self.assertEqual(selected.with_suffix('.sbi').read_bytes(), b'SBI fixture')
            self.assertEqual((moved / (moved / 'bios.cfg').read_text().strip()).read_bytes(), b'BIOS fixture')
            for filename in ('game.toml', 'disc.cfg', 'bios.cfg', 'settings.toml'):
                self.assertNotIn(str(root), (moved / filename).read_text())

    def test_source_product_without_bios_does_not_reference_a_folder(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            cue = synthetic_disc(root)
            config = root / 'game.toml'
            config.write_text('[game]\ndisc = "old.cue"\n')
            product = root / 'payload'
            product.mkdir()
            backend.stage_source_runtime(config, product, [cue], '', ['SLUS-00001'], True, True)
            settings = tomllib.loads((product / 'settings.toml').read_text())
            self.assertEqual(settings['bios']['path'], '')
            self.assertTrue(settings['video']['fast_boot'])
            self.assertEqual((product / 'bios.cfg').read_text().strip(), '')

    def test_source_compiler_choices_use_cmake_ipo_contract(self):
        for lto in (False, True):
            flags = compiler_options.arguments('fast', lto, source=True)
            self.assertNotIn('--no-lto', flags)
            self.assertIn('--cmake-extra=-DPSX_RUNTIME_IPO=' + ('ON' if lto else 'OFF'), flags)
        self.assertIn('--no-lto', compiler_options.arguments('fast', False))

    def test_project_only_toolchain_never_publishes_global_environment(self):
        path = Path(__file__).parents[1] / "toolchain_pack.py"
        spec = importlib.util.spec_from_file_location("isolated_toolchain_check", path)
        pack = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(pack)
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            with mock.patch.dict(os.environ, {"PSX_STUDIO_PROJECT_ONLY": "1", "RETCOMM_DATA_HOME": str(root / "data")}), \
                 mock.patch.object(pack, "unwrap_pack_root", return_value=root), \
                 mock.patch.object(pack, "pack_root_looks_usable", return_value=True), \
                 mock.patch.object(pack, "activate_toolchain_bin") as activate, \
                 mock.patch.object(pack, "_register_user_path_windows", side_effect=AssertionError("registry publication forbidden")) as windows, \
                 mock.patch.object(pack, "_register_user_path_unix", side_effect=AssertionError("login configuration publication forbidden")) as unix, \
                 mock.patch.object(pack, "_set_latest_pointer", side_effect=AssertionError("global pointer publication forbidden")) as latest:
                self.assertEqual(pack.retcomm_data_homes(), [root / "data"])
                self.assertEqual(pack.register_toolchain_user_env(root), root)
                activate.assert_called_once_with(root / "bin", log=None)
                windows.assert_not_called()
                unix.assert_not_called()
                latest.assert_not_called()

    @unittest.skipUnless(os.name == "nt", "Windows process-tree cancellation")
    def test_real_child_and_grandchild_are_cancelled(self):
        import ctypes
        from ctypes import wintypes
        kernel = ctypes.WinDLL("kernel32", use_last_error=True)
        kernel.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
        kernel.OpenProcess.restype = wintypes.HANDLE
        kernel.WaitForSingleObject.argtypes = [wintypes.HANDLE, wintypes.DWORD]
        kernel.WaitForSingleObject.restype = wintypes.DWORD
        kernel.TerminateProcess.argtypes = [wintypes.HANDLE, wintypes.UINT]
        kernel.CloseHandle.argtypes = [wintypes.HANDLE]
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            marker = root / "grandchild.pid"
            child = "import time; time.sleep(60)"
            parent = ("import subprocess,sys,time; from pathlib import Path; "
                      f"p=subprocess.Popen([sys.executable,'-c',{child!r}],creationflags=subprocess.CREATE_NO_WINDOW); "
                      f"Path({str(marker)!r}).write_text(str(p.pid)); time.sleep(60)")
            runner = backend.Runner(lambda line: None)
            handles = []
            def cancel_when_ready():
                deadline = time.monotonic() + 15
                while not marker.exists() and time.monotonic() < deadline:
                    time.sleep(.02)
                if marker.exists():
                    handle = kernel.OpenProcess(0x00100000 | 0x1000 | 0x0001, False, int(marker.read_text()))
                    if handle:
                        handles.append(handle)
                runner.cancel()
            monitor = threading.Thread(target=cancel_when_ready)
            monitor.start()
            try:
                with self.assertRaises(backend.Cancelled):
                    runner.run([sys.executable, "-c", parent], root)
                monitor.join()
                self.assertEqual(len(handles), 1, "Grandchild never became observable")
                self.assertEqual(kernel.WaitForSingleObject(handles[0], 2000), 0,
                                 "Grandchild survived cancellation of the active process tree")
            finally:
                monitor.join()
                for handle in handles:
                    if kernel.WaitForSingleObject(handle, 0) != 0:
                        kernel.TerminateProcess(handle, 1)
                    kernel.CloseHandle(handle)

    def test_batch_resolves_pair_once_and_stops_after_cancellation(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            cue = synthetic_disc(root)
            jobs = [selection.GameJob(f"Game {i}", [cue]) for i in range(3)]
            preferences = settings.Settings(output_folder=str(root / "Games"), skip_bios=True)
            resolved = []
            def snapshot(kind, ref, custom, cache, destination, runner):
                destination.mkdir()
                resolved.append((kind, ref))
                return {"kind": kind, "commit": "a" * 40, "content_sha256": kind}
            runner = backend.Runner(lambda line: None)
            calls = []
            def game_build(options, data_root, active, *, prepared_sources, prepared_runtime):
                calls.append(prepared_sources)
                preferences.core_ref = "a-moving-ref-changed-during-the-build"
                if len(calls) == 2:
                    active.cancel()
                return {"output": options.output}
            with mock.patch.object(backend, "snapshot_source", side_effect=snapshot), \
                 mock.patch.object(backend, "build", side_effect=game_build), \
                 mock.patch("buildtools.prepare", return_value={"receipt": {}}):
                with self.assertRaises(backend.Cancelled):
                    backend.build_batch(jobs, preferences, root / "data", runner)
            self.assertEqual(resolved, [("core", "master"), ("ui", "master")])
            self.assertEqual(len(calls), 2)
            self.assertIs(calls[0], calls[1])
            receipt = json.loads(next((root / "data/history").glob("batch-*/batch-record.json")).read_text())
            self.assertEqual(receipt["state"], "cancelled")
            self.assertEqual(len(receipt["games"]), 3)
            self.assertEqual(receipt["games"][2]["state"], "queued")

    def test_iso_normalization_and_portable_cue_preserve_input(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            synthetic_disc(root)
            iso = root / "Disc image.iso"
            (root / "track.bin").rename(iso)
            original_hash = backend.sha256(iso)
            cue = backend.normalize_image(iso, root / "normalized", root, backend.Runner(), None)
            staged = backend.stage_disc(cue, root / "portable" / "disc")
            self.assertNotIn(str(root).replace("\\", "/"), staged.read_text())
            self.assertEqual(backend.sha256(staged.parent / iso.name), original_hash)
            self.assertEqual(backend.sha256(iso), original_hash)
            output = backend.Runner(lambda line: None).run([sys.executable,
                Path(__file__).with_name("probe_disc.py"), staged, "--identity-only"], root)
            self.assertEqual(json.loads(output[output.index("{"):])["serial"], "SLUS-00001")

    def test_bios_region_selection_uses_matching_file(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            us, eu = root / "usa.bin", root / "europe.bin"
            us.touch()
            eu.touch()
            regions = {str(us): "USA", str(eu): "Europe"}
            self.assertEqual(backend.choose_bios([str(us), str(eu)], regions, "SLES-01234"), str(eu))
            with self.assertRaisesRegex(ValueError, "Japan"):
                backend.choose_bios([str(us), str(eu)], regions, "SLPS-01234")

    def test_settings_are_persistent_and_edition_local(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp) / "public-edition"
            self.assertFalse(root.exists())
            value = settings.load(root)
            self.assertTrue(value.portable)
            self.assertFalse(root.exists())
            value.bios_files = ["user BIOS.bin"]
            value.output_folder = "output with spaces"
            value.pgo = True
            settings.save(root, value)
            self.assertEqual(settings.load(root), value)
            self.assertEqual([p.name for p in root.iterdir()], ["settings.json"])

    def test_playlist_stays_one_job_beside_single_disc_game(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            for name in ["disc1.iso", "disc2.iso", "other.iso"]:
                (root / name).touch()
            playlist = root / "two discs.m3u"
            playlist.write_text("\ufeff#EXTM3U\ndisc1.iso\ndisc2.iso\n", encoding="utf-8")
            jobs = selection.group_files([playlist, root / "other.iso", root / "disc1.iso"], True)
            self.assertEqual(len(jobs), 2)
            self.assertEqual([p.name for p in jobs[0].discs], ["disc1.iso", "disc2.iso"])
            self.assertEqual(jobs[0].playlist, playlist)
            self.assertEqual([p.name for p in jobs[1].discs], ["other.iso"])

    def test_one_game_groups_discs_and_matches_sbi_in_same_selection(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            for name in ["part 1.iso", "part 2.iso", "part 2.sbi"]:
                (root / name).touch()
            jobs = selection.group_files([root / "part 1.iso", root / "part 2.iso", root / "part 2.sbi"])
            self.assertEqual(len(jobs), 1)
            self.assertEqual(len(jobs[0].discs), 2)
            self.assertEqual(jobs[0].sbi[root / "part 2.iso"], root / "part 2.sbi")
            with self.assertRaisesRegex(ValueError, "alongside"):
                selection.group_files([root / "part 2.sbi"])

    def test_missing_playlist_disc_and_orphan_sbi_are_real_errors(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            playlist = root / "missing.m3u"
            playlist.write_text("absent.iso\n", encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "not found"):
                selection.group_files([playlist], True)
            (root / "game.iso").touch()
            (root / "other.sbi").touch()
            with self.assertRaisesRegex(ValueError, "no matching"):
                selection.group_files([root / "game.iso", root / "other.sbi"], True)

    def test_real_windows_scaffold_uses_snapshots_without_fetch(self):
        if sys.platform != "win32":
            self.skipTest("Windows scaffold adapter")
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            cue = synthetic_disc(root)
            framework = root / "core-source"
            ui = root / "ui-source"
            parent = root / "output"
            temp = root / "temp"
            for path in (framework, ui, parent, temp):
                path.mkdir()
            (framework / "psxrecomp_cli.py").write_text("# fixture")
            (ui / "recomp_ui.cmake").write_text("# fixture")
            import os
            env = os.environ.copy()
            env.update(TEMP=str(temp), TMP=str(temp), PSXRECOMP_PYTHON=sys.executable)
            runner = backend.Runner(lambda line: None)
            runner.run(["pwsh", "-NoProfile", "-File", Path(__file__).with_name("setup_public.ps1"),
                        "-Disc", cue, "-Name", "Studio Test", "-Dir", parent, "-Yes",
                        "-NoGenerate", "-NoBuild", "-NoGithub", "-NoCi", "-NoNetplay",
                        "-NoFetchBoxart", "-NoWizard", "-EnableRecompUi", "-StageDisc",
                        "-PsxrecompSourceRoot", framework, "-RecompUiSourceRoot", ui,
                        "-ToolkitRoot", Path(__file__).parents[1] / "new_project_layout"], root, env=env)
            projects = [p for p in parent.iterdir() if (p / "game.toml").exists()]
            self.assertEqual(len(projects), 1)
            project = projects[0]
            self.assertFalse((project / ".git").exists())
            self.assertTrue((project / "disc" / "track.bin").is_file())
            self.assertIn("SLUS-00001", (project / "game.toml").read_text())
            self.assertEqual((project / "psxrecomp" / "psxrecomp_cli.py").read_text(), "# fixture")

    def test_import_has_no_network_or_process_activity(self):
        with mock.patch("subprocess.Popen", side_effect=AssertionError("process at import")), \
             mock.patch("urllib.request.urlopen", side_effect=AssertionError("network at import")):
            spec = importlib.util.spec_from_file_location("backend_import_check", Path(backend.__file__))
            module = importlib.util.module_from_spec(spec)
            sys.modules[spec.name] = module
            try:
                spec.loader.exec_module(module)
            finally:
                del sys.modules[spec.name]

    def test_readonly_source_stays_readonly_but_snapshot_can_be_restaged(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            source = root / "custom"
            source.mkdir()
            original = source / "bios.bin"
            original.write_bytes(b"private fixture")
            original.chmod(stat.S_IREAD)
            try:
                identity = backend.copy_source(source, root / "snapshot")
                copied = root / "snapshot/bios.bin"
                try:
                    self.assertTrue(copied.stat().st_mode & stat.S_IWRITE)
                    copied.write_bytes(b"replacement fixture")
                    self.assertEqual(original.read_bytes(), b"private fixture")
                    self.assertFalse(original.stat().st_mode & stat.S_IWRITE)
                    self.assertEqual(identity["files"]["bios.bin"], backend.sha256(original))
                finally:
                    copied.chmod(stat.S_IREAD | stat.S_IWRITE)
            finally:
                original.chmod(stat.S_IREAD | stat.S_IWRITE)

    def test_dirty_and_untracked_source_bytes_survive(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            source = root / "custom"
            source.mkdir()
            (source / ".git").mkdir()
            (source / ".git" / "secret-config").write_text("excluded")
            (source / "runtime.c").write_text("modified, not committed")
            (source / "extra.c").write_text("untracked addition")
            first = backend.copy_source(source, root / "first")
            self.assertEqual((root / "first" / "runtime.c").read_text(), "modified, not committed")
            self.assertEqual((root / "first" / "extra.c").read_text(), "untracked addition")
            self.assertFalse((root / "first" / ".git").exists())
            (source / "runtime.c").write_text("next revision")
            second = backend.copy_source(source, root / "second")
            self.assertNotEqual(first["content_sha256"], second["content_sha256"])
            self.assertEqual(first["files"]["runtime.c"], backend.sha256(root / "first" / "runtime.c"))

    def test_recursive_snapshot_refuses(self):
        with tempfile.TemporaryDirectory() as tmp:
            with self.assertRaisesRegex(ValueError, "outside"):
                backend.copy_source(Path(tmp), Path(tmp) / "nested")

    def test_invalid_inputs_fail_before_any_source_download(self):
        with tempfile.TemporaryDirectory() as tmp:
            options = backend.BuildOptions([str(Path(tmp) / "absent.cue")], "", tmp, skip_bios=True)
            with mock.patch.object(backend, "snapshot_source", side_effect=AssertionError("download started")):
                with self.assertRaisesRegex(ValueError, "not found"):
                    backend.build(options, Path(tmp) / "data", backend.Runner())

    def test_cue_missing_track_and_sbi_validation(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            cue = synthetic_disc(root)
            options = backend.BuildOptions([str(cue)], "", tmp, skip_bios=True)
            backend.validate(options)
            options.sbi = [str(root / "absent.sbi")]
            with self.assertRaisesRegex(ValueError, "SBI"):
                backend.validate(options)
            options.sbi = []
            (root / "track.bin").unlink()
            with self.assertRaisesRegex(ValueError, "track not found"):
                backend.validate(options)

    def test_real_bundled_probe_reads_authored_disc(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            cue = synthetic_disc(root)
            result = backend.Runner(lambda line: None).run(
                [sys.executable, Path(__file__).with_name("probe_disc.py"), cue, "--identity-only"], root)
            record = json.loads(result[result.index("{"):])
            self.assertEqual(record["serial"], "SLUS-00001")
            self.assertEqual(record["entry_pc"], "0x80010000")

    def test_silent_child_cancels_without_waiting_for_output(self):
        runner = backend.Runner(lambda line: None)
        timer = threading.Timer(.3, runner.cancel)
        timer.start()
        started = time.monotonic()
        try:
            with self.assertRaises(backend.Cancelled):
                runner.run([sys.executable, "-c", "import time; time.sleep(30)"], Path.cwd())
        finally:
            timer.cancel()
        self.assertLess(time.monotonic() - started, 5)


if __name__ == "__main__":
    unittest.main(verbosity=2)
