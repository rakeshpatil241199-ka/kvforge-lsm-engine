#pragma once

#include <cstddef>
#include <map>
#include <string>
#include <vector>

namespace kvforge {

struct MemTableEntry {
    std::string value;
    bool is_tombstone = false;
};

struct KeyValueRecord {
    std::string key;
    std::string value;
    bool is_tombstone = false;
};

// Ordered in-memory buffer backed by std::map so flushes produce sorted SSTables in O(N).
class MemTable {
public:
    MemTable() = default;

    void Put(const std::string& key, const std::string& value);
    void Delete(const std::string& key);

    // Returns true if the key exists in the MemTable (including tombstones).
    bool Get(const std::string& key, std::string* value, bool* is_tombstone) const;

    // Exports all entries in sorted key order for SSTable flushing.
    std::vector<KeyValueRecord> DumpSorted() const;

    void Clear();

    std::size_t ApproxMemoryUsage() const { return approx_bytes_; }
    std::size_t EntryCount() const { return table_.size(); }
    bool Empty() const { return table_.empty(); }

private:
    static constexpr std::size_t kNodeOverheadBytes = 48;

    std::map<std::string, MemTableEntry> table_;
    std::size_t approx_bytes_ = 0;
};

}  // namespace kvforge
