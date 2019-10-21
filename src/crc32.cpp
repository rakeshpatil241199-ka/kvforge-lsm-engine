#include "kvforge/crc32.h"

#include <array>

namespace kvforge {
namespace {

std::array<std::uint32_t, 256> BuildCrcTable() {
    std::array<std::uint32_t, 256> table{};
    constexpr std::uint32_t kPolynomial = 0xEDB88320U;
    for (std::uint32_t i = 0; i < 256; ++i) {
        std::uint32_t crc = i;
        for (int bit = 0; bit < 8; ++bit) {
            if (crc & 1U) {
                crc = (crc >> 1U) ^ kPolynomial;
            } else {
                crc >>= 1U;
            }
        }
        table[i] = crc;
    }
    return table;
}

const std::array<std::uint32_t, 256>& GetCrcTable() {
    static const auto kTable = BuildCrcTable();
    return kTable;
}

}  // namespace

std::uint32_t Crc32::Update(std::uint32_t current_crc, const void* data, std::size_t length) {
    const auto& table = GetCrcTable();
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    std::uint32_t crc = current_crc ^ 0xFFFFFFFFU;
    for (std::size_t i = 0; i < length; ++i) {
        crc = table[(crc ^ bytes[i]) & 0xFFU] ^ (crc >> 8U);
    }
    return crc ^ 0xFFFFFFFFU;
}

std::uint32_t Crc32::Compute(const void* data, std::size_t length) {
    return Update(0U, data, length);
}

std::uint32_t Crc32::Compute(const std::string& data) {
    return Compute(data.data(), data.size());
}

}  // namespace kvforge
