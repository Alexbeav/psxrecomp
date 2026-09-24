"""T212: the toolchain pack is fetched by pinned tag and verified by SHA-256.

  python -m unittest discover -s tools/tests -p "test_toolchain_pack_pin.py"
"""
import hashlib
import os
import re
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import toolchain_pack as tp  # noqa: E402

SH = Path(__file__).resolve().parents[1] / "fetch_toolchain.sh"


class PinnedDownload(unittest.TestCase):
    def setUp(self):
        self.env = mock.patch.dict(os.environ, {}, clear=False)
        self.env.start()
        for k in ("RETCOMM_TOOLCHAIN_TAG", "RETCOMM_TOOLCHAIN_SHA256"):
            os.environ.pop(k, None)

    def tearDown(self):
        self.env.stop()

    def test_default_is_the_pinned_tag_never_latest(self):
        for asset in set(tp._ASSET.values()):
            url, sha = tp.pinned_download(asset)
            self.assertIn("/releases/download/%s/%s" % (tp.PINNED_TAG, asset), url)
            self.assertNotIn("latest", url)
            self.assertEqual(sha, tp.PINNED_SHA256[asset])

    def test_every_asset_has_a_pinned_digest(self):
        self.assertEqual(set(tp._ASSET.values()), set(tp.PINNED_SHA256))
        for sha in tp.PINNED_SHA256.values():
            self.assertRegex(sha, r"^[0-9a-f]{64}$")

    def test_overridden_tag_without_digest_is_refused(self):
        os.environ["RETCOMM_TOOLCHAIN_TAG"] = "v9.9.9"
        with self.assertRaises(RuntimeError):
            tp.pinned_download("cmake-clang-v1-linux-x64.zip")

    def test_overridden_tag_with_digest_is_used(self):
        os.environ["RETCOMM_TOOLCHAIN_TAG"] = "v9.9.9"
        os.environ["RETCOMM_TOOLCHAIN_SHA256"] = "A" * 64
        url, sha = tp.pinned_download("cmake-clang-v1-linux-x64.zip")
        self.assertIn("/releases/download/v9.9.9/", url)
        self.assertEqual(sha, "a" * 64)

    def test_download_with_wrong_bytes_is_refused_before_install(self):
        def fake_download(url, dest, token=None):
            Path(dest).write_bytes(b"not the pinned pack")
        with mock.patch.object(tp, "download_url", fake_download), \
             mock.patch.object(tp, "install_from_zip") as install:
            with self.assertRaisesRegex(RuntimeError, "SHA-256 mismatch"):
                tp.download_latest_pack(artifact="linux-x64")
            install.assert_not_called()

    def test_download_with_matching_bytes_is_installed(self):
        payload = b"pinned pack bytes"
        digest = hashlib.sha256(payload).hexdigest()
        os.environ["RETCOMM_TOOLCHAIN_SHA256"] = digest

        def fake_download(url, dest, token=None):
            Path(dest).write_bytes(payload)
        with mock.patch.object(tp, "download_url", fake_download), \
             mock.patch.object(tp, "install_from_zip", return_value=Path("x")) as install:
            tp.download_latest_pack(artifact="windows-x64")
            install.assert_called_once()

    def test_shell_fetcher_pins_the_same_tag_and_digests(self):
        text = SH.read_text(encoding="utf-8")
        self.assertNotIn("releases/latest", text)
        self.assertIn("PINNED_TAG='%s'" % tp.PINNED_TAG, text)
        for sha in tp.PINNED_SHA256.values():
            self.assertIn(sha, text)


if __name__ == "__main__":
    unittest.main()
