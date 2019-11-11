#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "kvforge/status.h"

namespace kvforge {

class BloomFilter {
public:
    BloomFilter() = default;
    BloomFilter(std::size_t expected_keys, std::size_t bits_per_key);

    void Add(const std::string& key);
    bool MightContain(const std::string& key) const;

    std::string Serialize() const;
    static Status Deserialize(const std::string& data, BloomFilter* out);

    std::size_t bit_count() const { return bit_count_; }
    std::uint32_t num_hashes() const { return num_hashes_; }
    bool empty() const { return bit_count_ == 0; }

private:
    static std::uint32_t Hash1(const std::string& key);
    static std::uint32_t Hash2(const std::string& key);

    std::uint32_t num_hashes_ = 0;
    std::uint32_t bit_count_ = 0;
    std::vector<std::uint8_t> bits_;
};

}  // namespace kvforge
