#include "kvforge/compactor.h"

#include <queue>
#include <vector>

namespace kvforge {
namespace {

struct HeapItem {
    std::size_t table_idx;   // Lower index == newer SSTable
    std::size_t record_idx;
    const std::string* key;

    bool operator>(const HeapItem& other) const {
        if (*key != *other.key) {
            return *key > *other.key;
        }
        // Prefer lower table_idx (newer record) first
        return table_idx > other.table_idx;
    }
};

}  // namespace

Status Compactor::Compact(const std::vector<const SSTableReader*>& tables_newest_first,
                          const std::string& output_path,
                          const Options& options,
                          bool drop_tombstones,
                          CompactionResult* result) {
    CompactionResult stats;
    stats.input_tables = tables_newest_first.size();

    std::vector<std::vector<KeyValueRecord>> runs(tables_newest_first.size());
    for (std::size_t i = 0; i < tables_newest_first.size(); ++i) {
        Status s = tables_newest_first[i]->ReadAll(&runs[i]);
        if (!s.ok()) {
            return s;
        }
        stats.input_records += runs[i].size();
    }

    std::priority_queue<HeapItem, std::vector<HeapItem>, std::greater<HeapItem>> min_heap;
    for (std::size_t i = 0; i < runs.size(); ++i) {
        if (!runs[i].empty()) {
            min_heap.push(HeapItem{i, 0, &runs[i][0].key});
        }
    }

    std::vector<KeyValueRecord> merged;
    merged.reserve(stats.input_records);

    bool has_last_key = false;
    std::string last_key;

    while (!min_heap.empty()) {
        HeapItem top = min_heap.top();
        min_heap.pop();

        const auto& rec = runs[top.table_idx][top.record_idx];

        if (!has_last_key || rec.key != last_key) {
            last_key = rec.key;
            has_last_key = true;
            if (drop_tombstones && rec.is_tombstone) {
                ++stats.dropped_tombstones;
            } else {
                merged.push_back(rec);
            }
        }

        const std::size_t next_idx = top.record_idx + 1;
        if (next_idx < runs[top.table_idx].size()) {
            min_heap.push(HeapItem{top.table_idx, next_idx, &runs[top.table_idx][next_idx].key});
        }
    }

    stats.output_records = merged.size();
    Status build_status = SSTableBuilder::Build(
        output_path,
        merged,
        options.bloom_bits_per_key,
        options.sparse_index_interval);

    if (build_status.ok() && result != nullptr) {
        *result = stats;
    }
    return build_status;
}

}  // namespace kvforge
