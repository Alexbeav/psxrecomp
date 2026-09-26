"""Staged-catalog developer-channel filter: dependent entries leave with their feature."""
from pathlib import Path
import sys
import tomllib
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import mod_channel_filter as channel_filter

MANIFEST = '''format_version = 7
id = "example.mode"
version = "1.0.0"
name = "Mode"

[[target]]
game_id = "TEST"

[[feature]]
id = "mode"
name = "Mode"

[[feature]]
id = "probe"
name = "Probe"
channel = "developer"

[[option]]
feature = "mode"
id = "extras"
label = "Extras"
type = "choice"
default = "none"

[[option.choice]]
value = "none"
label = "None"

[[option.choice]]
value = "full"
label = "Full"

[[requirement]]
feature = "mode"
package = "psx.enhancement.8mb-ram"
requires_feature = "8mb-ram"
when = { extras = "full" }

[[requirement]]
feature = "probe"
package = "example.trace"
requires_feature = "trace"
'''


class ChannelFilterTest(unittest.TestCase):
    def test_developer_feature_takes_its_requirements_with_it(self):
        filtered, dropped = channel_filter.filter_manifest(MANIFEST)
        self.assertEqual(dropped, ['probe'])
        parsed = tomllib.loads(filtered)
        self.assertEqual([f['id'] for f in parsed['feature']], ['mode'])
        # The shipped feature keeps its requirement; the developer feature's
        # requirement does not survive to name a package a release lacks.
        self.assertEqual(parsed['requirement'], [dict(
            feature='mode', package='psx.enhancement.8mb-ram',
            requires_feature='8mb-ram', when=dict(extras='full'))])

    def test_no_developer_feature_leaves_the_manifest_untouched(self):
        text = MANIFEST.replace('channel = "developer"\n', '')
        filtered, dropped = channel_filter.filter_manifest(text)
        self.assertEqual(dropped, [])
        self.assertEqual(filtered, text)


if __name__ == '__main__':
    unittest.main()
