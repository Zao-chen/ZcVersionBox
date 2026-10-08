"""Summarize Qt JUnit reports without third-party Python dependencies."""

import os
from pathlib import Path
import sys
import xml.etree.ElementTree as ET


def summarize(directory):
    lines = ["## Test results", "", "| Suite | Tests | Failures | Skipped | Seconds |",
             "| --- | ---: | ---: | ---: | ---: |"]
    cases = []
    reports = sorted(Path(directory).glob("*.xml"))
    for report in reports:
        try:
            root = ET.parse(report).getroot()
        except ET.ParseError:
            lines.append(f"| {report.stem} | incomplete report | | | |")
            continue
        suites = [root] if root.tag == "testsuite" else root.findall("testsuite")
        for suite in suites:
            count = suite.get("tests", "0")
            failures = int(suite.get("failures", "0")) + int(suite.get("errors", "0"))
            lines.append(f"| {report.stem} | {count} | {failures} | "
                         f"{suite.get('skipped', '0')} | {suite.get('time', '0')} |")
            for case in suite.findall("testcase"):
                name = case.get("name", "")
                if name not in {"initTestCase", "cleanupTestCase"}:
                    cases.append((float(case.get("time", "0")), name, report.stem))
    if not reports:
        lines.append("| No reports (build or test startup did not complete) | | | | |")
    if cases:
        lines.extend(["", "### Slowest cases", "", "| Case | Suite | Seconds |",
                      "| --- | --- | ---: |"])
        for seconds, name, suite in sorted(cases, reverse=True)[:10]:
            name = name.replace("|", "\\|")
            lines.append(f"| {name} | {suite} | {seconds:.3f} |")
    return "\n".join(lines) + "\n"


if __name__ == "__main__":
    report = summarize(sys.argv[1])
    print(report)
    if destination := os.environ.get("GITHUB_STEP_SUMMARY"):
        with open(destination, "a", encoding="utf-8") as output:
            output.write(report)
