#!/usr/bin/env python3
"""Scenario runner for the KateAI harness eval suite.

The runner copies a fixture, starts a scripted OpenAI-compatible provider, runs
the real `kateai-headless` binary against it, and checks files, events, exit
codes and verification evidence. It has no third-party dependencies and never
calls a live model.

Exit codes: 0 all scenarios passed, 1 a scenario failed, 2 a harness error.
"""

from __future__ import annotations

import argparse
import json
import os
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path

EVAL_ROOT = Path(__file__).resolve().parent
REPO_ROOT = EVAL_ROOT.parents[1]


def default_headless() -> Path:
    candidates = [
        REPO_ROOT / "build" / "bin" / "kateai-headless",
        REPO_ROOT / "build" / "src" / "kateai-headless",
    ]
    for candidate in candidates:
        if candidate.exists():
            return candidate
    return candidates[0]


def find_scenarios(suite: str) -> list[Path]:
    scenarios: list[Path] = []
    for path in sorted((EVAL_ROOT / "scenarios").rglob("*.json")):
        if suite == "all" or path.parent.name == suite:
            scenarios.append(path)
    return scenarios


def read_events(output_dir: Path) -> list[dict]:
    events_path = output_dir / "events.jsonl"
    if not events_path.exists():
        return []
    events = []
    with events_path.open("r", encoding="utf-8") as handle:
        for line in handle:
            line = line.strip()
            if not line:
                continue
            try:
                events.append(json.loads(line))
            except json.JSONDecodeError:
                events.append({"type": "_invalid", "raw": line})
    return events


def read_summary(output_dir: Path) -> dict:
    path = output_dir / "summary.json"
    if not path.exists():
        return {}
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except json.JSONDecodeError:
        return {}


def is_subsequence(needle: list[str], haystack: list[str]) -> bool:
    position = 0
    for item in haystack:
        if position < len(needle) and item == needle[position]:
            position += 1
    return position == len(needle)


