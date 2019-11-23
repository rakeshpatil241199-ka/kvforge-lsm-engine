#include "kvforge/sstable.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>

#include "kvforge/crc32.h"

namespace kvforge {
namespace {

void PutU32(std::string& dst, std::uint32_t v) {
    dst.push_back(static_cast<char>(v & 0xFFU));
    dst.push_back(static_cast<char>((v >> 8U) & 0xFFU));
    dst.push_back(static_cast<char>((v >> 16U) & 0xFFU));
    dst.push_back(static_cast<char>((v >> 24U) & 0xFFU));
}

void PutU64(std::string& dst, std::uint64_t v) {
    for (int i = 0; i < 8; ++i) {
        dst.push_back(static_cast<char>((v >> (8 * i)) & 0xFFU));
    }
}

std::uint32_t GetU32(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0]) |
           (static_cast<std::uint32_t>(p[1]) << 8U) |
           (static_cast<std::uint32_t>(p[2]) << 16U) |
           (static_cast<std::uint32_t>(p[3]) << 24U);
}

std::uint64_t GetU64(const std::uint8_t* p) {
    std::uint64_t v = 0;
    for (int i = 0; i < 8; ++i) {
        v |= (static_cast<std::uint64_t>(p[i]) << (8 * i));
    }
    return v;
}

}  // namespace

Status SSTableBuilder::Build(const std::string& path,
                             const std::vector<KeyValueRecord>& sorted_records,
                             std::size_t bloom_bits_per_key,
                             std::size_t sparse_index_interval) {
    const std::string tmp_path = path + ".tmp";
    std::ofstream out(tmp_path, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) {
        return Status::IOError("Failed to open temp SSTable for writing: " + tmp_path);
    }

    const std::size_t interval = std::max<std::size_t>(sparse_index_interval, 1);
    BloomFilter bloom(sorted_records.size(), bloom_bits_per_key);
    std::vector<SparseIndexEntry> index_entries;

    std::uint64_t current_offset = 0;
    for (std::size_t i = 0; i < sorted_records.size(); ++i) {
        const auto& rec = sorted_records[i];
        if (i % interval == 0) {
            index_entries.push_back(SparseIndexEntry{rec.key, current_offset});
        }
        bloom.Add(rec.key);

        std::string encoded;
        encoded.reserve(9 + rec.key.size() + rec.value.size());
        encoded.push_back(static_cast<char>(rec.is_tombstone ? 1 : 0));
        PutU32(encoded, static_cast<std::uint32_t>(rec.key.size()));
        PutU32(encoded, static_cast<std::uint32_t>(rec.value.size()));
        encoded.append(rec.key);
        encoded.append(rec.value);

        out.write(encoded.data(), static_cast<std::streamsize>(encoded.size()));
        if (!out.good()) {
            return Status::IOError("Failed writing data block to SSTable: " + tmp_path);
        }
        current_offset += encoded.size();
    }

    // Write sparse index section
    const std::uint64_t index_offset = current_offset;
    std::string index_buf;
    PutU32(index_buf, static_cast<std::uint32_t>(index_entries.size()));
    for (const auto& idx : index_entries) {
        PutU32(index_buf, static_cast<std::uint32_t>(idx.key.size()));
        index_buf.append(idx.key);
        PutU64(index_buf, idx.offset);
    }
    out.write(index_buf.data(), static_cast<std::streamsize>(index_buf.size()));
    current_offset += index_buf.size();

    // Write bloom filter section
    const std::uint64_t bloom_offset = current_offset;
    const std::string serialized_bloom = bloom.Serialize();
    std::string bloom_header;
    PutU32(bloom_header, static_cast<std::uint32_t>(serialized_bloom.size()));
    out.write(bloom_header.data(), static_cast<std::streamsize>(bloom_header.size()));
    out.write(serialized_bloom.data(), static_cast<std::streamsize>(serialized_bloom.size()));
    current_offset += bloom_header.size() + serialized_bloom.size();

    // Write fixed 32-byte footer
    std::string footer_body;
    footer_body.reserve(20);
    PutU64(footer_body, index_offset);
    PutU64(footer_body, bloom_offset);
    PutU32(footer_body, static_cast<std::uint32_t>(sorted_records.size()));

    const std::uint32_t footer_crc = Crc32::Compute(footer_body.data(), footer_body.size());
    std::string footer = footer_body;
    PutU32(footer, footer_crc);
    PutU64(footer, kMagicNumber);

    out.write(footer.data(), static_cast<std::streamsize>(footer.size()));
    out.flush();
    if (!out.good()) {
        return Status::IOError("Failed flushing SSTable footer: " + tmp_path);
    }
    out.close();

    std::error_code ec;
    std::filesystem::rename(tmp_path, path, ec);
    if (ec) {
        return Status::IOError("Failed atomic rename of SSTable: " + ec.message());
    }
    return Status::Ok();
}

