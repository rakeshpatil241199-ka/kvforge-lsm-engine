#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "kvforge/bloom_filter.h"
#include "kvforge/memtable.h"
#include "kvforge/status.h"

namespace kvforge {

class SSTableBuilder {
public:
    static constexpr std::uint64_t kMagicNumber = 0x4B56464F52474531ULL;  // "KVFORGE1"
    static constexpr std::size_t kFooterSize = 32;

    static Status Build(const std::string& path,
                        const std::vector<KeyValueRecord>& sorted_records,
                        std::size_t bloom_bits_per_key,
                        std::size_t sparse_index_interval);
};

struct SparseIndexEntry {
    std::string key;
    std::uint64_t offset = 0;
};

class SSTableReader {
public:
    static Status Open(const std::string& path,
                       std::uint64_t sequence_id,
                       int level,
                       std::unique_ptr<SSTableReader>* out);

    // Searches for `key`. Optionally reports whether the lookup was short-circuited
    // by the in-memory Bloom filter without touching disk.
    Status Get(const std::string& key,
               std::string* value,
               bool* is_tombstone,
               bool* bloom_rejected = nullptr) const;

    Status ReadAll(std::vector<KeyValueRecord>* out) const;

    const std::string& path() const { return path_; }
    std::uint64_t sequence_id() const { return sequence_id_; }
    int level() const { return level_; }
    std::uint32_t entry_count() const { return entry_count_; }
    std::uint64_t file_size() const { return file_size_; }

private:
    SSTableReader(std::string path, std::uint64_t seq, int level)
        : path_(std::move(path)), sequence_id_(seq), level_(level) {}

    std::string path_;
    std::uint64_t sequence_id_ = 0;
    int level_ = 0;
    std::uint64_t index_offset_ = 0;
    std::uint64_t bloom_offset_ = 0;
    std::uint32_t entry_count_ = 0;
    std::uint64_t file_size_ = 0;
    std::vector<SparseIndexEntry> sparse_index_;
    BloomFilter bloom_filter_;
};

}  // namespace kvforge
