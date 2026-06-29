from __future__ import annotations

import argparse
import json
import shutil
import subprocess
import sys
import time
from collections import Counter
from datetime import datetime
from pathlib import Path
from typing import Any


SCRIPT_DIR = Path(__file__).resolve().parent
REPO_ROOT = SCRIPT_DIR.parents[1]
TASKS_DIR = SCRIPT_DIR / "tasks"
EXPECTED_DIR = SCRIPT_DIR / "expected"
RESULTS_DIR = SCRIPT_DIR / "results"


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Run codetopo query benchmarks.")
    parser.add_argument("--task", help="Task id or task JSON path.")
    parser.add_argument("--binary", help="Path to codetopo binary.")
    parser.add_argument(
        "--update-expected",
        action="store_true",
        help="Refresh benchmark/query/expected/*.json snapshots with current output.",
    )
    parser.add_argument("--no-write-results", action="store_true", help="Skip writing report files.")
    return parser.parse_args()


def resolve_binary(explicit: str | None) -> str:
    candidates = []
    if explicit:
        candidates.append(Path(explicit))
    build_binary = REPO_ROOT / "build" / "codetopo"
    if build_binary.exists():
        candidates.append(build_binary)
    path_binary = shutil.which("codetopo")
    if path_binary:
        candidates.append(Path(path_binary))
    for candidate in candidates:
        if candidate.exists():
            return str(candidate)
    raise FileNotFoundError("Unable to find codetopo binary. Use --binary or build ./build/codetopo.")


def load_task_paths(task_arg: str | None) -> list[Path]:
    if not task_arg:
        return sorted(TASKS_DIR.glob("*.json"))
    candidate = Path(task_arg)
    if candidate.exists():
        return [candidate.resolve()]
    task_path = TASKS_DIR / f"{task_arg}.json"
    if task_path.exists():
        return [task_path]
    raise FileNotFoundError(f"Unknown task: {task_arg}")


def load_json(path: Path) -> dict[str, Any]:
    return json.loads(path.read_text(encoding="utf-8"))


def run_tool(binary: str, task: dict[str, Any]) -> tuple[dict[str, Any], float, list[str], list[str]]:
    command = [
        binary,
        "query",
        "--root",
        task["repo_root"],
        task["tool"],
        json.dumps(task["params"], separators=(",", ":")),
    ]
    started = time.perf_counter()
    completed = subprocess.run(command, capture_output=True, text=True, cwd=REPO_ROOT)
    duration = time.perf_counter() - started

    stdout_lines = [line for line in completed.stdout.splitlines() if line.strip()]
    stderr_lines = [line for line in completed.stderr.splitlines() if line.strip()]
    if completed.returncode != 0:
        raise RuntimeError(
            f"{task['id']} failed with exit code {completed.returncode}: "
            + (" | ".join(stderr_lines) if stderr_lines else "no stderr")
        )
    try:
        payload = json.loads(completed.stdout)
    except json.JSONDecodeError as exc:
        raise RuntimeError(f"{task['id']} returned non-JSON output") from exc
    return payload, duration, command, stderr_lines


def evaluate_task(task: dict[str, Any], payload: dict[str, Any], duration: float) -> dict[str, Any]:
    category = task["category"]
    expected = task.get("expected", {})
    observed: dict[str, Any]
    checks: list[dict[str, Any]]
    summary: str

    if category == "enumeration":
        observed, checks, summary = evaluate_enumeration(payload, expected)
    elif category == "callers":
        observed, checks, summary = evaluate_callers(payload, expected)
    elif category == "method_internals":
        observed, checks, summary = evaluate_method_fields(payload, expected)
    elif category == "field_access":
        observed, checks, summary = evaluate_field_access(payload, expected, duration)
    elif category == "cross_root":
        observed, checks, summary = evaluate_cross_root(payload, expected)
    else:
        raise ValueError(f"Unsupported category: {category}")

    reference_path = EXPECTED_DIR / f"{task['id']}.json"
    reference_exists = reference_path.exists()
    reference_match = None
    if reference_exists:
        reference_match = canonical_json(load_json(reference_path)) == canonical_json(payload)

    failed_checks = [check for check in checks if not check["passed"]]
    if failed_checks:
        status = "FAIL"
    elif task.get("expected_status") == "warn":
        status = "WARN"
    else:
        status = "PASS"

    return {
        "task": task,
        "status": status,
        "wall_time_seconds": round(duration, 6),
        "summary": summary,
        "observed": observed,
        "checks": checks,
        "reference_snapshot": {
            "path": str(reference_path.relative_to(REPO_ROOT)),
            "exists": reference_exists,
            "matches": reference_match,
        },
        "payload": payload,
    }