Status SSTableReader::Open(const std::string& path,
                           std::uint64_t sequence_id,
                           int level,
                           std::unique_ptr<SSTableReader>* out) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in.is_open()) {
        return Status::IOError("Failed to open SSTable: " + path);
    }

    const std::streamoff size = in.tellg();
    if (size < static_cast<std::streamoff>(SSTableBuilder::kFooterSize)) {
        return Status::Corruption("SSTable file smaller than footer size: " + path);
    }

    std::uint8_t footer[SSTableBuilder::kFooterSize];
    in.seekg(size - static_cast<std::streamoff>(SSTableBuilder::kFooterSize));
    in.read(reinterpret_cast<char*>(footer), SSTableBuilder::kFooterSize);
    if (!in.good()) {
        return Status::IOError("Failed reading SSTable footer: " + path);
    }

    const std::uint64_t index_offset = GetU64(footer);
    const std::uint64_t bloom_offset = GetU64(footer + 8);
    const std::uint32_t entry_count = GetU32(footer + 16);
    const std::uint32_t stored_crc = GetU32(footer + 20);
    const std::uint64_t magic = GetU64(footer + 24);

    if (magic != SSTableBuilder::kMagicNumber) {
        return Status::Corruption("Invalid SSTable magic number: " + path);
    }
    const std::uint32_t computed_crc = Crc32::Compute(footer, 20);
    if (computed_crc != stored_crc) {
        return Status::Corruption("SSTable footer CRC32 mismatch: " + path);
    }

    std::unique_ptr<SSTableReader> reader(new SSTableReader(path, sequence_id, level));
    reader->index_offset_ = index_offset;
    reader->bloom_offset_ = bloom_offset;
    reader->entry_count_ = entry_count;
    reader->file_size_ = static_cast<std::uint64_t>(size);

    // Load sparse index into memory
    in.seekg(static_cast<std::streamoff>(index_offset));
    std::uint8_t count_buf[4];
    in.read(reinterpret_cast<char*>(count_buf), 4);
    const std::uint32_t idx_count = GetU32(count_buf);
    reader->sparse_index_.reserve(idx_count);

    for (std::uint32_t i = 0; i < idx_count; ++i) {
        std::uint8_t klen_buf[4];
        in.read(reinterpret_cast<char*>(klen_buf), 4);
        const std::uint32_t klen = GetU32(klen_buf);
        std::string key(klen, '\0');
        if (klen > 0) {
            in.read(&key[0], klen);
        }
        std::uint8_t off_buf[8];
        in.read(reinterpret_cast<char*>(off_buf), 8);
        if (!in.good()) {
            return Status::Corruption("Truncated sparse index in SSTable: " + path);
        }
        reader->sparse_index_.push_back(SparseIndexEntry{std::move(key), GetU64(off_buf)});
    }

    // Load Bloom filter into memory
    in.seekg(static_cast<std::streamoff>(bloom_offset));
    std::uint8_t blen_buf[4];
    in.read(reinterpret_cast<char*>(blen_buf), 4);
    const std::uint32_t blen = GetU32(blen_buf);
    std::string bloom_bytes(blen, '\0');
    if (blen > 0) {
        in.read(&bloom_bytes[0], blen);
    }
    if (!in.good()) {
        return Status::Corruption("Truncated Bloom filter in SSTable: " + path);
    }

    Status s = BloomFilter::Deserialize(bloom_bytes, &reader->bloom_filter_);
    if (!s.ok()) {
        return s;
    }

    *out = std::move(reader);
    return Status::Ok();
}

