# codetopo query benchmark

This benchmark suite measures local `codetopo query` behavior directly, separate from the existing agent-trajectory benchmark in `benchmark/`.

It covers:

- enumeration quality (`symbols_in_path`)
- caller blast-radius quality (`callers_approx`)
- method internals (`method_fields`)
- field-access search speed (`code_search`)
- cross-root workspace reachability

## Run

Run all tasks:

```bash
python3 benchmark/query/run_query.py
```

Run one task:

```bash
python3 benchmark/query/run_query.py --task enum-dagre
```

Use a specific binary:

```bash
python3 benchmark/query/run_query.py --binary ./build/codetopo
```

Refresh checked-in reference outputs after a schema or extractor change:

```bash
python3 benchmark/query/run_query.py --update-expected
```

`run_query.py` prefers `./build/codetopo` when it exists, then falls back to `codetopo` on `PATH`.

## Output

Each run writes:

- `benchmark/query/results/{timestamp}-report.json`
- `benchmark/query/results/{timestamp}-report.txt`

Checked-in reference payloads live in:

- `benchmark/query/expected/*.json`

Task definitions live in:

- `benchmark/query/tasks/*.json`

## Metrics

### Enumeration

- `total`: symbols returned
- `kind_distribution`: counts by kind
- `must_contain_names`: regression guard for known symbols

### Callers

- `candidate_total` / `total`: blast radius size
- `avg_confidence`: average confidence across returned candidate rows
- `max_confidence`: strongest returned candidate

`callers-generic` is an **expected warning** benchmark: it should stay noisy.

### Method internals

- `outgoing_calls`: unique calls surfaced by `method_fields`
- `field_count`: unique `this.*` fields surfaced

### Field access

- `total_matches`
- `total_files`
- `wall_time_seconds`

### Cross-root

- `total`: symbol count from a non-primary workspace root

## Add a task

1. Copy an existing JSON file from `benchmark/query/tasks/`.
2. Set `repo_root`, `tool`, and `params`.
3. Run the query manually and verify the observed output.
4. Encode pass/fail thresholds in the task's `expected` block.
5. Refresh the checked-in snapshot with `--update-expected`.

Task files support the shared fields shown below plus category-specific expectations:

```json
{
  "id": "enum-dagre",
  "description": "Enumerate all structural types in dagre/lib",
  "category": "enumeration",
  "repo_root": "/Volumes/Projects/triton",
  "tool": "symbols_in_path",
  "params": {
    "path": "/Volumes/Projects/dagre/lib",
    "kind": ["class", "interface", "type_alias"],
    "recursive": true,
    "limit": 200
  },
  "expected": {
    "min_total": 43,
    "max_total": 43
  }
}
```