def evaluate_enumeration(payload: dict[str, Any], expected: dict[str, Any]) -> tuple[dict[str, Any], list[dict[str, Any]], str]:
    results = payload.get("results", [])
    counts = Counter(item.get("kind", "unknown") for item in results)
    names = [item.get("name") for item in results if item.get("name")]
    observed = {
        "total": len(results),
        "kind_distribution": dict(counts),
        "names": names,
    }
    checks = [
        check_range("total", len(results), expected.get("min_total"), expected.get("max_total")),
    ]
    for kind, bounds in expected.get("kind_distribution", {}).items():
        checks.append(check_range(f"kind:{kind}", counts.get(kind, 0), bounds.get("min"), bounds.get("max")))
    for name in expected.get("must_contain_names", []):
        checks.append(check_contains("must_contain_names", name, set(names)))
    for name in expected.get("must_not_contain_names", []):
        checks.append(check_not_contains("must_not_contain_names", name, set(names)))
    summary = f"total={len(results)} kinds={format_kind_counts(counts)}"
    return observed, checks, summary


def evaluate_callers(payload: dict[str, Any], expected: dict[str, Any]) -> tuple[dict[str, Any], list[dict[str, Any]], str]:
    rows = payload.get(expected.get("confidence_array_field", "candidate_results"), [])
    total_field = expected.get("total_field", "total")
    total = int(payload.get(total_field, payload.get("total", len(rows))))
    confidences = [float(row.get("confidence", 0.0)) for row in rows]
    avg_confidence = (sum(confidences) / len(confidences)) if confidences else 0.0
    max_confidence = max(confidences) if confidences else 0.0
    callers = [row.get("caller") for row in rows if row.get("caller")]
    heuristics = [row.get("heuristic") for row in rows if row.get("heuristic")]
    observed = {
        "total": total,
        "avg_confidence": round(avg_confidence, 6),
        "max_confidence": round(max_confidence, 6),
        "callers": callers,
        "heuristics": heuristics,
        "rows_returned": len(rows),
    }
    checks = [
        check_range("total", total, expected.get("min_total"), expected.get("max_total")),
    ]
    if "max_avg_confidence" in expected:
        checks.append(check_upper("avg_confidence", avg_confidence, expected["max_avg_confidence"]))
    if "min_avg_confidence" in expected:
        checks.append(check_lower("avg_confidence", avg_confidence, expected["min_avg_confidence"]))
    if "min_max_confidence" in expected:
        checks.append(check_lower("max_confidence", max_confidence, expected["min_max_confidence"]))
    for caller in expected.get("must_contain_callers", []):
        checks.append(check_contains("must_contain_callers", caller, set(callers)))
    for heuristic in expected.get("must_contain_heuristics", []):
        checks.append(check_contains("must_contain_heuristics", heuristic, set(heuristics)))
    summary = f"total={total} avg_conf={avg_confidence:.2f} max_conf={max_confidence:.2f}"
    return observed, checks, summary


def evaluate_method_fields(payload: dict[str, Any], expected: dict[str, Any]) -> tuple[dict[str, Any], list[dict[str, Any]], str]:
    calls = payload.get("calls", [])
    fields = payload.get("fields", [])
    call_names = [item.get("name") for item in calls if item.get("name")]
    observed = {
        "method_name": payload.get("method_name"),
        "outgoing_calls": len(calls),
        "field_count": len(fields),
        "call_names": call_names,
    }
    checks = []
    if "must_contain_method_name" in expected:
        checks.append(
            {
                "name": "method_name",
                "passed": payload.get("method_name") == expected["must_contain_method_name"],
                "expected": expected["must_contain_method_name"],
                "actual": payload.get("method_name"),
            }
        )
    checks.append(check_lower("outgoing_calls", len(calls), expected.get("min_outgoing_calls", 0)))
    for call_name in expected.get("must_contain_calls", []):
        checks.append(check_contains("must_contain_calls", call_name, set(call_names)))
    summary = f"outgoing_calls={len(calls)} fields={len(fields)}"
    return observed, checks, summary


def evaluate_field_access(
    payload: dict[str, Any], expected: dict[str, Any], duration: float
) -> tuple[dict[str, Any], list[dict[str, Any]], str]:
    total_matches = int(payload.get("total_matches", 0))
    total_files = int(payload.get("total_files", 0))
    observed = {
        "total_matches": total_matches,
        "total_files": total_files,
        "files_returned": int(payload.get("files_returned", 0)),
    }
    checks = [
        check_range(
            "total_matches",
            total_matches,
            expected.get("min_total_matches"),
            expected.get("max_total_matches"),
        ),
        check_range("total_files", total_files, expected.get("min_total_files"), expected.get("max_total_files")),
    ]
    if "max_duration_seconds" in expected:
        checks.append(check_upper("wall_time_seconds", duration, expected["max_duration_seconds"]))
    summary = f"matches={total_matches} files={total_files}"
    return observed, checks, summary


def evaluate_cross_root(payload: dict[str, Any], expected: dict[str, Any]) -> tuple[dict[str, Any], list[dict[str, Any]], str]:
    total = int(payload.get("total", len(payload.get("results", []))))
    returned = len(payload.get("results", []))
    observed = {
        "total": total,
        "results_returned": returned,
        "has_more": bool(payload.get("has_more", False)),
    }
    checks = [
        check_range("total", total, expected.get("min_total"), expected.get("max_total")),
    ]
    if "min_results_returned" in expected:
        checks.append(check_lower("results_returned", returned, expected["min_results_returned"]))
    summary = f"total={total} returned={returned}"
    return observed, checks, summary


