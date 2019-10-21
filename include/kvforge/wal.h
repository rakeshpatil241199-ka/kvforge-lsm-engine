#pragma once

#include <cstdint>
#include <cstdio>
#include <functional>
#include <string>

#include "kvforge/status.h"

namespace kvforge {

enum class WalOpType : std::uint8_t {
    kPut = 1,
    kDelete = 2
};

struct WalRecord {
    WalOpType op;
    std::string key;
    std::string value;
};

// Binary on-disk record format (little-endian):
//   [uint32_t crc32][uint8_t op_type][uint32_t key_len][uint32_t val_len][key][value]
// CRC32 covers [op_type..value].
class WriteAheadLog {
public:
    WriteAheadLog(std::string path, bool sync_on_write);
    ~WriteAheadLog();

    WriteAheadLog(const WriteAheadLog&) = delete;
    WriteAheadLog& operator=(const WriteAheadLog&) = delete;

    Status Open();
    Status Close();
    Status Append(WalOpType op, const std::string& key, const std::string& value);
    Status Reset();

    using ReplayCallback = std::function<void(const WalRecord&)>;
    Status Replay(const ReplayCallback& callback, bool allow_truncated_tail = true);

    const std::string& path() const { return path_; }
    std::uint64_t bytes_written() const { return bytes_written_; }

private:
    Status SyncFile();

    std::string path_;
    bool sync_on_write_;
    std::FILE* file_ = nullptr;
    std::uint64_t bytes_written_ = 0;
};

}  // namespace kvforge
