from __future__ import annotations

import json
from dataclasses import asdict, dataclass
from typing import Any


STRUCTURAL_TOOLS = {
    "symbol_search",
    "context_for",
    "file_overview",
    "symbols_in_path",
    "context_by_name",
    "dir_tree",
    "method_fields",
    "callers_approx",
    "callees_approx",
    "impact_of",
}

SOURCE_TOOLS = {"source_at", "code_search"}
NODE_REUSE_TOOLS = {"context_for", "callers_approx"}


@dataclass(frozen=True)
class ToolCall:
    name: str
    arguments: Any
    result: Any


@dataclass(frozen=True)
class ToolMetrics:
    tool_call_count: int
    tool_counts: dict[str, int]
    structural_ratio: float
    source_lines_read: int
    reread_rate: float
    node_reuse_rate: float

    def to_dict(self) -> dict[str, Any]:
        return asdict(self)


def compute_tool_metrics(tool_calls: list[ToolCall]) -> ToolMetrics:
    tool_counts: dict[str, int] = {}
    classified_calls = 0
    structural_calls = 0
    source_lines_read = 0
    source_call_count = 0
    reread_count = 0
    prior_source_ranges: dict[str, list[tuple[int, int]]] = {}
    prior_node_ids: set[int] = set()
    reuse_denominator = 0
    reused_node_calls = 0

    for tool_call in tool_calls:
        name = tool_call.name
        tool_counts[name] = tool_counts.get(name, 0) + 1

        if name in STRUCTURAL_TOOLS or name in SOURCE_TOOLS:
            classified_calls += 1
        if name in STRUCTURAL_TOOLS:
            structural_calls += 1

        if name == "source_at":
            source_call_count += 1
            file_path, start_line, end_line = _extract_source_range(tool_call.arguments, tool_call.result)
            if file_path is not None and start_line is not None and end_line is not None and end_line >= start_line:
                source_lines_read += (end_line - start_line + 1)
                seen_ranges = prior_source_ranges.setdefault(file_path, [])
                if any(_ranges_overlap((start_line, end_line), existing) for existing in seen_ranges):
                    reread_count += 1
                seen_ranges.append((start_line, end_line))

        if name in NODE_REUSE_TOOLS:
            reuse_denominator += 1
            node_id = _extract_node_id_from_arguments(tool_call.arguments)
            if isinstance(node_id, int) and node_id in prior_node_ids:
                reused_node_calls += 1

        prior_node_ids.update(_collect_node_ids(tool_call.result))

    structural_ratio = (structural_calls / classified_calls) if classified_calls else 0.0
    reread_rate = (reread_count / source_call_count) if source_call_count else 0.0
    node_reuse_rate = (reused_node_calls / reuse_denominator) if reuse_denominator else 0.0

    return ToolMetrics(
        tool_call_count=len(tool_calls),
        tool_counts=tool_counts,
        structural_ratio=structural_ratio,
        source_lines_read=source_lines_read,
        reread_rate=reread_rate,
        node_reuse_rate=node_reuse_rate,
    )


def _extract_source_range(arguments: Any, result: Any) -> tuple[str | None, int | None, int | None]:
    data = _coerce_mapping(arguments)
    file_path = _first_str(data, "file", "path", "file_path")
    start_line = _first_int(data, "start_line", "start", "line")
    end_line = _first_int(data, "end_line", "end")

    if start_line is not None and end_line is None:
        end_line = start_line

    if file_path is not None and start_line is not None and end_line is not None:
        return file_path, start_line, end_line

    result_data = _coerce_mapping(result)
    file_path = file_path or _first_str(result_data, "file", "path", "file_path")
    start_line = start_line if start_line is not None else _first_int(result_data, "start_line", "start", "line")
    end_line = end_line if end_line is not None else _first_int(result_data, "end_line", "end")
    if start_line is not None and end_line is None:
        end_line = start_line
    return file_path, start_line, end_line


def _extract_node_id_from_arguments(arguments: Any) -> int | None:
    data = _coerce_mapping(arguments)
    value = data.get("node_id")
    return value if isinstance(value, int) else None


def _collect_node_ids(value: Any) -> set[int]:
    parsed = _maybe_parse_json(value)
    found: set[int] = set()

    def walk(node: Any) -> None:
        if isinstance(node, dict):
            for key, child in node.items():
                if key == "node_id" and isinstance(child, int):
                    found.add(child)
                elif key.endswith("_node_id") and isinstance(child, int):
                    found.add(child)
                else:
                    walk(child)
        elif isinstance(node, list):
            for child in node:
                walk(child)

    walk(parsed)
    return found


def _coerce_mapping(value: Any) -> dict[str, Any]:
    parsed = _maybe_parse_json(value)
    return parsed if isinstance(parsed, dict) else {}


def _maybe_parse_json(value: Any) -> Any:
    if not isinstance(value, str):
        return value
    stripped = value.strip()
    if not stripped or stripped[0] not in "{[":
        return value
    try:
        return json.loads(stripped)
    except json.JSONDecodeError:
        return value


def _first_str(data: dict[str, Any], *keys: str) -> str | None:
    for key in keys:
        value = data.get(key)
        if isinstance(value, str) and value:
            return value
    return None


def _first_int(data: dict[str, Any], *keys: str) -> int | None:
    for key in keys:
        value = data.get(key)
        if isinstance(value, int):
            return value
    return None


def _ranges_overlap(left: tuple[int, int], right: tuple[int, int]) -> bool:
    return left[0] <= right[1] and right[0] <= left[1]
