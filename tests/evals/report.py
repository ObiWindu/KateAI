#!/usr/bin/env python3
"""Turn a harness-eval report.json into a Markdown summary.

Usage: report.py tests/evals/out/report.json [--output report.md]
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("report", type=Path)
    parser.add_argument("--output", type=Path, default=None)
    args = parser.parse_args()

    try:
        data = json.loads(args.report.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        print(error, file=sys.stderr)
        return 1

    results = data.get("results", [])
    lines = [
        "# KateAI harness eval report",
        "",
        f"- Scenarios: {data.get('total', len(results))}",
        f"- Passed: {data.get('passed', sum(1 for r in results if r.get('passed')))}",
        f"- Failed: {data.get('failed', sum(1 for r in results if not r.get('passed')))}",
        "",
        "| Scenario | Result | Exit | Turns | Tool calls | Tokens in/out | Cost | Duration |",
        "| --- | --- | --- | --- | --- | --- | --- | --- |",
    ]
    for result in results:
        lines.append(
            f"| {result.get('id')} | {'PASS' if result.get('passed') else 'FAIL'} | "
            f"{result.get('exitCode')} | {result.get('modelRequests', 0)} | "
            f"{result.get('toolCalls', 0)} | "
            f"{result.get('inputTokens', 0)}/{result.get('outputTokens', 0)} | "
            f"${float(result.get('costUsd') or 0.0):.4f} | "
            f"{result.get('durationSeconds')}s |"
        )
    for result in results:
        lines.append("")
        lines.append(f"## {result.get('id')}")
        lines.append("")
        lines.append(result.get("description", ""))
        for assertion in result.get("assertions", []):
            marker = "PASS" if assertion.get("ok") else "FAIL"
            lines.append(f"- [{marker}] {assertion.get('name')}")
            detail = assertion.get("detail")
            if detail and not assertion.get("ok"):
                lines.append("  ```")
                lines.extend(f"  {line}" for line in str(detail).splitlines()[-20:])
                lines.append("  ```")
    text = "\n".join(lines) + "\n"
    if args.output:
        args.output.write_text(text, encoding="utf-8")
    else:
        print(text, end="")
    return 0


if __name__ == "__main__":
    sys.exit(main())
