"""Negative controls for trace identity, completeness, and field comparison."""
import copy
import hashlib
import json
from pathlib import Path
import tempfile
import unittest

from validate_timer2_trace import validate


class TraceValidation(unittest.TestCase):
    def test_reject_corruption_and_detect_every_changed_field(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            matrix = root / "matrix.json"
            matrix.write_text(json.dumps({"cases": [{"id": "example", "operations": [
                {"op": "advance", "cycles": 7}, {"op": "read", "reg": 4}]}]}))
            metadata = {"metadata": {"matrix_sha256": hashlib.sha256(matrix.read_bytes()).hexdigest()}}
            reset = dict(case_id="example", event_index=-1, kind="reset", cycle=0,
                         counter=0, target=0, pulses=0, read=-1, next=-1)
            advance = dict(reset, event_index=0, kind="advance", cycle=7, next=1024)
            read = dict(reset, event_index=1, kind="read", cycle=7, read=0)
            rows = [metadata, reset, advance, read]

            def save(name, values):
                path = root / name
                path.write_text("".join(json.dumps(value) + "\n" for value in values))
                return path

            good = save("good.jsonl", rows)
            self.assertEqual(validate(matrix, [good, good])["different_fields"], {})
            malformed = [rows[:-1], rows + [read], rows[:2] + [reset] + rows[2:],
                         [dict(metadata={"matrix_sha256": "wrong"})] + rows[1:]]
            for field, value in [("cycle", 8), ("event_index", 3), ("counter", -2),
                                 ("pulses", True), ("read", 0), ("next", -1)]:
                changed = copy.deepcopy(rows)
                changed[2][field] = value
                malformed.append(changed)
            for index, changed in enumerate(malformed):
                with self.subTest(corruption=index), self.assertRaises(ValueError):
                    validate(matrix, [save("bad.jsonl", changed)])
            for field in ["counter", "target", "pulses", "read", "next"]:
                changed = copy.deepcopy(rows)
                index = 3 if field == "read" else 2
                changed[index][field] += 1
                result = validate(matrix, [good, save("changed.jsonl", changed)])
                self.assertEqual(result["different_fields"], {field: 1})


if __name__ == "__main__":
    unittest.main()
