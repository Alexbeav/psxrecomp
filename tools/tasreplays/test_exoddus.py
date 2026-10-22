"""Asset-free checks of the Abe's Exoddus driver: the disc 2 continuation, the route events, the profile."""
import json
from pathlib import Path
import re
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))
import exoddus
import octoshock210_route
import run_native


class ExoddusDriver(unittest.TestCase):
    def test_disc2_is_a_one_byte_continuation(self):
        one = bytearray(0x60000); one[0x538C8] = 1
        two = bytearray(one); two[0x538C8] = 2
        swap = exoddus.continuation(bytes(one), bytes(two))
        self.assertEqual(swap['ram'], '0x800630C8')
        for bad in (bytes(one), bytes(two[:-1]), bytes(two[:5]) + b'\x01' + bytes(two[6:])):
            with self.subTest(size=len(bad)), self.assertRaises(ValueError):
                exoddus.continuation(bytes(one), bad)
        three = bytearray(one); three[0x538C8] = 3
        with self.assertRaises(ValueError):
            exoddus.continuation(bytes(one), bytes(three))

    def test_identity_shape(self):
        for value in (exoddus.ROUTE_SHA, exoddus.P1_WORDS_SHA, exoddus.P2_WORDS_SHA, exoddus.EXE_SHA,
                      exoddus.DISC2_EXE_SHA, exoddus.MOVIE_SHA, exoddus.BIOS_SHA):
            self.assertRegex(value, '^[0-9a-f]{64}$')
        for stem, size in zip(exoddus.DISC_STEMS, exoddus.TRACK_BYTES):
            self.assertEqual(size % 2352, 0)
            self.assertIn(stem + '.cue', exoddus.SPEC['media'])
            self.assertIn(stem + '.bin', exoddus.SPEC['media'])
        self.assertEqual(exoddus.FRAMES, 478752)
        for boot, cnf in exoddus.SYSTEM_CNF.items():
            self.assertTrue(cnf.startswith(b'BOOT = cdrom:' + bytes([92]) + boot.encode() + b';1\r\n'))

    def test_one_swap_in_route_event_names(self):
        names = set(octoshock210_route.EVENT_NAMES.values())
        self.assertEqual([e['frame'] for e in exoddus.EVENTS], [179993, 179996, 179996])
        self.assertTrue({e['event'] for e in exoddus.EVENTS} <= names)
        self.assertEqual(exoddus.EVENTS[-1]['disc_index'], 2)

    def test_profile_leaves_the_ack_model_to_the_run(self):
        self.assertNotIn('--pad-ack-model', exoddus.PROFILE)
        self.assertNotIn('--card-model', exoddus.PROFILE)
        source = Path(run_native.__file__).read_text(encoding='utf-8')
        choices = re.search(r'"--pad-ack-model", choices=\(([^)]*)\)', source)[1]
        for model in exoddus.PAD_ACK_MODELS:
            self.assertIn(f'"{model}"', choices)
        self.assertNotIn('dualshock', ' '.join(exoddus.PAD_ACK_MODELS))

    def test_reference_with_another_schema_is_refused(self):
        with tempfile.TemporaryDirectory() as temp:
            ref = Path(temp) / 'ref.json'
            ref.write_text(json.dumps({'schema': 'crash-independent-source-v1', 'source_qualification': 'pass',
                                       'movie_sha256': exoddus.MOVIE_SHA, 'original_inputs': exoddus.FRAMES}))
            with self.assertRaises(ValueError):
                exoddus.verify_reference(ref)


if __name__ == '__main__':
    unittest.main()
