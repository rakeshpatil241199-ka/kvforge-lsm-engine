#!/usr/bin/env python3
"""YCSB-Style Workload Benchmark Harness for KVForge LSM Engine.

Drives `kvforge_cli` across sequential write, random write, read-heavy, and
Zipfian mixed workloads while capturing throughput and p50/p95/p99 latencies.
"""

import argparse
import json
import math
import random
import subprocess
import tempfile
import time
from pathlib import Path
from typing import Dict, List, Tuple


def percentile(sorted_values: List[float], pct: float) -> float:
    if not sorted_values:
        return 0.0
    idx = (len(sorted_values) - 1) * (pct / 100.0)
    lo = math.floor(idx)
    hi = math.ceil(idx)
    if lo == hi:
        return sorted_values[lo]
    return sorted_values[lo] + (sorted_values[hi] - sorted_values[lo]) * (idx - lo)


class ZipfGenerator:
    """Discrete Zipfian distribution sampler for hot-key cache/read testing."""

    def __init__(self, n: int, skew: float = 0.99, seed: int = 42) -> None:
        self.rng = random.Random(seed)
        weights = [1.0 / ((i + 1) ** skew) for i in range(max(n, 1))]
        total = sum(weights)
        self.cdf: List[float] = []
        acc = 0.0
        for w in weights:
            acc += w / total
            self.cdf.append(acc)

    def sample(self) -> int:
        u = self.rng.random()
        lo, hi = 0, len(self.cdf) - 1
        while lo < hi:
            mid = (lo + hi) // 2
            if self.cdf[mid] < u:
                lo = mid + 1
            else:
                hi = mid
        return lo


def generate_workload(
    workload: str, ops: int, key_space: int, value_size: int, seed: int = 42
) -> List[str]:
    rng = random.Random(seed)
    payload = "v" * value_size
    commands: List[str] = []

    if workload == "seq_write":
        for i in range(ops):
            commands.append(f"PUT key_{i:08d} {payload}")
    elif workload == "rand_write":
        for _ in range(ops):
            k = rng.randint(0, key_space - 1)
            commands.append(f"PUT key_{k:08d} {payload}")
    elif workload == "read_heavy":
        for i in range(key_space):
            commands.append(f"PUT key_{i:08d} {payload}")
        commands.append("FLUSH")
        for _ in range(ops):
            k = rng.randint(0, key_space - 1)
            if rng.random() < 0.90:
                commands.append(f"GET key_{k:08d}")
            else:
                commands.append(f"PUT key_{k:08d} {payload}")
    elif workload == "mixed_zipf":
        zipf = ZipfGenerator(key_space, skew=0.99, seed=seed)
        for i in range(min(key_space, ops // 2)):
            commands.append(f"PUT key_{i:08d} {payload}")
        for _ in range(ops):
            k = zipf.sample()
            if rng.random() < 0.70:
                commands.append(f"GET key_{k:08d}")
            else:
                commands.append(f"PUT key_{k:08d} {payload}")
    else:
        raise ValueError(f"Unsupported workload: {workload}")

    return commands


def run_benchmark(
    binary: Path,
    workload: str,
    ops: int,
    key_space: int,
    value_size: int,
    memtable_kb: int,
    sync_wal: bool,
) -> Dict[str, object]:
    commands = generate_workload(workload, ops, key_space, value_size)

    with tempfile.TemporaryDirectory(prefix="kvforge_bench_") as tmpdir:
        cli_args = [
            str(binary),
            "--db",
            tmpdir,
            "--memtable-kb",
            str(memtable_kb),
        ]
        if not sync_wal:
            cli_args.append("--no-sync")

        proc = subprocess.Popen(
            cli_args,
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            bufsize=1,
        )
        assert proc.stdin is not None and proc.stdout is not None

        latencies_ms: List[float] = []
        t_start = time.perf_counter()

        for cmd in commands:
            op_start = time.perf_counter()
            proc.stdin.write(cmd + "\n")
            proc.stdin.flush()
            resp = proc.stdout.readline()
            op_elapsed_ms = (time.perf_counter() - op_start) * 1000.0
            if not resp:
                raise RuntimeError("kvforge_cli terminated unexpectedly")
            if cmd != "FLUSH":
                latencies_ms.append(op_elapsed_ms)

        proc.stdin.write("STATS\nEXIT\n")
        proc.stdin.flush()
        stats_line = proc.stdout.readline().strip()
        proc.wait(timeout=15)

        total_sec = time.perf_counter() - t_start
        disk_bytes = sum(f.stat().st_size for f in Path(tmpdir).glob("*") if f.is_file())

    latencies_ms.sort()
    executed = len(latencies_ms)
    return {
        "workload": workload,
        "operations": executed,
        "value_size_bytes": value_size,
        "sync_wal": sync_wal,
        "elapsed_sec": round(total_sec, 4),
        "throughput_ops_sec": round(executed / total_sec, 2) if total_sec > 0 else 0.0,
        "p50_ms": round(percentile(latencies_ms, 50.0), 4),
        "p95_ms": round(percentile(latencies_ms, 95.0), 4),
        "p99_ms": round(percentile(latencies_ms, 99.0), 4),
        "disk_bytes": disk_bytes,
        "engine_stats": stats_line,
    }


def main() -> int:
    parser = argparse.ArgumentParser(description="Run KVForge LSM performance benchmarks.")
    parser.add_argument("--bin", default="./build/kvforge_cli", help="Path to kvforge_cli binary.")
    parser.add_argument(
        "--workloads",
        nargs="+",
        default=["seq_write", "rand_write", "read_heavy", "mixed_zipf"],
        help="Workloads to execute.",
    )
    parser.add_argument("--ops", type=int, default=2000, help="Operations per workload.")
    parser.add_argument("--key-space", type=int, default=1000, help="Distinct key universe size.")
    parser.add_argument("--value-size", type=int, default=128, help="Value payload size in bytes.")
    parser.add_argument("--memtable-kb", type=int, default=64, help="MemTable flush threshold in KiB.")
    parser.add_argument("--sync", action="store_true", help="Enable fsync on every WAL append.")
    parser.add_argument("--json-out", help="Optional path to export JSON benchmark results.")
    args = parser.parse_args()

    binary = Path(args.bin)
    if not binary.exists():
        raise SystemExit(f"Binary not found at {binary}. Build project with CMake first.")

    results: List[Dict[str, object]] = []
    print(f"{'Workload':<14} {'Ops':>7} {'Ops/sec':>10} {'p50(ms)':>9} {'p95(ms)':>9} {'p99(ms)':>9} {'Disk(KB)':>10}")
    print("-" * 74)

    for wl in args.workloads:
        res = run_benchmark(
            binary=binary,
            workload=wl,
            ops=args.ops,
            key_space=args.key_space,
            value_size=args.value_size,
            memtable_kb=args.memtable_kb,
            sync_wal=args.sync,
        )
        results.append(res)
        print(
            f"{res['workload']:<14} {res['operations']:>7} {res['throughput_ops_sec']:>10.1f} "
            f"{res['p50_ms']:>9.4f} {res['p95_ms']:>9.4f} {res['p99_ms']:>9.4f} "
            f"{res['disk_bytes'] / 1024.0:>10.1f}"
        )

    if args.json_out:
        Path(args.json_out).write_text(json.dumps(results, indent=2), encoding="utf-8")
        print(f"\nSaved JSON benchmark report to {args.json_out}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