def check_range(name: str, actual: float, minimum: float | None, maximum: float | None) -> dict[str, Any]:
    passed = True
    if minimum is not None and actual < minimum:
        passed = False
    if maximum is not None and actual > maximum:
        passed = False
    return {"name": name, "passed": passed, "expected_min": minimum, "expected_max": maximum, "actual": actual}


def check_lower(name: str, actual: float, minimum: float) -> dict[str, Any]:
    return {"name": name, "passed": actual >= minimum, "expected_min": minimum, "actual": actual}


def check_upper(name: str, actual: float, maximum: float) -> dict[str, Any]:
    return {"name": name, "passed": actual <= maximum, "expected_max": maximum, "actual": actual}


def check_contains(name: str, expected_item: str, actual_items: set[str]) -> dict[str, Any]:
    return {"name": name, "passed": expected_item in actual_items, "expected_item": expected_item}


def check_not_contains(name: str, forbidden_item: str, actual_items: set[str]) -> dict[str, Any]:
    return {"name": name, "passed": forbidden_item not in actual_items, "forbidden_item": forbidden_item}


def format_kind_counts(counts: Counter[str]) -> str:
    order = ["interface", "type_alias", "class", "function", "method"]
    items = [f"{kind}:{counts[kind]}" for kind in order if counts.get(kind)]
    remaining = sorted(kind for kind in counts if kind not in order)
    items.extend(f"{kind}:{counts[kind]}" for kind in remaining)
    return "{" + ",".join(items) + "}"


def canonical_json(payload: Any) -> str:
    return json.dumps(payload, sort_keys=True, separators=(",", ":"))


def write_expected_snapshot(task_id: str, payload: dict[str, Any]) -> None:
    EXPECTED_DIR.mkdir(parents=True, exist_ok=True)
    path = EXPECTED_DIR / f"{task_id}.json"
    path.write_text(json.dumps(payload, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def render_text_report(report: dict[str, Any]) -> str:
    lines = [
        "codetopo Query Benchmark Report",
        "================================",
        f"{report['generated_at']} | {report['binary']}",
        "",
    ]
    for item in report["tasks"]:
        lines.append(
            f"{item['status']:<4}  {item['task']['id']:<18} {item['summary']:<48} {item['wall_time_seconds']:.2f}s"
        )
    lines.append("")
    summary = report["summary"]
    parts = [f"{summary['pass_count']}/{summary['task_count']} passed"]
    if summary["warn_count"]:
        parts.append(f"{summary['warn_count']} expected-noise warning")
    if summary["fail_count"]:
        parts.append(f"{summary['fail_count']} failed")
    lines.append(", ".join(parts))
    return "\n".join(lines) + "\n"


def build_report(binary: str, task_paths: list[Path], update_expected: bool) -> dict[str, Any]:
    task_reports = []
    for task_path in task_paths:
        task = load_json(task_path)
        payload, duration, command, stderr_lines = run_tool(binary, task)
        if update_expected:
            write_expected_snapshot(task["id"], payload)
        task_report = evaluate_task(task, payload, duration)
        task_report["command"] = command
        task_report["stderr"] = stderr_lines
        task_report["task_file"] = str(task_path.relative_to(REPO_ROOT))
        task_reports.append(task_report)

    summary = {
        "task_count": len(task_reports),
        "pass_count": sum(1 for item in task_reports if item["status"] == "PASS"),
        "warn_count": sum(1 for item in task_reports if item["status"] == "WARN"),
        "fail_count": sum(1 for item in task_reports if item["status"] == "FAIL"),
    }
    return {
        "report_version": 1,
        "generated_at": datetime.now().astimezone().isoformat(timespec="seconds"),
        "binary": binary,
        "tasks": task_reports,
        "summary": summary,
    }


def write_results(report: dict[str, Any], text_report: str) -> tuple[Path, Path]:
    RESULTS_DIR.mkdir(parents=True, exist_ok=True)
    stamp = datetime.now().strftime("%Y%m%dT%H%M%S")
    json_path = RESULTS_DIR / f"{stamp}-report.json"
    text_path = RESULTS_DIR / f"{stamp}-report.txt"
    json_path.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    text_path.write_text(text_report, encoding="utf-8")
    return json_path, text_path


def main() -> int:
    args = parse_args()
    binary = resolve_binary(args.binary)
    task_paths = load_task_paths(args.task)
    report = build_report(binary, task_paths, args.update_expected)
    text_report = render_text_report(report)
    if not args.no_write_results:
        write_results(report, text_report)
    print(text_report, end="")
    return 1 if report["summary"]["fail_count"] else 0


if __name__ == "__main__":
    sys.exit(main())
