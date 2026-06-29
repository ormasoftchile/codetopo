from __future__ import annotations

import argparse
import json
import re
from dataclasses import asdict, dataclass
from pathlib import Path
from typing import Any


FINAL_ANSWER_RE = re.compile(r"<final_answer>(.*?)</final_answer>", re.IGNORECASE | re.DOTALL)
CITATION_RE = re.compile(
    r"(?P<path>(?:[A-Za-z]:)?(?:/|\.{1,2}/)?[^\s:`<>\"']+):(?P<start>\d+)(?:-(?P<end>\d+))?"
)


@dataclass(frozen=True)
class Citation:
    file_path: str
    start_line: int
    end_line: int
    raw_text: str


@dataclass(frozen=True)
class TaskDefinition:
    task_id: str
    prompt: str
    repo_root: Path
    ground_truth_files: list[str]
    ground_truth_symbols: list[str]
    ground_truth_line_ranges: list[dict[str, Any]]
    oracle_notes: str


@dataclass(frozen=True)
class ScoreReport:
    citations: list[dict[str, Any]]
    parsed_citation_count: int
    file_precision: float
    file_recall: float
    file_f1: float
    line_precision: float
    line_recall: float
    line_f1: float
    missing_files: list[str]
    extra_files: list[str]
    line_coverage: list[dict[str, Any]]

    def to_dict(self) -> dict[str, Any]:
        return asdict(self)


def load_task(task_path: str | Path) -> TaskDefinition:
    raw = json.loads(Path(task_path).read_text(encoding="utf-8"))
    ground_truth = raw.get("ground_truth", {})
    return TaskDefinition(
        task_id=raw["id"],
        prompt=raw["prompt"],
        repo_root=Path(raw["repo_root"]),
        ground_truth_files=list(ground_truth.get("files", [])),
        ground_truth_symbols=list(ground_truth.get("symbols", [])),
        ground_truth_line_ranges=list(ground_truth.get("line_ranges", [])),
        oracle_notes=raw.get("oracle_notes", ""),
    )


def score_output(task: TaskDefinition, agent_output: str) -> ScoreReport:
    predicted = parse_citations(agent_output)
    ground_truth_files = [_normalize_task_path(path, task.repo_root) for path in task.ground_truth_files]
    canonical_ground_truth = set(ground_truth_files)

    normalized_predictions = [
        Citation(
            file_path=_canonicalize_prediction_path(citation.file_path, task.repo_root, canonical_ground_truth),
            start_line=citation.start_line,
            end_line=citation.end_line,
            raw_text=citation.raw_text,
        )
        for citation in predicted
    ]
    deduped_predictions = _dedupe_citations(normalized_predictions)

    predicted_files = {citation.file_path for citation in deduped_predictions}
    file_precision, file_recall, file_f1 = _precision_recall_f1(predicted_files, canonical_ground_truth)

    expected_ranges = [
        {
            "file": _normalize_task_path(item["file"], task.repo_root),
            "start": int(item["start"]),
            "end": int(item["end"]),
        }
        for item in task.ground_truth_line_ranges
    ]
    expected_lines = _expand_ranges((item["file"], item["start"], item["end"]) for item in expected_ranges)
    predicted_lines = _expand_ranges(
        (citation.file_path, citation.start_line, citation.end_line) for citation in deduped_predictions
    )
    line_precision, line_recall, line_f1 = _precision_recall_f1(predicted_lines, expected_lines)

    line_coverage: list[dict[str, Any]] = []
    for item in expected_ranges:
        range_lines = {(item["file"], line_no) for line_no in range(item["start"], item["end"] + 1)}
        covered_lines = predicted_lines & range_lines
        total_lines = len(range_lines)
        line_coverage.append(
            {
                "file": item["file"],
                "start": item["start"],
                "end": item["end"],
                "covered_lines": len(covered_lines),
                "total_lines": total_lines,
                "coverage": (len(covered_lines) / total_lines) if total_lines else 0.0,
            }
        )

    return ScoreReport(
        citations=[asdict(citation) for citation in deduped_predictions],
        parsed_citation_count=len(deduped_predictions),
        file_precision=file_precision,
        file_recall=file_recall,
        file_f1=file_f1,
        line_precision=line_precision,
        line_recall=line_recall,
        line_f1=line_f1,
        missing_files=sorted(canonical_ground_truth - predicted_files),
        extra_files=sorted(predicted_files - canonical_ground_truth),
        line_coverage=line_coverage,
    )


