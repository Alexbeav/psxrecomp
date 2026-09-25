"""Negative controls for the deterministic seek comparison gate."""
import copy
import hashlib
import json
from pathlib import Path
import tempfile
import unittest
from validate_cd_seek_trace import validate


class SeekTraceValidation(unittest.TestCase):
    def test_corruption_and_changed_delay(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            matrix = root / "matrix.json"
            matrix.write_text(json.dumps({"cases": [{"id": "probe"}]}))
            rows = [{"metadata": {"matrix_sha256": hashlib.sha256(matrix.read_bytes()).hexdigest()}},
                    {"case_id": "probe", "cycles": 20000}]

            def save(name, values):
                path = root / name
                path.write_text("".join(json.dumps(value) + "\n" for value in values))
                return path

            good = save("good.jsonl", rows)
            self.assertEqual(validate(matrix, [good, good])["different_results"], 0)
            corrupt = [rows[:-1], rows + [rows[-1]]]
            for key, value in [("case_id", "wrong"), ("cycles", -1), ("cycles", True),
                               ("cycles", 2147483648)]:
                modified = copy.deepcopy(rows)
                modified[1][key] = value
                corrupt.append(modified)
            for modified in corrupt:
                with self.assertRaises(ValueError):
                    validate(matrix, [save("bad.jsonl", modified)])
            modified = copy.deepcopy(rows)
            modified[1]["cycles"] += 1
            result = validate(matrix, [good, save("changed.jsonl", modified)])
            self.assertEqual(result["different_results"], 1)


if __name__ == "__main__":
    unittest.main()
