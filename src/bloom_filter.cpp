#include "kvforge/bloom_filter.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace kvforge {
namespace {

void WriteU32(std::string& dst, std::uint32_t v) {
    dst.push_back(static_cast<char>(v & 0xFFU));
    dst.push_back(static_cast<char>((v >> 8U) & 0xFFU));
    dst.push_back(static_cast<char>((v >> 16U) & 0xFFU));
    dst.push_back(static_cast<char>((v >> 24U) & 0xFFU));
}

std::uint32_t ReadU32(const char* src) {
    const auto* b = reinterpret_cast<const std::uint8_t*>(src);
    return static_cast<std::uint32_t>(b[0]) |
           (static_cast<std::uint32_t>(b[1]) << 8U) |
           (static_cast<std::uint32_t>(b[2]) << 16U) |
           (static_cast<std::uint32_t>(b[3]) << 24U);
}

}  // namespace

BloomFilter::BloomFilter(std::size_t expected_keys, std::size_t bits_per_key) {
    const std::size_t safe_keys = std::max<std::size_t>(expected_keys, 1);
    const std::size_t safe_bpk = std::max<std::size_t>(bits_per_key, 4);

    // Optimal k = (m / n) * ln(2), clamped to [1, 30]
    auto k = static_cast<std::uint32_t>(std::round(static_cast<double>(safe_bpk) * 0.69314718056));
    num_hashes_ = std::clamp<std::uint32_t>(k, 1U, 30U);

    std::size_t total_bits = std::max<std::size_t>(safe_keys * safe_bpk, 64);
    const std::size_t total_bytes = (total_bits + 7) / 8;
    bit_count_ = static_cast<std::uint32_t>(total_bytes * 8);
    bits_.assign(total_bytes, 0);
}

std::uint32_t BloomFilter::Hash1(const std::string& key) {
    // 32-bit FNV-1a
    std::uint32_t h = 2166136261U;
    for (unsigned char c : key) {
        h ^= c;
        h *= 16777619U;
    }
    return h;
}

std::uint32_t BloomFilter::Hash2(const std::string& key) {
    // Murmur-style avalanche mix seeded differently
    std::uint32_t h = 0x9E3779B9U;
    for (unsigned char c : key) {
        h ^= static_cast<std::uint32_t>(c) + 0x9E3779B9U + (h << 6U) + (h >> 2U);
    }
    h ^= (h >> 16U);
    h *= 0x85EBCA6BU;
    h ^= (h >> 13U);
    h *= 0xC2B2AE35U;
    h ^= (h >> 16U);
    return (h == 0U) ? 0x27D4EB2DU : h;
}

void BloomFilter::Add(const std::string& key) {
    if (bit_count_ == 0) {
        return;
    }
    const std::uint32_t h1 = Hash1(key);
    const std::uint32_t h2 = Hash2(key);
    for (std::uint32_t i = 0; i < num_hashes_; ++i) {
        const std::uint32_t bit_pos = (h1 + i * h2) % bit_count_;
        bits_[bit_pos / 8U] |= static_cast<std::uint8_t>(1U << (bit_pos % 8U));
    }
}

bool BloomFilter::MightContain(const std::string& key) const {
    if (bit_count_ == 0) {
        return true;
    }
    const std::uint32_t h1 = Hash1(key);
    const std::uint32_t h2 = Hash2(key);
    for (std::uint32_t i = 0; i < num_hashes_; ++i) {
        const std::uint32_t bit_pos = (h1 + i * h2) % bit_count_;
        if ((bits_[bit_pos / 8U] & static_cast<std::uint8_t>(1U << (bit_pos % 8U))) == 0U) {
            return false;
        }
    }
    return true;
}

std::string BloomFilter::Serialize() const {
    std::string out;
    out.reserve(8 + bits_.size());
    WriteU32(out, num_hashes_);
    WriteU32(out, bit_count_);
    out.append(reinterpret_cast<const char*>(bits_.data()), bits_.size());
    return out;
}

Status BloomFilter::Deserialize(const std::string& data, BloomFilter* out) {
    if (out == nullptr) {
        return Status::InvalidArgument("Null output BloomFilter pointer");
    }
    if (data.size() < 8) {
        return Status::Corruption("Bloom filter buffer too short");
    }
    const std::uint32_t num_hashes = ReadU32(data.data());
    const std::uint32_t bit_count = ReadU32(data.data() + 4);
    const std::size_t expected_bytes = (bit_count + 7U) / 8U;
    if (data.size() != 8 + expected_bytes) {
        return Status::Corruption("Bloom filter payload length mismatch");
    }

    out->num_hashes_ = num_hashes;
    out->bit_count_ = bit_count;
    out->bits_.resize(expected_bytes);
    if (expected_bytes > 0) {
        std::memcpy(out->bits_.data(), data.data() + 8, expected_bytes);
    }
    return Status::Ok();
}

}  // namespace kvforge
