#!/usr/bin/env python3
"""Binary WAL and SSTable Inspector for KVForge LSM Engine.

Decodes on-disk little-endian frames, verifies IEEE CRC32 checksums, detects
truncated crash tails, and audits SSTable footers.
"""

import argparse
import json
import struct
import sys
import zlib
from pathlib import Path
from typing import Any, Dict, List

WAL_HEADER_FMT = "<IBII"  # crc32, op_type, key_len, val_len
WAL_HEADER_SIZE = struct.calcsize(WAL_HEADER_FMT)
SST_FOOTER_FMT = "<QQIIQ"  # index_offset, bloom_offset, entry_count, footer_crc, magic
SST_FOOTER_SIZE = struct.calcsize(SST_FOOTER_FMT)
SST_MAGIC = 0x4B56464F52474531

OP_NAMES = {1: "PUT", 2: "DELETE"}


def inspect_wal(path: Path) -> Dict[str, Any]:
    """Parse a KVForge WAL file and verify per-record CRC32 integrity."""
    raw = path.read_bytes()
    offset = 0
    total_bytes = len(raw)
    records: List[Dict[str, Any]] = []
    errors: List[str] = []
    truncated_tail_bytes = 0

    while offset < total_bytes:
        remaining = total_bytes - offset
        if remaining < WAL_HEADER_SIZE:
            truncated_tail_bytes = remaining
            errors.append(f"Truncated header at byte {offset} ({remaining} trailing bytes)")
            break

        stored_crc, op_code, klen, vlen = struct.unpack_from(WAL_HEADER_FMT, raw, offset)
        payload_start = offset + WAL_HEADER_SIZE
        payload_end = payload_start + klen + vlen

        if payload_end > total_bytes:
            truncated_tail_bytes = remaining
            errors.append(
                f"Truncated payload at byte {offset} (needed {WAL_HEADER_SIZE + klen + vlen}B, have {remaining}B)"
            )
            break

        crc_body = raw[offset + 4 : payload_end]
        computed_crc = zlib.crc32(crc_body) & 0xFFFFFFFF
        crc_ok = computed_crc == stored_crc

        key_bytes = raw[payload_start : payload_start + klen]
        val_bytes = raw[payload_start + klen : payload_end]

        records.append(
            {
                "offset": offset,
                "op": OP_NAMES.get(op_code, f"UNKNOWN({op_code})"),
                "key": key_bytes.decode("utf-8", errors="replace"),
                "value_len": vlen,
                "crc_valid": crc_ok,
                "stored_crc": f"0x{stored_crc:08X}",
                "computed_crc": f"0x{computed_crc:08X}",
            }
        )
        if not crc_ok:
            errors.append(
                f"CRC mismatch at offset {offset}: stored=0x{stored_crc:08X}, computed=0x{computed_crc:08X}"
            )
        offset = payload_end

    return {
        "file": str(path),
        "size_bytes": total_bytes,
        "record_count": len(records),
        "valid_crc_count": sum(1 for r in records if r["crc_valid"]),
        "truncated_tail_bytes": truncated_tail_bytes,
        "healthy": len(errors) == 0,
        "errors": errors,
        "records": records,
    }


def inspect_sstable(path: Path) -> Dict[str, Any]:
    """Validate and decode a KVForge SSTable 32-byte footer."""
    raw = path.read_bytes()
    size = len(raw)
    if size < SST_FOOTER_SIZE:
        return {
            "file": str(path),
            "size_bytes": size,
            "healthy": False,
            "error": "File smaller than 32-byte SSTable footer",
        }

    footer = raw[-SST_FOOTER_SIZE:]
    index_offset, bloom_offset, entry_count, stored_crc, magic = struct.unpack(SST_FOOTER_FMT, footer)
    computed_crc = zlib.crc32(footer[:20]) & 0xFFFFFFFF
    magic_ok = magic == SST_MAGIC
    crc_ok = computed_crc == stored_crc

    return {
        "file": str(path),
        "size_bytes": size,
        "entry_count": entry_count,
        "data_section_bytes": index_offset,
        "sparse_index_bytes": bloom_offset - index_offset,
        "bloom_section_bytes": (size - SST_FOOTER_SIZE) - bloom_offset,
        "magic_valid": magic_ok,
        "footer_crc_valid": crc_ok,
        "healthy": magic_ok and crc_ok,
    }


def main() -> int:
    parser = argparse.ArgumentParser(description="Inspect KVForge binary WAL and SSTable files.")
    parser.add_argument("target", help="Path to a wal.log file, .sst file, or database directory.")
    parser.add_argument("--json", action="store_true", help="Emit structured JSON output.")
    parser.add_argument("--limit", type=int, default=20, help="Max WAL records to display in text mode.")
    args = parser.parse_args()

    target = Path(args.target)
    if not target.exists():
        print(f"Error: path not found: {target}", file=sys.stderr)
        return 1

    report: Dict[str, Any] = {"wal": None, "sstables": []}
    if target.is_dir():
        wal_file = target / "wal.log"
        if wal_file.exists():
            report["wal"] = inspect_wal(wal_file)
        for sst in sorted(target.glob("*.sst")):
            report["sstables"].append(inspect_sstable(sst))
    elif target.suffix == ".sst":
        report["sstables"].append(inspect_sstable(target))
    else:
        report["wal"] = inspect_wal(target)

    if args.json:
        print(json.dumps(report, indent=2))
        return 0

    if report["wal"] is not None:
        w = report["wal"]
        print(f"=== WAL Inspection: {w['file']} ===")
        print(
            f"Size: {w['size_bytes']} B | Records: {w['record_count']} "
            f"(Valid CRC: {w['valid_crc_count']}) | Healthy: {w['healthy']}"
        )
        for err in w["errors"]:
            print(f"  [WARN] {err}")
        for rec in w["records"][: args.limit]:
            status = "OK" if rec["crc_valid"] else "CORRUPT"
            print(
                f"  @{rec['offset']:>6} | {rec['op']:<6} | key={rec['key']:<24} "
                f"| val_len={rec['value_len']:<5} | {status}"
            )

    if report["sstables"]:
        print("\n=== SSTable Footer Audit ===")
        for s in report["sstables"]:
            if "error" in s:
                print(f"  {s['file']}: INVALID ({s['error']})")
            else:
                print(
                    f"  {Path(s['file']).name:<18} | entries={s['entry_count']:<6} "
                    f"| size={s['size_bytes']:<8}B | bloom={s['bloom_section_bytes']:<6}B "
                    f"| healthy={s['healthy']}"
                )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
