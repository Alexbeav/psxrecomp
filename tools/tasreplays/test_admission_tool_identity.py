"""Each route module must check its source reference against the admission tool that admitted it.

An admitted source reference records admission_tool_sha256, the digest of the admission tool's
bytes, and the route module that replays it refuses the reference unless that digest equals the
tool file it imports. One tool file can serve several references, so editing it for one title
moves the digest under the others: pinning the BizHawk 2.10 host for Abe's Exoddus changed
octoshock2x_admission.py, and Abe's Oddysee's gate setup then refused its own reference
(PS1B-593). A revision that admitted a reference stays in the tree under its own name.

The admitted references live outside the repository, so the digests they record are held here,
each beside the reference it was read from. Asset-free.
"""
import hashlib
import importlib
from pathlib import Path
import sys
import unittest

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

# Route module -> (its admitted source reference, the admission_tool_sha256 that reference records).
ADMITTED = {
    'abesoddysee': ('abesoddysee-5620M-source-01.json',
                    '8a94bcc276871f67f72694ada4f21adb177c2ab303d71d842a01d1e2e8365517'),
    'crash': ('crash-7798S-source-02.json',
              '8a94bcc276871f67f72694ada4f21adb177c2ab303d71d842a01d1e2e8365517'),
    'exoddus': ('exoddus-6672M-source-02.json',
                'ae9a1038b8e1d4ba82af552357ea4da3f1832401335771c7c4cef0abef88e041'),
}


def sha256(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


class AdmissionToolIdentity(unittest.TestCase):
    def test_each_route_module_checks_against_the_tool_that_admitted_its_reference(self):
        for name, (reference, admitted) in ADMITTED.items():
            with self.subTest(module=name):
                module = importlib.import_module(name)
                tool = Path(module.source.__file__)
                # The expression verify_reference compares the reference's recorded digest with.
                actual = module.source.digest(tool)
                self.assertEqual(actual, sha256(tool))
                self.assertEqual(actual, admitted,
                                 f'{name}.py checks {reference} against {tool.name} ({actual[:8]}), '
                                 f'not the tool revision that admitted it ({admitted[:8]})')

    def test_every_recorded_digest_names_a_file_in_the_tree(self):
        tree = {sha256(p): p.name for p in HERE.glob('*.py')}
        for name, (reference, admitted) in ADMITTED.items():
            with self.subTest(reference=reference):
                self.assertTrue(admitted in tree,
                                f'no file in tools/tasreplays has the digest {reference} records ({admitted[:8]})')


if __name__ == '__main__':
    unittest.main()
