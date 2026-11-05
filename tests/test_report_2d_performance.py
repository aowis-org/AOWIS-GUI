#!/usr/bin/env python3
"""Regression tests for the compact per-interval 2D rendering report."""
import importlib.util
import pathlib
import unittest

SCRIPT = pathlib.Path(__file__).resolve().parents[1] / "tools" / "report_2d_performance.py"
SPEC = importlib.util.spec_from_file_location("report_2d_performance", SCRIPT)
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


class ReportTests(unittest.TestCase):
    def test_parses_only_correct_diagnostic_kind(self):
        data = ('noise\n{"kind":"other"}\n'
                '{"kind":"aowis_map_render_diagnostics","elapsed_ms":5000,'
                '"counts":{"frames":250,"bgfx_present":251},'
                '"total_duration_ms":{"frames":4100,"bgfx_present":3000}}\n')
        snapshots = MODULE.diagnostic_snapshots(data)
        self.assertEqual(len(snapshots), 1)
        self.assertIn("50.0", MODULE.make_report(snapshots))
        self.assertIn("12.00", MODULE.make_report(snapshots))

    def test_interval_values_are_not_cumulative(self):
        base = {"kind": "aowis_map_render_diagnostics", "elapsed_ms":5000,
                "counts": {"frames": 200, "bgfx_present": 200, "zoom_change": 7},
                "total_duration_ms": {"frames": 3000, "bgfx_present": 2000,
                                      "bgfx_2d_imagery": 400}}
        next_one = {"kind": "aowis_map_render_diagnostics", "elapsed_ms":10000,
                    "counts": {"frames": 500, "bgfx_present": 500, "zoom_change": 20},
                    "total_duration_ms": {"frames": 7800, "bgfx_present": 6500,
                                          "bgfx_2d_imagery": 1000}}
        rows = list(MODULE.intervals([base, next_one]))
        self.assertEqual(rows[1]["fps"], 60.0)
        self.assertEqual(rows[1]["camera_changes"], 13)
        self.assertEqual(rows[1]["frame_ms"], 16.0)
        self.assertEqual(rows[1]["present_ms"], 15.0)
        self.assertEqual(rows[1]["imagery_ms"], 2.0)

    def test_older_log_explains_missing_phase_timers(self):
        old = {"kind": "aowis_map_render_diagnostics", "elapsed_ms":5000,
               "counts": {"frames": 100}}
        self.assertIn("predates detailed", MODULE.make_report([old]))

    def test_empty_and_unparsable_logs(self):
        self.assertEqual(MODULE.diagnostic_snapshots("not json\n{bad json"), [])
        self.assertIn("No AOWIS map diagnostics", MODULE.make_report([]))


if __name__ == "__main__":
    unittest.main()
