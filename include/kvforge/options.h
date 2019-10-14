#pragma once

#include <cstddef>
#include <cstdint>

namespace kvforge {

struct Options {
    // Flush MemTable to an L0 SSTable once memory usage reaches this threshold.
    std::size_t memtable_max_bytes = 4 * 1024 * 1024;  // 4 MiB

    // Number of bits per key allocated in SSTable Bloom filters (~1% FP at 10 bits).
    std::size_t bloom_bits_per_key = 10;

    // Sparse index sampling interval (1 index entry per N keys).
    std::size_t sparse_index_interval = 16;

    // Number of L0 SSTables that triggers L0 -> L1 merge compaction.
    std::size_t l0_compaction_trigger = 4;

    // Issue OS fsync/FlushFileBuffers on every WAL append when true.
    bool sync_wal = true;

    // When true, partial/truncated tail WAL records from sudden power loss are
    // logged and discarded instead of failing database open.
    bool recover_truncated_wal_tail = true;
};

}  // namespace kvforge
