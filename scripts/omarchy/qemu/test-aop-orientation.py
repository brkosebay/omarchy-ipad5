#!/usr/bin/env python3
"""Offline signed-axis derivation: synthetic poses, no SSH or device access."""
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest

spec = importlib.util.spec_from_file_location("orientation", Path(__file__).parents[1] / "aop-orientation.py")
orientation = importlib.util.module_from_spec(spec)
spec.loader.exec_module(orientation)


class PoseTests(unittest.TestCase):
    def fixtures(self, directory, sensor_from_screen):
        for pose, screen in orientation.TARGETS.items():
            raw = [round(sum(row[i] * screen[i] for i in range(3)) / 0.000149638)
                   for row in sensor_from_screen]
            data = {"schema": 1, "name": "apple-a9-aop-accel", "pose": pose,
                    "boot_id": "same-test-boot", "scale": 0.000149638,
                    "samples": [{"time_ns": i*100000000, "raw": raw} for i in range(30)]}
            (directory / f"{pose}.json").write_text(json.dumps(data))

    def test_candidate_and_non_symmetric_mapping(self):
        for sensor, expected in [
            ([[-1, 0, 0], [0, -1, 0], [0, 0, -1]], [[-1, 0, 0], [0, -1, 0], [0, 0, -1]]),
            ([[0, 1, 0], [-1, 0, 0], [0, 0, 1]], [[0, -1, 0], [1, 0, 0], [0, 0, 1]]),
        ]:
            with tempfile.TemporaryDirectory() as tmp:
                self.fixtures(Path(tmp), sensor)
                self.assertEqual(orientation.derive(Path(tmp))["matrix"], expected)

    def test_mislabeled_pose_rejected(self):
        with tempfile.TemporaryDirectory() as tmp:
            p = Path(tmp)
            self.fixtures(p, [[1, 0, 0], [0, 1, 0], [0, 0, 1]])
            data = json.loads((p / 'portrait.json').read_text())
            data['pose'] = 'portrait-flipped'
            (p / 'portrait-flipped.json').write_text(json.dumps(data))
            with self.assertRaises(AssertionError):
                orientation.derive(p)

    def test_moving_or_different_boot_rejected(self):
        for change in ('motion', 'boot'):
            with tempfile.TemporaryDirectory() as tmp:
                p = Path(tmp)
                self.fixtures(p, [[1, 0, 0], [0, 1, 0], [0, 0, 1]])
                data = json.loads((p / 'screen-up.json').read_text())
                if change == 'boot':
                    data['boot_id'] = 'other-boot'
                else:
                    data['samples'][0]['raw'][0] = 655360
                (p / 'screen-up.json').write_text(json.dumps(data))
                with self.assertRaises(AssertionError):
                    orientation.derive(p)


if __name__ == '__main__':
    unittest.main()
