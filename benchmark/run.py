from __future__ import annotations

import argparse
import json
from pathlib import Path
from typing import Any

from metrics import ToolCall, compute_tool_metrics
from score import TaskDefinition, load_task, score_output


def build_report(task: TaskDefinition, trajectory_payload: Any | None, final_output: str | None) -> dict[str, Any]:
    tool_calls = extract_tool_calls(trajectory_payload) if trajectory_payload is not None else []
    resolved_output = final_output or extract_final_output(trajectory_payload)
    if not resolved_output:
        raise ValueError("No final agent output found. Provide --output, --output-file, or a trajectory with final_output.")

    score_report = score_output(task, resolved_output)
    metric_report = compute_tool_metrics(tool_calls)

    return {
        "task": {
            "id": task.task_id,
            "prompt": task.prompt,
            "repo_root": str(task.repo_root),
            "oracle_notes": task.oracle_notes,
        },
        "score": score_report.to_dict(),
        "metrics": metric_report.to_dict(),
    }


def extract_tool_calls(payload: Any) -> list[ToolCall]:
    items = _trajectory_items(payload)
    tool_calls: list[ToolCall] = []
    for item in items:
        if not isinstance(item, dict):
            continue
        name = item.get("tool") or item.get("name") or item.get("tool_name")
        if not isinstance(name, str) or not name:
            continue
        arguments = item.get("arguments", item.get("args", item.get("input", item.get("params", {}))))
        result = item.get("result", item.get("output", item.get("response", item.get("content"))))
        tool_calls.append(ToolCall(name=name, arguments=arguments, result=result))
    return tool_calls


def extract_final_output(payload: Any) -> str | None:
    if payload is None:
        return None
    if isinstance(payload, dict):
        for key in ("final_output", "final_answer", "assistant_output", "output"):
            value = payload.get(key)
            if isinstance(value, str) and value.strip():
                return value
        messages = payload.get("messages")
        if isinstance(messages, list):
            for message in reversed(messages):
                if not isinstance(message, dict):
                    continue
                role = message.get("role") or message.get("type")
                content = message.get("content") or message.get("text")
                if role in {"assistant", "final_answer"} and isinstance(content, str) and content.strip():
                    return content
    elif isinstance(payload, list):
        for item in reversed(payload):
            if not isinstance(item, dict):
                continue
            if item.get("role") != "assistant":
                continue
            content = item.get("content") or item.get("text")
            if isinstance(content, str) and content.strip():
                return content
    return None


def _trajectory_items(payload: Any) -> list[Any]:
    if isinstance(payload, list):
        return payload
    if isinstance(payload, dict):
        for key in ("tool_calls", "trajectory", "calls", "steps"):
            value = payload.get(key)
            if isinstance(value, list):
                return value
    return []


def main() -> None:
    parser = argparse.ArgumentParser(description="Run the codetopo benchmark scorer over an offline trajectory.")
    parser.add_argument("task", help="Path to a benchmark task JSON file.")
    parser.add_argument("--trajectory", help="Path to a pre-captured trajectory JSON file.")
    output_group = parser.add_mutually_exclusive_group()
    output_group.add_argument("--output", help="Raw final agent output text.")
    output_group.add_argument("--output-file", help="Path to a file containing raw final agent output.")
    parser.add_argument("--json", action="store_true", help="Emit the full report as JSON.")
    args = parser.parse_args()

    task = load_task(args.task)
    trajectory_payload = json.loads(Path(args.trajectory).read_text(encoding="utf-8")) if args.trajectory else None
    final_output = args.output
    if args.output_file:
        final_output = Path(args.output_file).read_text(encoding="utf-8")

    report = build_report(task, trajectory_payload, final_output)
    if args.json:
        print(json.dumps(report, indent=2))
        return

    score = report["score"]
    metrics = report["metrics"]
    print(f"task: {report['task']['id']}")
    print(
        "file_f1="
        f"{score['file_f1']:.3f} "
        f"(p={score['file_precision']:.3f}, r={score['file_recall']:.3f})"
    )
    print(
        "line_f1="
        f"{score['line_f1']:.3f} "
        f"(p={score['line_precision']:.3f}, r={score['line_recall']:.3f})"
    )
    print(f"missing_files={score['missing_files']}")
    print(f"extra_files={score['extra_files']}")
    print(f"tool_call_count={metrics['tool_call_count']}")
    print(f"structural_ratio={_fmt_optional(metrics['structural_ratio'])}")
    print(f"source_lines_read={metrics['source_lines_read']}")
    print(f"reread_rate={_fmt_optional(metrics['reread_rate'])}")
    print(f"node_reuse_rate={_fmt_optional(metrics['node_reuse_rate'])}")


def _fmt_optional(value: float | None) -> str:
    return "n/a" if value is None else f"{value:.3f}"


if __name__ == "__main__":
    main()