def parse_citations(agent_output: str) -> list[Citation]:
    search_text = _citation_text(agent_output)
    citations: list[Citation] = []
    for match in CITATION_RE.finditer(search_text):
        start_line = int(match.group("start"))
        end_line = int(match.group("end") or start_line)
        citations.append(
            Citation(
                file_path=_clean_path_token(match.group("path")),
                start_line=start_line,
                end_line=end_line,
                raw_text=match.group(0),
            )
        )
    return citations


def _citation_text(agent_output: str) -> str:
    matches = FINAL_ANSWER_RE.findall(agent_output)
    if not matches:
        return agent_output
    return "\n".join(matches)


def _clean_path_token(path_token: str) -> str:
    return path_token.strip().strip("`").rstrip(".,);]")


def _normalize_task_path(path_value: str, repo_root: Path) -> str:
    path = Path(path_value)
    if path.is_absolute():
        try:
            return path.resolve(strict=False).relative_to(repo_root.resolve(strict=False)).as_posix()
        except ValueError:
            return path.resolve(strict=False).as_posix()
    return path.as_posix()


def _canonicalize_prediction_path(path_value: str, repo_root: Path, canonical_ground_truth: set[str]) -> str:
    normalized = _normalize_task_path(path_value, repo_root)
    if normalized in canonical_ground_truth:
        return normalized

    repo_joined = _normalize_task_path(str(repo_root / path_value), repo_root)
    if repo_joined in canonical_ground_truth:
        return repo_joined

    suffix_matches = [candidate for candidate in canonical_ground_truth if candidate.endswith(normalized)]
    if len(suffix_matches) == 1:
        return suffix_matches[0]

    return normalized


def _dedupe_citations(citations: list[Citation]) -> list[Citation]:
    seen: set[tuple[str, int, int]] = set()
    deduped: list[Citation] = []
    for citation in citations:
        key = (citation.file_path, citation.start_line, citation.end_line)
        if key in seen:
            continue
        seen.add(key)
        deduped.append(citation)
    return deduped


def _expand_ranges(items: Any) -> set[tuple[str, int]]:
    lines: set[tuple[str, int]] = set()
    for file_path, start_line, end_line in items:
        for line_no in range(start_line, end_line + 1):
            lines.add((file_path, line_no))
    return lines


def _precision_recall_f1(predicted: set[Any], expected: set[Any]) -> tuple[float, float, float]:
    true_positives = len(predicted & expected)
    precision = true_positives / len(predicted) if predicted else 0.0
    recall = true_positives / len(expected) if expected else 0.0
    if precision + recall == 0:
        f1 = 0.0
    else:
        f1 = 2 * precision * recall / (precision + recall)
    return precision, recall, f1


def main() -> None:
    parser = argparse.ArgumentParser(description="Score codetopo benchmark output against a task oracle.")
    parser.add_argument("task", help="Path to a benchmark task JSON file.")
    output_group = parser.add_mutually_exclusive_group(required=True)
    output_group.add_argument("--text", help="Raw agent output to score.")
    output_group.add_argument("--text-file", help="Path to a file containing raw agent output.")
    args = parser.parse_args()

    task = load_task(args.task)
    agent_output = args.text if args.text is not None else Path(args.text_file).read_text(encoding="utf-8")
    report = score_output(task, agent_output)
    print(json.dumps(report.to_dict(), indent=2))


if __name__ == "__main__":
    main()
