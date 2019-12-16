#include "kvforge/db.h"

#include <algorithm>
#include <filesystem>
#include <iomanip>
#include <mutex>
#include <sstream>

namespace fs = std::filesystem;

namespace kvforge {
namespace {

bool ParseTableFilename(const std::string& filename, std::uint64_t* seq, int* level) {
    // Expected format: 000001_L0.sst
    if (filename.size() < 11 || filename.substr(filename.size() - 4) != ".sst") {
        return false;
    }
    const auto sep = filename.find("_L");
    if (sep == std::string::npos || sep == 0) {
        return false;
    }
    try {
        *seq = std::stoull(filename.substr(0, sep));
        *level = std::stoi(filename.substr(sep + 2, filename.size() - sep - 6));
        return (*level == 0 || *level == 1);
    } catch (...) {
        return false;
    }
}

}  // namespace

DB::DB(std::string db_dir, Options options)
    : db_dir_(std::move(db_dir)), options_(options) {}

DB::~DB() {
    if (wal_) {
        wal_->Close();
    }
}

std::string DB::FormatTablePath(std::uint64_t seq, int level) const {
    std::ostringstream oss;
    oss << std::setw(6) << std::setfill('0') << seq << "_L" << level << ".sst";
    return (fs::path(db_dir_) / oss.str()).string();
}

Status DB::Open(const std::string& db_dir, const Options& options, std::unique_ptr<DB>* out) {
    if (out == nullptr) {
        return Status::InvalidArgument("Null output DB pointer");
    }
    std::error_code ec;
    fs::create_directories(db_dir, ec);
    if (ec) {
        return Status::IOError("Failed creating DB directory: " + ec.message());
    }

    std::unique_ptr<DB> db(new DB(db_dir, options));
    Status s = db->Recover();
    if (!s.ok()) {
        return s;
    }

    *out = std::move(db);
    return Status::Ok();
}

Status DB::Recover() {
    // 1. Discover and open existing SSTables
    std::uint64_t max_seq = 0;
    for (const auto& entry : fs::directory_iterator(db_dir_)) {
        if (!entry.is_regular_file()) continue;
        const std::string fname = entry.path().filename().string();
        std::uint64_t seq = 0;
        int level = 0;
        if (!ParseTableFilename(fname, &seq, &level)) continue;

        std::unique_ptr<SSTableReader> reader;
        Status s = SSTableReader::Open(entry.path().string(), seq, level, &reader);
        if (!s.ok()) {
            return s;
        }
        max_seq = std::max(max_seq, seq);
        if (level == 0) {
            l0_tables_.push_back(std::move(reader));
        } else {
            l1_tables_.push_back(std::move(reader));
        }
    }

    auto newest_first = [](const std::unique_ptr<SSTableReader>& a,
                           const std::unique_ptr<SSTableReader>& b) {
        return a->sequence_id() > b->sequence_id();
    };
    std::sort(l0_tables_.begin(), l0_tables_.end(), newest_first);
    std::sort(l1_tables_.begin(), l1_tables_.end(), newest_first);
    next_seq_ = max_seq + 1;

    // 2. Replay Write-Ahead Log into MemTable
    const std::string wal_path = (fs::path(db_dir_) / "wal.log").string();
    wal_ = std::make_unique<WriteAheadLog>(wal_path, options_.sync_wal);

    Status replay_status = wal_->Replay(
        [this](const WalRecord& rec) {
            if (rec.op == WalOpType::kPut) {
                memtable_.Put(rec.key, rec.value);
            } else if (rec.op == WalOpType::kDelete) {
                memtable_.Delete(rec.key);
            }
        },
        options_.recover_truncated_wal_tail);

    if (!replay_status.ok()) {
        return replay_status;
    }

    return wal_->Open();
}

Status DB::Put(const std::string& key, const std::string& value) {
    if (key.empty()) {
        return Status::InvalidArgument("Key cannot be empty");
    }
    std::unique_lock<std::shared_mutex> lock(mu_);

    Status s = wal_->Append(WalOpType::kPut, key, value);
    if (!s.ok()) {
        return s;
    }

    memtable_.Put(key, value);
    ++total_puts_;

    if (memtable_.ApproxMemoryUsage() >= options_.memtable_max_bytes) {
        return FlushLocked();
    }
    return Status::Ok();
}

Status DB::Delete(const std::string& key) {
    if (key.empty()) {
        return Status::InvalidArgument("Key cannot be empty");
    }
    std::unique_lock<std::shared_mutex> lock(mu_);

    Status s = wal_->Append(WalOpType::kDelete, key, "");
    if (!s.ok()) {
        return s;
    }

    memtable_.Delete(key);
    ++total_deletes_;

    if (memtable_.ApproxMemoryUsage() >= options_.memtable_max_bytes) {
        return FlushLocked();
    }
    return Status::Ok();
}

Status DB::Get(const std::string& key, std::string* value) const {
    if (key.empty()) {
        return Status::InvalidArgument("Key cannot be empty");
    }
    std::shared_lock<std::shared_mutex> lock(mu_);
    ++total_gets_;

    // 1. Check active MemTable
    bool is_tombstone = false;
    if (memtable_.Get(key, value, &is_tombstone)) {
        return is_tombstone ? Status::NotFound() : Status::Ok();
    }

    // 2. Search L0 SSTables (newest first)
    for (const auto& sst : l0_tables_) {
        Status s = sst->Get(key, value, &is_tombstone);
        if (s.ok()) {
            return is_tombstone ? Status::NotFound() : Status::Ok();
        }
        if (!s.IsNotFound()) {
            return s;
        }
    }

    // 3. Search L1 SSTables (newest first)
    for (const auto& sst : l1_tables_) {
        Status s = sst->Get(key, value, &is_tombstone);
        if (s.ok()) {
            return is_tombstone ? Status::NotFound() : Status::Ok();
        }
        if (!s.IsNotFound()) {
            return s;
        }
    }

    return Status::NotFound();
}

Status DB::Flush() {
    std::unique_lock<std::shared_mutex> lock(mu_);
    return FlushLocked();
}

Status DB::FlushLocked() {
    if (memtable_.Empty()) {
        return Status::Ok();
    }

    const std::uint64_t seq = next_seq_++;
    const std::string path = FormatTablePath(seq, 0);
    const auto sorted = memtable_.DumpSorted();

    Status s = SSTableBuilder::Build(
        path, sorted, options_.bloom_bits_per_key, options_.sparse_index_interval);
    if (!s.ok()) {
        return s;
    }

    std::unique_ptr<SSTableReader> reader;
    s = SSTableReader::Open(path, seq, 0, &reader);
    if (!s.ok()) {
        return s;
    }

    l0_tables_.insert(l0_tables_.begin(), std::move(reader));
    memtable_.Clear();
    ++total_flushes_;

    s = wal_->Reset();
    if (!s.ok()) {
        return s;
    }

    if (l0_tables_.size() >= options_.l0_compaction_trigger) {
        return CompactLocked(nullptr);
    }
    return Status::Ok();
}

Status DB::Compact(CompactionResult* result) {
    std::unique_lock<std::shared_mutex> lock(mu_);
    return CompactLocked(result);
}

Status DB::CompactLocked(CompactionResult* result) {
    if (l0_tables_.empty() && l1_tables_.size() <= 1) {
        return Status::Ok();
    }

    std::vector<const SSTableReader*> inputs;
    inputs.reserve(l0_tables_.size() + l1_tables_.size());
    for (const auto& t : l0_tables_) inputs.push_back(t.get());
    for (const auto& t : l1_tables_) inputs.push_back(t.get());

    const std::uint64_t new_seq = next_seq_++;
    const std::string out_path = FormatTablePath(new_seq, 1);

    CompactionResult local_res;
    Status s = Compactor::Compact(inputs, out_path, options_, true, &local_res);
    if (!s.ok()) {
        return s;
    }

    std::unique_ptr<SSTableReader> compacted_reader;
    s = SSTableReader::Open(out_path, new_seq, 1, &compacted_reader);
    if (!s.ok()) {
        return s;
    }

    std::vector<std::string> obsolete_paths;
    for (const auto& t : l0_tables_) obsolete_paths.push_back(t->path());
    for (const auto& t : l1_tables_) obsolete_paths.push_back(t->path());

    l0_tables_.clear();
    l1_tables_.clear();
    if (local_res.output_records > 0) {
        l1_tables_.push_back(std::move(compacted_reader));
    } else {
        obsolete_paths.push_back(out_path);
    }

    for (const auto& old_path : obsolete_paths) {
        std::error_code ec;
        fs::remove(old_path, ec);
    }

    ++total_compactions_;
    if (result != nullptr) {
        *result = local_res;
    }
    return Status::Ok();
}

DBStats DB::GetStats() const {
    std::shared_lock<std::shared_mutex> lock(mu_);
    DBStats st;
    st.memtable_entries = memtable_.EntryCount();
    st.memtable_bytes = memtable_.ApproxMemoryUsage();
    st.l0_table_count = l0_tables_.size();
    st.l1_table_count = l1_tables_.size();
    st.total_puts = total_puts_;
    st.total_deletes = total_deletes_;
    st.total_gets = total_gets_;
    st.total_flushes = total_flushes_;
    st.total_compactions = total_compactions_;
    for (const auto& t : l0_tables_) st.total_disk_bytes += t->file_size();
    for (const auto& t : l1_tables_) st.total_disk_bytes += t->file_size();
    return st;
}

}  // namespace kvforge
