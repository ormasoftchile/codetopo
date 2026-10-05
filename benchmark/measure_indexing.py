#!/usr/bin/env python3
"""
measure_indexing.py — Comprehensive indexing benchmark and hot-spot analysis tool for codetopo.

Usage:
  # Run a benchmark on a repository:
  python3 benchmark/measure_indexing.py run /Volumes/Projects/kubernetes --name kubernetes --turbo

  # Run a benchmark with custom settings:
  python3 benchmark/measure_indexing.py run /Volumes/Projects/kibana --name kibana --threads 10 --turbo

  # Compare multiple benchmark results:
  python3 benchmark/measure_indexing.py compare benchmark/results/kubernetes.json benchmark/results/kibana.json

  # Compare and print markdown table:
  python3 benchmark/measure_indexing.py compare benchmark/results/*.json --markdown
"""

import argparse
import json
import os
import re
import shutil
import sqlite3
import subprocess
import sys
import time
from pathlib import Path


def run_benchmark(args):
    repo_root = Path(args.repo_root).resolve()
    if not repo_root.is_dir():
        print(f"Error: repo directory {repo_root} does not exist", file=sys.stderr)
        sys.exit(1)

    name = args.name or repo_root.name
    results_dir = Path(args.results_dir).resolve()
    results_dir.mkdir(parents=True, exist_ok=True)
    json_output = Path(args.output).resolve() if args.output else results_dir / f"{name}.json"
    temp_profile_json = results_dir / f"{name}_raw_profile.json"

    # Database destination
    if args.db:
        db_path = Path(args.db).resolve()
    else:
        # Default to a dedicated benchmark database in results_dir or repo .codetopo
        if args.in_tree:
            codetopo_dir = repo_root / ".codetopo"
            codetopo_dir.mkdir(parents=True, exist_ok=True)
            db_path = codetopo_dir / "index.sqlite"
        else:
            db_path = results_dir / f"{name}_index.sqlite"

    # Delete existing index if clean run is requested
    if args.clean and db_path.exists():
        print(f"Cleaning existing index at {db_path}...")
        for p in db_path.parent.glob(f"{db_path.name}*"):
            p.unlink(missing_ok=True)

    exe = Path(args.binary).resolve()
    if not exe.is_file():
        print(f"Error: binary {exe} not found. Please build codetopo first.", file=sys.stderr)
        sys.exit(1)

    cmd = [
        str(exe), "index",
        "--root", str(repo_root),
        "--db", str(db_path),
        "--profile",
        "--profile-json", str(temp_profile_json)
    ]

    if args.threads > 0:
        cmd.extend(["--threads", str(args.threads)])
    if args.turbo:
        cmd.append("--turbo")
    if args.force:
        cmd.append("--force")
    if args.max_files > 0:
        cmd.extend(["--max-files", str(args.max_files)])
    if args.batch_size > 0:
        cmd.extend(["--batch-size", str(args.batch_size)])

    print("=" * 70)
    print(f"Running Indexing Benchmark: {name}")
    print(f"  Repo:       {repo_root}")
    print(f"  Database:   {db_path}")
    print(f"  Command:    {' '.join(cmd)}")
    print("=" * 70)

    start_wall = time.perf_counter()
    proc = subprocess.run(cmd)
    end_wall = time.perf_counter()
    wall_duration = end_wall - start_wall

    if proc.returncode != 0:
        print(f"Error: codetopo exited with code {proc.returncode}", file=sys.stderr)
        sys.exit(proc.returncode)

    # Load raw profile if generated
    profile_data = {}
    if temp_profile_json.exists():
        try:
            with open(temp_profile_json, "r") as f:
                profile_data = json.load(f)
            temp_profile_json.unlink(missing_ok=True)
        except Exception as e:
            print(f"Warning: could not parse profile json: {e}", file=sys.stderr)

    # Query DB stats
    db_stats = {
        "file_count": 0,
        "symbol_count": 0,
        "edge_count": 0,
        "ref_count": 0,
        "db_size_mb": 0.0,
        "wal_size_mb": 0.0,
    }

    if db_path.exists():
        db_stats["db_size_mb"] = round(db_path.stat().st_size / (1024 * 1024), 2)
        wal_path = Path(str(db_path) + "-wal")
        if wal_path.exists():
            db_stats["wal_size_mb"] = round(wal_path.stat().st_size / (1024 * 1024), 2)

        try:
            conn = sqlite3.connect(str(db_path))
            c = conn.cursor()
            c.execute("SELECT count(*) FROM files")
            db_stats["file_count"] = c.fetchone()[0]
            c.execute("SELECT count(*) FROM nodes WHERE node_type = 'symbol'")
            db_stats["symbol_count"] = c.fetchone()[0]
            c.execute("SELECT count(*) FROM edges")
            db_stats["edge_count"] = c.fetchone()[0]
            c.execute("SELECT count(*) FROM refs WHERE resolved_node_id IS NOT NULL")
            db_stats["resolved_refs"] = c.fetchone()[0]
            c.execute("SELECT count(*) FROM refs WHERE resolved_node_id IS NULL")
            db_stats["unresolved_refs"] = c.fetchone()[0]
            conn.close()
        except Exception as e:
            print(f"Warning: DB queries failed: {e}", file=sys.stderr)

    # Compute graph quality metrics
    quality_data = {}
    if db_path.exists():
        try:
            q_res = subprocess.run([str(exe), "quality", "--db", str(db_path), "--json"], capture_output=True, text=True)
            if q_res.returncode == 0:
                quality_data = json.loads(q_res.stdout)
        except Exception as e:
            print(f"Warning: codetopo quality failed: {e}", file=sys.stderr)

    # Synthesize benchmark record
    total_files = db_stats["file_count"] or profile_data.get("total_files", 0)
    files_per_sec = round(total_files / wall_duration, 1) if wall_duration > 0 else 0

    record = {
        "name": name,
        "repo_root": str(repo_root),
        "timestamp": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "wall_time_s": round(wall_duration, 2),
        "total_files": total_files,
        "files_per_sec": files_per_sec,
        "threads": args.threads if args.threads > 0 else profile_data.get("thread_count", 0),
        "turbo": args.turbo,
        "peak_rss_mb": profile_data.get("peak_rss_mb", 0.0),
        "db_stats": db_stats,
        "quality": quality_data,
        "phases": profile_data.get("phases", {}),
    }

    with open(json_output, "w") as f:
        json.dump(record, f, indent=2)

    print(f"\nSaved benchmark record to {json_output}")
    print_single_summary(record)


