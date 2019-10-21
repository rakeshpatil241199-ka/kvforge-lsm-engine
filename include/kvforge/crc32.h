#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace kvforge {

// Computes standard IEEE 802.3 CRC32 (matches Python's zlib.crc32).
class Crc32 {
public:
    static std::uint32_t Compute(const void* data, std::size_t length);
    static std::uint32_t Update(std::uint32_t current_crc, const void* data, std::size_t length);
    static std::uint32_t Compute(const std::string& data);
};

}  // namespace kvforge
