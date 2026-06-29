# codetopo benchmark harness

This benchmark adapts FastContext's citation-scoring shape to codetopo:

- file-level precision / recall / F1
- line-level precision / recall / F1
- missing / extra citation reporting
- codetopo telemetry: tool counts, structural ratio, source lines read, reread rate, node reuse rate

## Layout

```text
benchmark/
  README.md
  requirements.txt
  metrics.py
  score.py
  run.py
  tasks/
```

## Task format

Each task JSON contains:

- `id`
- `prompt`
- `repo_root`
- `ground_truth.files`
- `ground_truth.symbols`
- `ground_truth.line_ranges`
- `oracle_notes`

Task 3 intentionally mixes codetopo-relative paths with an absolute FastContext path.

## Expected trajectory format

`run.py` accepts an offline JSON trajectory. The simplest shape is:

```json
{
  "final_output": "<final_answer>\nsrc/main.cpp:211-212\n</final_answer>",
  "tool_calls": [
    {
      "tool": "context_by_name",
      "arguments": {"name": "context_by_name"},
      "result": {"symbol": {"node_id": 123}}
    },
    {
      "tool": "context_for",
      "arguments": {"node_id": 123, "include_source": false},
      "result": {"symbol": {"file_path": "src/mcp/tools.cpp"}}
    }
  ]
}
```

It also accepts raw lists of tool-call objects, or top-level keys such as `trajectory` / `steps`.

## Run

From this directory:

```bash
python3 run.py tasks/task1.json --trajectory trajectory.json
```

Score raw output without a trajectory:

```bash
python3 score.py tasks/task1.json --text-file final_output.txt
```

Print full JSON:

```bash
python3 run.py tasks/task1.json --trajectory trajectory.json --json
```

## Citation contract

The scorer looks for citations in either:

- a `<final_answer>...</final_answer>` block, or
- the full output text if no block exists

Supported citation forms:

- `file:line`
- `file:start-end`

Examples:

```text
src/main.cpp:211
src/mcp/tools.cpp:3781-3788
/Volumes/Projects/fastcontext/skills/fastcontext/SKILL.md:21-26
```

## Metrics

### Accuracy

- **file precision / recall / F1**: set overlap between cited files and oracle files
- **line precision / recall / F1**: set overlap between cited lines and oracle line ranges
- **missing files**: oracle files not cited
- **extra files**: cited files not in oracle
- **line coverage**: per-range coverage fraction for each oracle range

### codetopo-specific efficiency

- **tool_call_count**: total captured MCP calls
- **structural_ratio**: structural calls ÷ `(structural + source/code-search calls)`
- **source_lines_read**: inclusive sum of requested `source_at` ranges
- **reread_rate**: fraction of `source_at` calls whose range overlapped an earlier `source_at` call for the same file
- **node_reuse_rate**: fraction of `context_for` / `callers_approx` calls that used a `node_id` already surfaced by a prior tool result

Structural tools counted today:

- `symbol_search`
- `context_for`
- `file_overview`
- `symbols_in_path`
- `context_by_name`
- `dir_tree`
- `method_fields`
- `callers_approx`
- `callees_approx`
- `impact_of`

Source-read tools counted today:

- `source_at`
- `code_search`

## Ground-truth notes

- Task 1 and Task 2 line numbers were verified by reading codetopo source directly.
- Task 3's FastContext line range was taken from `.squad/agents/simon/fastcontext-analysis.md`, because this harness is checked into codetopo while the reference repo lives beside it.

## Interpreting scores

- High file F1 + low line F1: the agent found the right files but cited broad or imprecise spans.
- High line F1 + low structural ratio: the agent solved the task, but leaned too hard on raw reads.
- High reread rate: wasted source fetches.
- High node reuse rate: good structural-handle discipline.