def print_single_summary(rec):
    print("\n" + "=" * 50)
    print(f"BENCHMARK SUMMARY: {rec['name']}")
    print("=" * 50)
    print(f"  Wall Time:     {rec['wall_time_s']} s")
    print(f"  Indexed Files: {rec['total_files']} files")
    print(f"  Throughput:    {rec['files_per_sec']} files/s")
    print(f"  Symbols:       {rec['db_stats'].get('symbol_count', 0):,}")
    print(f"  Edges:         {rec['db_stats'].get('edge_count', 0):,}")
    print(f"  Peak RSS:      {rec.get('peak_rss_mb', 0)} MB")
    print(f"  DB File Size:  {rec['db_stats'].get('db_size_mb', 0)} MB")

    quality = rec.get("quality", {})
    if quality:
        print("-" * 50)
        print("Graph Quality:")
        print(f"  Call Resolution Rate: {quality.get('call_resolution_rate', 0.0):.1f}%")
        print(f"  Resolved Calls:       {quality.get('resolved_call_refs', 0):,}")
        print(f"  Unresolved Calls:     {quality.get('unresolved_call_refs', 0):,}")
        print(f"  Ambiguous (>=2 defs): {quality.get('ambiguous_call_refs', 0):,}")
        print(f"  Dangling (external):  {quality.get('dangling_call_refs', 0):,}")
    print("-" * 50)

    phases = rec.get("phases", {})
    if phases:
        print("Phase Breakdown:")
        # Sort phases by ms descending
        sorted_phases = sorted(phases.items(), key=lambda kv: kv[1].get("ms", 0), reverse=True)
        for name, data in sorted_phases:
            ms = data.get("ms", 0)
            calls = data.get("calls", 0)
            avg = data.get("avg_ms", 0)
            if ms > 0.1:
                print(f"  {name:16s}: {ms:10.1f} ms  ({calls:7d} calls, {avg:7.2f} ms/call)")


