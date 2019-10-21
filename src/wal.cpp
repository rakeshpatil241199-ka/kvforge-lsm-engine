#include "kvforge/wal.h"

#include <cstring>
#include <vector>

#include "kvforge/crc32.h"

#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

namespace kvforge {
namespace {

constexpr std::size_t kWalHeaderBytes = 13;  // 4(crc) + 1(op) + 4(klen) + 4(vlen)

void EncodeFixed32(std::uint8_t* dst, std::uint32_t val) {
    dst[0] = static_cast<std::uint8_t>(val & 0xFFU);
    dst[1] = static_cast<std::uint8_t>((val >> 8U) & 0xFFU);
    dst[2] = static_cast<std::uint8_t>((val >> 16U) & 0xFFU);
    dst[3] = static_cast<std::uint8_t>((val >> 24U) & 0xFFU);
}

std::uint32_t DecodeFixed32(const std::uint8_t* src) {
    return static_cast<std::uint32_t>(src[0]) |
           (static_cast<std::uint32_t>(src[1]) << 8U) |
           (static_cast<std::uint32_t>(src[2]) << 16U) |
           (static_cast<std::uint32_t>(src[3]) << 24U);
}

}  // namespace

WriteAheadLog::WriteAheadLog(std::string path, bool sync_on_write)
    : path_(std::move(path)), sync_on_write_(sync_on_write) {}

WriteAheadLog::~WriteAheadLog() {
    Close();
}

Status WriteAheadLog::Open() {
    if (file_ != nullptr) {
        return Status::Ok();
    }
    file_ = std::fopen(path_.c_str(), "ab+");
    if (file_ == nullptr) {
        return Status::IOError("Failed to open WAL file: " + path_);
    }
    if (std::fseek(file_, 0, SEEK_END) == 0) {
        long pos = std::ftell(file_);
        if (pos >= 0) {
            bytes_written_ = static_cast<std::uint64_t>(pos);
        }
    }
    return Status::Ok();
}

Status WriteAheadLog::Close() {
    if (file_ != nullptr) {
        std::fflush(file_);
        std::fclose(file_);
        file_ = nullptr;
    }
    return Status::Ok();
}

Status WriteAheadLog::SyncFile() {
    if (file_ == nullptr) {
        return Status::IOError("WAL file is not open");
    }
    if (std::fflush(file_) != 0) {
        return Status::IOError("Failed to flush WAL buffer");
    }
    if (!sync_on_write_) {
        return Status::Ok();
    }
#ifdef _WIN32
    int fd = _fileno(file_);
    if (fd < 0 || _commit(fd) != 0) {
        return Status::IOError("WAL _commit failed");
    }
#else
    int fd = fileno(file_);
    if (fd < 0 || ::fsync(fd) != 0) {
        return Status::IOError("WAL fsync failed");
    }
#endif
    return Status::Ok();
}

Status WriteAheadLog::Append(WalOpType op, const std::string& key, const std::string& value) {
    if (file_ == nullptr) {
        Status s = Open();
        if (!s.ok()) return s;
    }

    const std::size_t payload_size = 1 + 4 + 4 + key.size() + value.size();
    std::vector<std::uint8_t> frame(4 + payload_size);

    frame[4] = static_cast<std::uint8_t>(op);
    EncodeFixed32(frame.data() + 5, static_cast<std::uint32_t>(key.size()));
    EncodeFixed32(frame.data() + 9, static_cast<std::uint32_t>(value.size()));

    if (!key.empty()) {
        std::memcpy(frame.data() + kWalHeaderBytes, key.data(), key.size());
    }
    if (!value.empty()) {
        std::memcpy(frame.data() + kWalHeaderBytes + key.size(), value.data(), value.size());
    }

    const std::uint32_t crc = Crc32::Compute(frame.data() + 4, payload_size);
    EncodeFixed32(frame.data(), crc);

    if (std::fwrite(frame.data(), 1, frame.size(), file_) != frame.size()) {
        return Status::IOError("Failed to write record to WAL");
    }

    bytes_written_ += frame.size();
    return SyncFile();
}

Status WriteAheadLog::Reset() {
    Close();
    file_ = std::fopen(path_.c_str(), "wb+");
    if (file_ == nullptr) {
        return Status::IOError("Failed to reset WAL file: " + path_);
    }
    bytes_written_ = 0;
    return SyncFile();
}

Status WriteAheadLog::Replay(const ReplayCallback& callback, bool /*allow_truncated_tail*/) {
    std::FILE* reader = std::fopen(path_.c_str(), "rb");
    if (reader == nullptr) {
        return Status::Ok();
    }

    std::uint8_t header[kWalHeaderBytes];
    while (true) {
        const std::size_t n = std::fread(header, 1, kWalHeaderBytes, reader);
        if (n == 0) {
            break;
        }
        if (n < kWalHeaderBytes) {
            std::fclose(reader);
            return Status::Corruption("Incomplete WAL record header");
        }

        const std::uint32_t stored_crc = DecodeFixed32(header);
        const std::uint8_t op_byte = header[4];
        const std::uint32_t key_len = DecodeFixed32(header + 5);
        const std::uint32_t val_len = DecodeFixed32(header + 9);

        if (op_byte != static_cast<std::uint8_t>(WalOpType::kPut) &&
            op_byte != static_cast<std::uint8_t>(WalOpType::kDelete)) {
            std::fclose(reader);
            return Status::Corruption("Unknown WAL operation code");
        }

        std::string key(key_len, '\0');
        std::string value(val_len, '\0');

        if (key_len > 0 && std::fread(&key[0], 1, key_len, reader) != key_len) {
            std::fclose(reader);
            return Status::Corruption("Truncated WAL key payload");
        }
        if (val_len > 0 && std::fread(&value[0], 1, val_len, reader) != val_len) {
            std::fclose(reader);
            return Status::Corruption("Truncated WAL value payload");
        }

        std::uint32_t computed_crc = Crc32::Update(0U, header + 4, 9);
        if (!key.empty()) {
            computed_crc = Crc32::Update(computed_crc, key.data(), key.size());
        }
        if (!value.empty()) {
            computed_crc = Crc32::Update(computed_crc, value.data(), value.size());
        }

        if (computed_crc != stored_crc) {
            std::fclose(reader);
            return Status::Corruption("WAL CRC32 checksum mismatch");
        }

        callback(WalRecord{static_cast<WalOpType>(op_byte), std::move(key), std::move(value)});
    }

    std::fclose(reader);
    return Status::Ok();
}

}  // namespace kvforge
