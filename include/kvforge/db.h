#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <shared_mutex>
#include <string>
#include <vector>

#include "kvforge/compactor.h"
#include "kvforge/memtable.h"
#include "kvforge/options.h"
#include "kvforge/sstable.h"
#include "kvforge/status.h"
#include "kvforge/wal.h"

namespace kvforge {

struct DBStats {
    std::size_t memtable_entries = 0;
    std::size_t memtable_bytes = 0;
    std::size_t l0_table_count = 0;
    std::size_t l1_table_count = 0;
    std::uint64_t total_puts = 0;
    std::uint64_t total_deletes = 0;
    std::uint64_t total_gets = 0;
    std::uint64_t bloom_filter_skips = 0;
    std::uint64_t total_flushes = 0;
    std::uint64_t total_compactions = 0;
    std::uint64_t total_disk_bytes = 0;
};

class DB {
public:
    static Status Open(const std::string& db_dir, const Options& options, std::unique_ptr<DB>* out);
    ~DB();

    DB(const DB&) = delete;
    DB& operator=(const DB&) = delete;

    Status Put(const std::string& key, const std::string& value);
    Status Delete(const std::string& key);
    Status Get(const std::string& key, std::string* value) const;

    Status Flush();
    Status Compact(CompactionResult* result = nullptr);
    DBStats GetStats() const;

private:
    DB(std::string db_dir, Options options);

    Status Recover();
    Status FlushLocked();
    Status CompactLocked(CompactionResult* result);
    std::string FormatTablePath(std::uint64_t seq, int level) const;

    std::string db_dir_;
    Options options_;
    mutable std::shared_mutex mu_;

    std::unique_ptr<WriteAheadLog> wal_;
    MemTable memtable_;
    std::vector<std::unique_ptr<SSTableReader>> l0_tables_;
    std::vector<std::unique_ptr<SSTableReader>> l1_tables_;
    std::uint64_t next_seq_ = 1;

    mutable std::atomic<std::uint64_t> total_gets_{0};
    mutable std::atomic<std::uint64_t> bloom_filter_skips_{0};
    std::uint64_t total_puts_ = 0;
    std::uint64_t total_deletes_ = 0;
    std::uint64_t total_flushes_ = 0;
    std::uint64_t total_compactions_ = 0;
};

}  // namespace kvforge
