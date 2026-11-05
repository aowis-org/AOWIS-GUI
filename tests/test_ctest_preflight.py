#!/usr/bin/env python3
"""Test the concise CTest preflight without Qt/bgfx dependencies."""

import importlib.util
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / "tools" / "ctest_preflight.py"
spec = importlib.util.spec_from_file_location("ctest_preflight", SCRIPT)
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


class PreflightTests(unittest.TestCase):
    def test_present_and_missing_executables(self):
        count, missing = module.check_registration({"tests": [
            {"name": "present", "command": ["/tmp/present"]},
            {"name": "not-built"},
            {"name": "also-not-built", "command": []},
        ]})
        self.assertEqual(count, 3)
        self.assertEqual(missing, ["not-built", "also-not-built"])

    def test_empty_selection(self):
        self.assertEqual(module.check_registration({"tests": []}), (0, []))

    def test_actual_ctest_json_does_not_dump_missing_executable_paths(self):
        # Real CTest, isolated source and build directory, no Qt needed.
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory)
            (source / "CMakeLists.txt").write_text(
                "cmake_minimum_required(VERSION 3.21)\n"
                "project(aowis_preflight_fixture LANGUAGES C)\n"
                "include(CTest)\n"
                "add_executable(aowis-sample-tests sample.c)\n"
                "add_test(NAME aowis-sample COMMAND aowis-sample-tests)\n"
                "set_tests_properties(aowis-sample PROPERTIES LABELS aowis)\n"
            )
            (source / "sample.c").write_text("int main(void) { return 0; }\n")
            build = source / "build"
            subprocess.run(["cmake", "-S", str(source), "-B", str(build),
                            "-G", "Ninja"], check=True, capture_output=True, text=True)
            missing_result = subprocess.run(
                [sys.executable, str(SCRIPT), str(build)], capture_output=True, text=True)
            self.assertEqual(missing_result.returncode, 1)
            self.assertIn("1 of 1 AOWIS tests", missing_result.stderr)
            self.assertIn("aowis-sample", missing_result.stderr)
            self.assertNotIn("Looked in the following places", missing_result.stderr)
            subprocess.run(["cmake", "--build", str(build)], check=True,
                           capture_output=True, text=True)
            passed_result = subprocess.run(
                [sys.executable, str(SCRIPT), str(build)], capture_output=True, text=True)
            self.assertEqual(passed_result.returncode, 0, passed_result.stderr)
            self.assertIn("1 registered; all executables available", passed_result.stdout)

    def test_linux_runner_uses_compact_ctest_output(self):
        runner = (ROOT / "compile_linux_tests.sh").read_text()
        self.assertIn('tools/ctest_preflight.py', runner)
        self.assertIn('--output-on-failure', runner)
        self.assertNotIn('--verbose', runner)
        self.assertNotIn('ctest --test-dir "${build_dir}" -N', runner)

    def test_build_target_is_derived_from_registered_executables(self):
        cmake = (ROOT / "CMakeLists.txt").read_text()
        self.assertIn('DIRECTORY PROPERTY BUILDSYSTEM_TARGETS', cmake)
        self.assertIn('^aowis-.*-(tests|benchmark)$', cmake)
        self.assertIn('add_custom_target(aowis-tests DEPENDS ${_aowis_test_executables})', cmake)
        self.assertNotIn('aowis-render-core-link-tests\n            aowis-', cmake)


if __name__ == "__main__":
    unittest.main(verbosity=2)
