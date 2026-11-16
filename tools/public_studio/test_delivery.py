import hashlib
from pathlib import Path
import shutil
import tempfile
import unittest
import delivery
import settings
import support_files


class DeliveryChecks(unittest.TestCase):
    def test_direct_game_folder_collision_and_source_preservation(self):
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory);source=root/'private work';source.mkdir();(source/'Game.exe').write_bytes(b'program');(source/'saves').mkdir();(source/'saves/user.sav').write_bytes(b'user progress')
            output=root/'Games';output.mkdir();original=output/'Game (USA)';original.mkdir();(original/'keep.txt').write_text('unrelated output')
            target,record=delivery.deliver(source,output,'Game (USA)')
            self.assertEqual(target.name,'Game (USA) (2)')
            self.assertEqual((target/'saves/user.sav').read_bytes(),b'user progress')
            self.assertEqual((source/'saves/user.sav').read_bytes(),b'user progress')
            self.assertEqual((original/'keep.txt').read_text(),'unrelated output')
            self.assertEqual(record['files_verified'],2)
            self.assertEqual(sorted(p.name for p in output.iterdir()),['Game (USA)','Game (USA) (2)'])

    def test_missing_license_refuses_before_build_support_use(self):
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory)
            required=('compiler_options.py','delivery.py','batch_state.py','native_bridge.py','toolchain-pin.toml','pin_entry.py','asset_preflight.py','pin_builder/workbench_child.py','pin_builder/workbench_progress.py','pin_builder/source_snapshot.py','pin_builder/lean_boot_gate.py','pin_builder/overlay_compile_check.py','pin_builder/product_residue.py')
            for relative in required:
                path=root/relative;path.parent.mkdir(parents=True,exist_ok=True);path.write_text('# fixture')
            with self.assertRaisesRegex(RuntimeError,'LGPL-2.1.txt'):support_files.check(root)
            license_path=root/'pin_builder/licenses/LGPL-2.1.txt';license_path.parent.mkdir();shutil.copy2(Path(__file__).parent/'pin_builder/licenses/LGPL-2.1.txt',license_path)
            self.assertEqual(support_files.check(root)['lgpl_sha256'],support_files.LICENSE_SHA256)
            license_path.write_text('wrong bytes')
            with self.assertRaisesRegex(RuntimeError,'differs'):support_files.check(root)

    def test_new_preferences_keep_runtime_checks_off(self):
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory);prefs=settings.Settings()
            self.assertFalse(prefs.startup_checks)
            prefs.lto=False;prefs.compiler_optimization='fast';settings.save(root,prefs)
            actual=settings.load(root)
            self.assertFalse(actual.startup_checks)
            self.assertFalse(actual.lto)
            self.assertEqual(actual.compiler_optimization,'fast')

if __name__=='__main__':unittest.main(verbosity=2)
