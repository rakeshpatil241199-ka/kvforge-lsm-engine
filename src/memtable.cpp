#include "kvforge/memtable.h"

namespace kvforge {

void MemTable::Put(const std::string& key, const std::string& value) {
    auto it = table_.find(key);
    if (it != table_.end()) {
        if (approx_bytes_ >= it->second.value.size()) {
            approx_bytes_ -= it->second.value.size();
        }
        it->second.value = value;
        it->second.is_tombstone = false;
        approx_bytes_ += value.size();
    } else {
        table_.emplace(key, MemTableEntry{value, false});
        approx_bytes_ += key.size() + value.size() + kNodeOverheadBytes;
    }
}

void MemTable::Delete(const std::string& key) {
    auto it = table_.find(key);
    if (it != table_.end()) {
        if (approx_bytes_ >= it->second.value.size()) {
            approx_bytes_ -= it->second.value.size();
        }
        it->second.value.clear();
        it->second.is_tombstone = true;
    } else {
        table_.emplace(key, MemTableEntry{"", true});
        approx_bytes_ += key.size() + kNodeOverheadBytes;
    }
}

bool MemTable::Get(const std::string& key, std::string* value, bool* is_tombstone) const {
    auto it = table_.find(key);
    if (it == table_.end()) {
        return false;
    }
    if (is_tombstone != nullptr) {
        *is_tombstone = it->second.is_tombstone;
    }
    if (!it->second.is_tombstone && value != nullptr) {
        *value = it->second.value;
    }
    return true;
}

std::vector<KeyValueRecord> MemTable::DumpSorted() const {
    std::vector<KeyValueRecord> out;
    out.reserve(table_.size());
    for (const auto& kv : table_) {
        out.push_back(KeyValueRecord{kv.first, kv.second.value, kv.second.is_tombstone});
    }
    return out;
}

void MemTable::Clear() {
    table_.clear();
    approx_bytes_ = 0;
}

}  // namespace kvforge