def check_scenario(scenario: dict, headless: Path, work_root: Path, verbose: bool) -> dict:
    case_id = scenario.get("id") or "unnamed"
    fixture = EVAL_ROOT / "fixtures" / scenario.get("fixture", "empty")
    result = {
        "id": case_id,
        "description": scenario.get("description", ""),
        "passed": False,
        "assertions": [],
        "exitCode": None,
        "durationSeconds": 0.0,
    }
    if not fixture.exists():
        result["assertions"].append(
            {"name": "fixture", "ok": False, "detail": f"missing fixture {fixture}"}
        )
        return result

    case_dir = Path(tempfile.mkdtemp(prefix=f"{case_id}-", dir=str(work_root)))
    workspace = case_dir / "workspace"
    output_dir = case_dir / "output"
    home = case_dir / "home"
    shutil.copytree(fixture, workspace)
    if scenario.get("git", False):
        subprocess.run(["git", "init", "-q"], cwd=str(workspace), check=False)
        subprocess.run(["git", "add", "-A"], cwd=str(workspace), check=False)
        subprocess.run(
            [
                "git",
                "-c",
                "user.name=KateAI Eval",
                "-c",
                "user.email=eval@kateai.invalid",
                "commit",
                "-qm",
                "fixture",
            ],
            cwd=str(workspace),
            check=False,
        )
    output_dir.mkdir()
    home.mkdir()

    script_path = case_dir / "script.json"
    script_path.write_text(json.dumps({"responses": scenario.get("script", [])}), encoding="utf-8")
    port_file = case_dir / "port"
    provider = subprocess.Popen(
        [
            sys.executable,
            str(EVAL_ROOT / "scripted_provider.py"),
            "--script",
            str(script_path),
            "--port-file",
            str(port_file),
        ],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    port = None
    for _ in range(200):
        if port_file.exists():
            text = port_file.read_text(encoding="utf-8").strip()
            if text:
                port = int(text)
                break
        if provider.poll() is not None:
            break
        time.sleep(0.025)
    if port is None:
        provider.kill()
        result["assertions"].append(
            {"name": "provider", "ok": False, "detail": "scripted provider did not start"}
        )
        return result

    env = dict(os.environ)
    env.update(
        {
            "HOME": str(home),
            "XDG_CONFIG_HOME": str(home / "config"),
            "XDG_DATA_HOME": str(home / "data"),
            "KATEAI_SESSION_DB": str(home / "sessions.sqlite"),
            "KATEAI_HARNESS_FLAGS": "+eventLog",
        }
    )
    env.update({key: str(value) for key, value in (scenario.get("env") or {}).items()})

    command = [
        str(headless),
        "run",
        "--workspace",
        str(workspace),
        "--output-dir",
        str(output_dir),
        "--prompt",
        scenario.get("prompt", "Do the task."),
        "--provider",
        "openai-compatible",
        "--base-url",
        f"http://127.0.0.1:{port}/v1",
        "--model",
        "scripted",
        "--api-key",
        "eval-key",
        "--permission",
        scenario.get("permission", "auto"),
        "--sandbox",
        scenario.get("sandbox", "workspace"),
        "--no-persist",
    ]
    if not scenario.get("verify", True):
        command.append("--no-verify")
    if scenario.get("jsonl", False):
        command.append("--jsonl")

    started = time.time()
    timed_out = False
    try:
        completed = subprocess.run(
            command,
            cwd=str(workspace),
            env=env,
            capture_output=True,
            text=True,
            timeout=scenario.get("timeout_seconds", 120),
        )
        exit_code = completed.returncode
        stdout = completed.stdout
        stderr = completed.stderr
    except subprocess.TimeoutExpired as timeout:
        timed_out = True
        exit_code = -1
        stdout = (timeout.stdout or b"").decode(errors="replace") if isinstance(timeout.stdout, bytes) else (timeout.stdout or "")
        stderr = (timeout.stderr or b"").decode(errors="replace") if isinstance(timeout.stderr, bytes) else (timeout.stderr or "")
    finally:
        provider.terminate()
        try:
            provider.wait(timeout=5)
        except subprocess.TimeoutExpired:
            provider.kill()
    result["durationSeconds"] = round(time.time() - started, 3)
    result["exitCode"] = exit_code
    result["stdout"] = stdout[-4000:]
    result["stderr"] = stderr[-8000:]

    events = read_events(output_dir)
    summary = read_summary(output_dir)
    event_types = [event.get("type") for event in events]
    result["costUsd"] = float(summary.get("costUsd") or 0.0)
    result["inputTokens"] = int(summary.get("inputTokens") or 0)
    result["outputTokens"] = int(summary.get("outputTokens") or 0)
    result["modelRequests"] = int(summary.get("modelRequests") or 0)
    result["toolCalls"] = int(summary.get("toolCalls") or 0)
    assertions = scenario.get("assertions") or {}

    def add(name: str, ok: bool, detail: str = "") -> None:
        result["assertions"].append({"name": name, "ok": ok, "detail": detail})

    if timed_out:
        add("timeout", False, "the headless process exceeded its timeout")

    expected_exit = assertions.get("exit_code", 0)
    add(
        "exit_code",
        exit_code == expected_exit,
        f"expected {expected_exit}, got {exit_code}\n{stderr[-2000:]}",
    )

    for relative, needle in (assertions.get("files") or {}).items():
        path = workspace / relative
        if not path.exists():
            add(f"file {relative}", False, "file does not exist")
            continue
        content = path.read_text(encoding="utf-8", errors="replace")
        add(f"file {relative}", needle in content, f"missing {needle!r}")

    for relative in assertions.get("file_absent") or []:
        path = workspace / relative
        add(f"absent {relative}", not path.exists(), f"{path} exists")

    for relative in assertions.get("outside_absent") or []:
        path = (workspace / relative).resolve()
        add(f"outside absent {relative}", not path.exists(), f"{path} exists")

    for expected_type in assertions.get("events_include") or []:
        add(f"event {expected_type}", expected_type in event_types, "event not found")

    order = assertions.get("events_order") or []
    if order:
        add("event order", is_subsequence(order, event_types), " | ".join(event_types))

    tool_calls = sum(1 for item in event_types if item == "tool/call")
    if "max_tool_calls" in assertions:
        limit = int(assertions["max_tool_calls"])
        add("max_tool_calls", tool_calls <= limit, f"{tool_calls} > {limit}")

    if assertions.get("verification_passed"):
        verification = [
            event
            for event in events
            if event.get("type") == "verification"
            and (event.get("payload") or {}).get("passed") is True
        ]
        add("verification_passed", bool(verification), "no passing verification event")

    if assertions.get("tool_failed"):
        failures = [
            event
            for event in events
            if event.get("type") == "tool/result"
            and (event.get("payload") or {}).get("ok") is False
        ]
        add("tool_failed", bool(failures), "no failed tool result")

    if "max_cost_usd" in assertions:
        cost = float(summary.get("costUsd") or 0.0)
        add("max_cost_usd", cost <= float(assertions["max_cost_usd"]), f"{cost}")

    for needle in assertions.get("stdout_contains") or []:
        add(f"stdout {needle!r}", needle in stdout, stdout[-1000:])

    patch_path = output_dir / "patch.diff"
    if assertions.get("patch_contains"):
        text = patch_path.read_text(encoding="utf-8", errors="replace") if patch_path.exists() else ""
        for needle in assertions["patch_contains"]:
            add(f"patch {needle!r}", needle in text, text[-1000:])

    if verbose and stderr.strip():
        print(stderr[-4000:], file=sys.stderr)

    result["passed"] = all(item["ok"] for item in result["assertions"])
    if not result["passed"] or verbose:
        result["events"] = event_types
        result["toolCalls"] = tool_calls
        result["workspace"] = str(workspace)
    if scenario.get("keep", False) or os.environ.get("KATEAI_EVAL_KEEP"):
        result["keptAt"] = str(case_dir)
    else:
        shutil.rmtree(case_dir, ignore_errors=True)
    return result


def write_reports(results: list[dict], output: Path) -> None:
    output.mkdir(parents=True, exist_ok=True)
    report = {
        "total": len(results),
        "passed": sum(1 for result in results if result["passed"]),
        "failed": sum(1 for result in results if not result["passed"]),
        "results": results,
    }
    (output / "report.json").write_text(
        json.dumps(report, indent=2, sort_keys=True), encoding="utf-8"
    )
    lines = [
        "# KateAI harness eval report",
        "",
        f"- Scenarios: {report['total']}",
        f"- Passed: {report['passed']}",
        f"- Failed: {report['failed']}",
        "",
        "| Scenario | Result | Exit | Turns | Tool calls | Tokens in/out | Cost | Duration |",
        "| --- | --- | --- | --- | --- | --- | --- | --- |",
    ]
    for result in results:
        lines.append(
            f"| {result['id']} | {'PASS' if result['passed'] else 'FAIL'} | "
            f"{result['exitCode']} | {result.get('modelRequests', 0)} | "
            f"{result.get('toolCalls', 0)} | "
            f"{result.get('inputTokens', 0)}/{result.get('outputTokens', 0)} | "
            f"${result.get('costUsd', 0.0):.4f} | {result['durationSeconds']}s |"
        )
    for result in results:
        lines.append("")
        lines.append(f"## {result['id']}")
        lines.append("")
        lines.append(result.get("description", ""))
        for assertion in result["assertions"]:
            marker = "PASS" if assertion["ok"] else "FAIL"
            detail = assertion.get("detail", "")
            lines.append(f"- [{marker}] {assertion['name']}")
            if detail and not assertion["ok"]:
                lines.append("  ```")
                lines.extend(f"  {line}" for line in str(detail).splitlines()[-20:])
                lines.append("  ```")
    (output / "report.md").write_text("\n".join(lines) + "\n", encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--suite", default="smoke", choices=["smoke", "injection", "all"])
    parser.add_argument("--headless", type=Path, default=default_headless())
    parser.add_argument("--output", type=Path, default=EVAL_ROOT / "out")
    parser.add_argument("--scenario", type=Path, default=None)
    parser.add_argument("--list", action="store_true")
    parser.add_argument("--verbose", action="store_true")
    args = parser.parse_args()

    scenarios = [args.scenario] if args.scenario else find_scenarios(args.suite)
    if args.list:
        for path in scenarios:
            print(path.relative_to(EVAL_ROOT))
        return 0
    if not scenarios:
        print(f"No scenarios found for suite {args.suite!r}.", file=sys.stderr)
        return 2
    if not args.headless.exists():
        print(f"Headless binary not found: {args.headless}", file=sys.stderr)
        print("Build it with: cmake --build build --target kateai-headless", file=sys.stderr)
        return 2

    work_root = Path(tempfile.mkdtemp(prefix="kateai-evals-"))
    results = []
    try:
        for path in scenarios:
            try:
                scenario = json.loads(path.read_text(encoding="utf-8"))
            except (OSError, json.JSONDecodeError) as error:
                results.append(
                    {
                        "id": path.stem,
                        "description": str(error),
                        "passed": False,
                        "assertions": [
                            {"name": "load", "ok": False, "detail": str(error)}
                        ],
                        "exitCode": None,
                        "durationSeconds": 0.0,
                    }
                )
                continue
            result = check_scenario(scenario, args.headless, work_root, args.verbose)
            results.append(result)
            print(
                f"{'PASS' if result['passed'] else 'FAIL'} {result['id']} "
                f"({result['durationSeconds']}s)"
            )
    finally:
        shutil.rmtree(work_root, ignore_errors=True)

    write_reports(results, args.output)
    failed = [result for result in results if not result["passed"]]
    print(f"{len(results) - len(failed)}/{len(results)} scenarios passed")
    print(f"Report: {args.output / 'report.md'}")
    return 0 if not failed else 1


if __name__ == "__main__":
    sys.exit(main())
