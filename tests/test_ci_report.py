import importlib.util
from pathlib import Path
import tempfile
import unittest

spec = importlib.util.spec_from_file_location(
    "ci_report", Path(__file__).resolve().parents[1] / "scripts/ci_report.py")
ci_report = importlib.util.module_from_spec(spec)
spec.loader.exec_module(ci_report)


class CiReportTests(unittest.TestCase):
    def test_suite_results_and_case_timings(self):
        with tempfile.TemporaryDirectory() as directory:
            Path(directory, "regression.xml").write_text(
                '<testsuite tests="3" failures="1" errors="1" skipped="1" time="7.2">'
                '<testcase name="initTestCase" time="20"/>'
                '<testcase name="switch" time="7.2"/>'
                '<testcase name="preview" time="0.1"/></testsuite>')
            result = ci_report.summarize(directory)
            self.assertIn("| regression | 3 | 2 | 1 | 7.2 |", result)
            self.assertIn("| switch | regression | 7.200 |", result)
            self.assertNotIn("initTestCase", result)
            self.assertLess(result.index("| switch |"), result.index("| preview |"))

    def test_junit_testsuites_root_and_empty_directory(self):
        with tempfile.TemporaryDirectory() as directory:
            self.assertIn("No reports", ci_report.summarize(directory))
            Path(directory, "update_core.xml").write_text(
                '<testsuites><testsuite tests="1" failures="0" time="0.01">'
                '<testcase name="versions" time="0.01"/></testsuite></testsuites>')
            self.assertIn("| update_core | 1 | 0 | 0 | 0.01 |", ci_report.summarize(directory))

    def test_interrupted_report_keeps_other_results(self):
        with tempfile.TemporaryDirectory() as directory:
            Path(directory, "backup_core.xml").write_text('<testsuite>')
            Path(directory, "update_core.xml").write_text('<testsuite tests="1"/>')
            result = ci_report.summarize(directory)
            self.assertIn("backup_core | incomplete report", result)
            self.assertIn("update_core | 1", result)


if __name__ == "__main__":
    unittest.main()