Status SSTableReader::Get(const std::string& key, std::string* value, bool* is_tombstone) const {
    if (entry_count_ == 0) {
        return Status::NotFound();
    }
    if (!bloom_filter_.MightContain(key)) {
        return Status::NotFound();
    }

    // Locate starting block offset via sparse index scan
    std::uint64_t scan_start = 0;
    std::uint64_t scan_end = index_offset_;
    for (std::size_t i = 0; i < sparse_index_.size(); ++i) {
        if (sparse_index_[i].key <= key) {
            scan_start = sparse_index_[i].offset;
            if (i + 1 < sparse_index_.size()) {
                scan_end = sparse_index_[i + 1].offset;
            } else {
                scan_end = index_offset_;
            }
        } else {
            if (i == 0) {
                return Status::NotFound();
            }
            break;
        }
    }

    std::ifstream in(path_, std::ios::binary);
    if (!in.is_open()) {
        return Status::IOError("Failed opening SSTable for read: " + path_);
    }

    in.seekg(static_cast<std::streamoff>(scan_start));
    std::uint64_t pos = scan_start;
    while (pos < scan_end) {
        std::uint8_t hdr[9];
        in.read(reinterpret_cast<char*>(hdr), 9);
        if (!in.good()) {
            return Status::Corruption("Unexpected EOF scanning SSTable block");
        }
        const bool tomb = (hdr[0] != 0);
        const std::uint32_t klen = GetU32(hdr + 1);
        const std::uint32_t vlen = GetU32(hdr + 5);

        std::string rec_key(klen, '\0');
        if (klen > 0) {
            in.read(&rec_key[0], klen);
        }

        if (rec_key == key) {
            if (is_tombstone != nullptr) {
                *is_tombstone = tomb;
            }
            if (!tomb && value != nullptr) {
                value->assign(vlen, '\0');
                if (vlen > 0) {
                    in.read(&(*value)[0], vlen);
                }
            }
            return Status::Ok();
        }

        if (rec_key > key) {
            return Status::NotFound();
        }

        in.seekg(static_cast<std::streamoff>(vlen), std::ios::cur);
        pos += 9 + klen + vlen;
    }

    return Status::NotFound();
}

Status SSTableReader::ReadAll(std::vector<KeyValueRecord>* out) const {
    if (out == nullptr) {
        return Status::InvalidArgument("Null ReadAll output vector");
    }
    out->clear();
    out->reserve(entry_count_);

    std::ifstream in(path_, std::ios::binary);
    if (!in.is_open()) {
        return Status::IOError("Failed opening SSTable for full scan: " + path_);
    }

    std::uint64_t pos = 0;
    while (pos < index_offset_) {
        std::uint8_t hdr[9];
        in.read(reinterpret_cast<char*>(hdr), 9);
        if (!in.good()) {
            return Status::Corruption("Corrupted record header during SSTable scan");
        }
        const bool tomb = (hdr[0] != 0);
        const std::uint32_t klen = GetU32(hdr + 1);
        const std::uint32_t vlen = GetU32(hdr + 5);

        std::string key(klen, '\0');
        std::string val(vlen, '\0');
        if (klen > 0) in.read(&key[0], klen);
        if (vlen > 0) in.read(&val[0], vlen);
        if (!in.good()) {
            return Status::Corruption("Truncated payload during SSTable scan");
        }

        out->push_back(KeyValueRecord{std::move(key), std::move(val), tomb});
        pos += 9 + klen + vlen;
    }
    return Status::Ok();
}

}  // namespace kvforge
