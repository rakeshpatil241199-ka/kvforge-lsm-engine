#pragma once

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

#include "kvforge/options.h"
#include "kvforge/sstable.h"
#include "kvforge/status.h"

namespace kvforge {

struct CompactionResult {
    std::size_t input_tables = 0;
    std::size_t input_records = 0;
    std::size_t output_records = 0;
    std::size_t dropped_tombstones = 0;
};

class Compactor {
public:
    // Merges input SSTables (ordered newest-to-oldest by sequence_id) into a single
    // deduplicated SSTable at `output_path`. When `drop_tombstones` is true (bottommost
    // level compaction), deleted keys are physically reclaimed.
    static Status Compact(const std::vector<const SSTableReader*>& tables_newest_first,
                          const std::string& output_path,
                          const Options& options,
                          bool drop_tombstones,
                          CompactionResult* result = nullptr);
};

}  // namespace kvforge