def compare_benchmarks(args):
    records = []
    for p in args.files:
        path = Path(p)
        if not path.is_file():
            continue
        try:
            with open(path, "r") as f:
                records.append(json.load(f))
        except Exception as e:
            print(f"Error reading {path}: {e}", file=sys.stderr)

    if not records:
        print("No valid benchmark files provided.", file=sys.stderr)
        return

    # Sort records by name or total_files
    records.sort(key=lambda r: r.get("total_files", 0))

    if args.markdown:
        render_markdown_comparison(records)
    else:
        render_terminal_comparison(records)


def render_terminal_comparison(records):
    print("\n" + "=" * 90)
    print("CODETOPO INDEXING PERFORMANCE COMPARISON")
    print("=" * 90)

    # General overview
    col_w = max(18, max(len(r["name"]) for r in records) + 2)
    header = f"{'Metric':<26} " + "".join(f"{r['name']:>{col_w}}" for r in records)
    print(header)
    print("-" * len(header))

    def row(label, func):
        vals = "".join(f"{func(r):>{col_w}}" for r in records)
        print(f"{label:<26} {vals}")

    row("Files Indexed", lambda r: f"{r['total_files']:,}")
    row("Wall Time (s)", lambda r: f"{r['wall_time_s']:.1f}")
    row("Throughput (files/s)", lambda r: f"{r['files_per_sec']:.1f}")
    row("Symbols Indexed", lambda r: f"{r['db_stats'].get('symbol_count', 0):,}")
    row("Edges Created", lambda r: f"{r['db_stats'].get('edge_count', 0):,}")
    row("Peak RSS (MB)", lambda r: f"{r.get('peak_rss_mb', 0):.1f}")
    row("DB Size (MB)", lambda r: f"{r['db_stats'].get('db_size_mb', 0):.1f}")
    row("Call Resolution %", lambda r: f"{r.get('quality', {}).get('call_resolution_rate', 0.0):.1f}%")
    row("Dangling Refs", lambda r: f"{r.get('quality', {}).get('dangling_call_refs', 0):,}")
    row("Threads / Turbo", lambda r: f"{r.get('threads', '?')} / {'ON' if r.get('turbo') else 'OFF'}")

    print("\n" + "-" * len(header))
    print("PHASE BREAKDOWN (ms)")
    print("-" * len(header))

    all_phase_names = [
        "scan", "file_read", "hash", "parse", "extract", "contention",
        "persist", "persist_wait", "flush", "fts_rebuild", "resolve_refs",
        "pagerank", "metadata", "wal_ckpt"
    ]

    for p in all_phase_names:
        row(p, lambda r: f"{r.get('phases', {}).get(p, {}).get('ms', 0):.1f}")

    print("\n" + "=" * 90)
    print("HOT PLACES & BOTTLENECK ANALYSIS")
    print("=" * 90)
    analyze_hotspots(records, col_w)


def render_markdown_comparison(records):
    print("## Codetopo Indexing Performance Comparison\n")
    headers = ["Metric"] + [r["name"] for r in records]
    print("| " + " | ".join(headers) + " |")
    print("| " + " | ".join(["---"] * len(headers)) + " |")

    def mrow(label, func):
        vals = [func(r) for r in records]
        print(f"| **{label}** | " + " | ".join(vals) + " |")

    mrow("Files Indexed", lambda r: f"{r['total_files']:,}")
    mrow("Wall Time", lambda r: f"{r['wall_time_s']:.1f} s")
    mrow("Throughput", lambda r: f"{r['files_per_sec']:.1f} files/s")
    mrow("Symbols", lambda r: f"{r['db_stats'].get('symbol_count', 0):,}")
    mrow("Edges", lambda r: f"{r['db_stats'].get('edge_count', 0):,}")
    mrow("Peak RSS", lambda r: f"{r.get('peak_rss_mb', 0):.1f} MB")
    mrow("DB Size", lambda r: f"{r['db_stats'].get('db_size_mb', 0):.1f} MB")
    mrow("Call Resolution %", lambda r: f"{r.get('quality', {}).get('call_resolution_rate', 0.0):.1f}%")
    mrow("Dangling Refs", lambda r: f"{r.get('quality', {}).get('dangling_call_refs', 0):,}")
    mrow("Threads / Turbo", lambda r: f"{r.get('threads', '?')} / {'ON' if r.get('turbo') else 'OFF'}")

    print("\n### Phase Timing Breakdown (ms)\n")
    all_phase_names = [
        "scan", "file_read", "hash", "parse", "extract", "contention",
        "persist", "persist_wait", "flush", "fts_rebuild", "resolve_refs",
        "pagerank", "metadata", "wal_ckpt"
    ]
    p_headers = ["Phase"] + [r["name"] for r in records]
    print("| " + " | ".join(p_headers) + " |")
    print("| " + " | ".join(["---"] * len(p_headers)) + " |")
    for p in all_phase_names:
        mrow(p, lambda r: f"{r.get('phases', {}).get(p, {}).get('ms', 0):,.1f}")


