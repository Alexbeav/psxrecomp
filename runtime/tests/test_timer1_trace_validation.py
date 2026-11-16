"""Ensure Timer1 evidence validation rejects incomplete or altered results."""
import copy
import hashlib
import json
from pathlib import Path
import tempfile
import unittest
from validate_timer1_trace import validate


class TraceValidation(unittest.TestCase):
    def test_negative_controls(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            matrix = root / "matrix.json"
            matrix.write_text(json.dumps({"cases": [{"id": "case", "operations": [
                {"op": "write", "reg": 4, "value": 16}, {"op": "read", "reg": 4}]}]}))
            base = dict(case_id="case", event_index=-1, kind="reset", cycle=0,
                        counter=0, target=0, read=-1, accepted=-1)
            rows = [{"metadata": {"matrix_sha256": hashlib.sha256(matrix.read_bytes()).hexdigest()}},
                    base, dict(base, event_index=0, kind="write", accepted=0),
                    dict(base, event_index=1, kind="read", read=0)]

            def save(name, values):
                path = root / name
                path.write_text("".join(json.dumps(v) + "\n" for v in values))
                return path

            good = save("good.jsonl", rows)
            self.assertEqual(validate(matrix, [good, good])["different_fields"], {})
            corrupt = [rows[:-1], rows + [rows[-1]], rows[:2] + rows[3:]]
            for key, value in [("accepted", -1), ("counter", True), ("cycle", 1), ("read", 0)]:
                modified = copy.deepcopy(rows)
                modified[2][key] = value
                corrupt.append(modified)
            for modified in corrupt:
                with self.assertRaises(ValueError):
                    validate(matrix, [save("bad.jsonl", modified)])
            for key in ["counter", "target", "read", "accepted"]:
                modified = copy.deepcopy(rows)
                modified[3 if key == "read" else 2][key] += 1
                result = validate(matrix, [good, save("changed.jsonl", modified)])
                self.assertEqual(result["different_fields"], {key: 1})


if __name__ == "__main__":
    unittest.main()
