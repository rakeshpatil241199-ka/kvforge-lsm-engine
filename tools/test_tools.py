import struct
import tempfile
import unittest
import zlib
from pathlib import Path

from benchmark import ZipfGenerator, percentile
from wal_inspector import SST_FOOTER_FMT, SST_MAGIC, WAL_HEADER_FMT, inspect_sstable, inspect_wal


def build_wal_frame(op: int, key: bytes, value: bytes) -> bytes:
    body = struct.pack("<BII", op, len(key), len(value)) + key + value
    crc = zlib.crc32(body) & 0xFFFFFFFF
    return struct.pack("<I", crc) + body


class TestSystemsTools(unittest.TestCase):
    def test_wal_inspector_valid_and_truncated_tail(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            wal = Path(td) / "wal.log"
            data = build_wal_frame(1, b"acct:10", b"balance=500") + build_wal_frame(2, b"acct:10", b"")
            wal.write_bytes(data)

            res = inspect_wal(wal)
            self.assertTrue(res["healthy"])
            self.assertEqual(res["record_count"], 2)
            self.assertEqual(res["valid_crc_count"], 2)

            # Append 5 partial tail bytes simulating crash
            wal.write_bytes(data + b"\xAA\xBB\xCC\xDD\x01")
            res_tail = inspect_wal(wal)
            self.assertFalse(res_tail["healthy"])
            self.assertEqual(res_tail["truncated_tail_bytes"], 5)
            self.assertEqual(res_tail["record_count"], 2)

    def test_sstable_footer_verification(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            sst = Path(td) / "000001_L0.sst"
            body = struct.pack("<QQI", 128, 256, 42)
            crc = zlib.crc32(body) & 0xFFFFFFFF
            footer = struct.pack(SST_FOOTER_FMT, 128, 256, 42, crc, SST_MAGIC)
            sst.write_bytes(b"\x00" * 300 + footer)

            res = inspect_sstable(sst)
            self.assertTrue(res["healthy"])
            self.assertEqual(res["entry_count"], 42)

    def test_zipf_and_percentile(self) -> None:
        zipf = ZipfGenerator(100, skew=1.1, seed=7)
        samples = [zipf.sample() for _ in range(1000)]
        hot_hits = sum(1 for s in samples if s < 10)
        self.assertGreater(hot_hits, 450)
        self.assertAlmostEqual(percentile([1.0, 2.0, 3.0, 4.0, 5.0], 50.0), 3.0)


if __name__ == "__main__":
    unittest.main()