def analyze_hotspots(records, col_w):
    for r in records:
        print(f"\nTarget: {r['name']} ({r['total_files']:,} files, {r['wall_time_s']:.1f}s)")
        phases = r.get("phases", {})
        if not phases:
            print("  No phase telemetry available.")
            continue

        # Separate worker parallel thread-time from serial wall-clock stages
        parallel_phases = ["file_read", "hash", "parse", "extract"]
        serial_phases = ["scan", "persist", "flush", "fts_rebuild", "resolve_refs", "pagerank", "metadata", "wal_ckpt"]
        sync_phases = ["contention", "persist_wait"]

        parallel_sum = sum(phases.get(p, {}).get("ms", 0) for p in parallel_phases)
        serial_sum = sum(phases.get(p, {}).get("ms", 0) for p in serial_phases)

        threads = r.get("threads", 1) or 1
        est_parallel_wall_ms = parallel_sum / threads

        print(f"  Worker CPU Time:      {parallel_sum / 1000:.1f} s total across {threads} threads (~{est_parallel_wall_ms / 1000:.1f}s effective wall)")
        print(f"  Serial Pipeline Time: {serial_sum / 1000:.1f} s")

        # Top 3 hot spots
        all_sorted = sorted(phases.items(), key=lambda kv: kv[1].get("ms", 0), reverse=True)
        print("  Top Hot Places:")
        for rank, (name, data) in enumerate(all_sorted[:4], 1):
            ms = data.get("ms", 0)
            avg = data.get("avg_ms", 0)
            calls = data.get("calls", 0)
            tag = "Parallel Worker" if name in parallel_phases else ("Sync/Contention" if name in sync_phases else "Serial Stage")
            print(f"    {rank}. {name:<14} {ms/1000:7.2f} s ({ms:9.1f} ms) [{tag}] - {calls} calls @ {avg:.2f} ms/call")


def main():
    parser = argparse.ArgumentParser(description="Codetopo Indexing Performance & Profiling Harness")
    subparsers = parser.add_subparsers(dest="command", required=True)

    # run subcommand
    run_parser = subparsers.add_parser("run", help="Run indexing benchmark on a repository")
    run_parser.add_argument("repo_root", help="Path to repository to index")
    run_parser.add_argument("--name", help="Benchmark name identifier (default: repo dir name)")
    run_parser.add_argument("--binary", default="./build/codetopo", help="Path to codetopo executable")
    run_parser.add_argument("--db", help="Explicit database path")
    run_parser.add_argument("--in-tree", action="store_true", help="Index directly into <repo>/.codetopo/index.sqlite")
    run_parser.add_argument("--clean", action="store_true", default=True, help="Wipe database before running for a cold-index benchmark")
    run_parser.add_argument("--threads", type=int, default=0, help="Worker thread count (0=auto)")
    run_parser.add_argument("--turbo", action="store_true", help="Enable turbo perf mode")
    run_parser.add_argument("--force", action="store_true", help="Re-extract even if unchanged")
    run_parser.add_argument("--max-files", type=int, default=0, help="Limit max files to index")
    run_parser.add_argument("--batch-size", type=int, default=0, help="Batch size for persistence")
    run_parser.add_argument("--results-dir", default="benchmark/results", help="Directory to save benchmark output")
    run_parser.add_argument("--output", help="Explicit path for benchmark JSON result")

    # compare subcommand
    comp_parser = subparsers.add_parser("compare", help="Compare multiple benchmark results")
    comp_parser.add_argument("files", nargs="+", help="JSON benchmark result files")
    comp_parser.add_argument("--markdown", action="store_true", help="Output GitHub-flavored markdown table")

    args = parser.parse_args()
    if args.command == "run":
        run_benchmark(args)
    elif args.command == "compare":
        compare_benchmarks(args)


if __name__ == "__main__":
    main()
