#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "kvforge/bloom_filter.h"
#include "kvforge/crc32.h"
#include "kvforge/wal.h"

namespace fs = std::filesystem;

namespace {

std::string MakeTempFile(const std::string& name) {
    fs::path dir = fs::temp_directory_path() / "kvforge_unit_tests";
    fs::create_directories(dir);
    fs::path p = dir / name;
    std::error_code ec;
    fs::remove(p, ec);
    return p.string();
}

TEST(Crc32Test, KnownVectorMatchesIEEE8023) {
    // Standard CRC32 check value for ASCII "123456789" is 0xCBF43926
    EXPECT_EQ(kvforge::Crc32::Compute("123456789"), 0xCBF43926U);
}

TEST(BloomFilterTest, ZeroFalseNegativesAndLowFalsePositives) {
    const std::size_t n = 2000;
    kvforge::BloomFilter bf(n, 10);
    for (std::size_t i = 0; i < n; ++i) {
        bf.Add("inserted_key_" + std::to_string(i));
    }

    for (std::size_t i = 0; i < n; ++i) {
        ASSERT_TRUE(bf.MightContain("inserted_key_" + std::to_string(i)));
    }

    const std::string raw = bf.Serialize();
    kvforge::BloomFilter restored;
    ASSERT_TRUE(kvforge::BloomFilter::Deserialize(raw, &restored).ok());

    std::size_t false_positives = 0;
    for (std::size_t i = 0; i < n; ++i) {
        if (restored.MightContain("missing_key_" + std::to_string(i))) {
            ++false_positives;
        }
    }
    const double fp_rate = static_cast<double>(false_positives) / static_cast<double>(n);
    EXPECT_LT(fp_rate, 0.03);
}

TEST(WalTest, AppendReplayAndTruncatedTailRecovery) {
    const std::string wal_path = MakeTempFile("tail_recovery.wal");
    {
        kvforge::WriteAheadLog wal(wal_path, false);
        ASSERT_TRUE(wal.Open().ok());
        ASSERT_TRUE(wal.Append(kvforge::WalOpType::kPut, "alpha", "100").ok());
        ASSERT_TRUE(wal.Append(kvforge::WalOpType::kPut, "beta", "200").ok());
        ASSERT_TRUE(wal.Append(kvforge::WalOpType::kDelete, "alpha", "").ok());
    }

    // Simulate power loss mid-write by appending 7 garbage bytes to the tail
    {
        std::ofstream corrupt_tail(wal_path, std::ios::binary | std::ios::app);
        corrupt_tail.write("\xDE\xAD\xBE\xEF\x01\x05\x00", 7);
    }

    std::vector<kvforge::WalRecord> replayed;
    kvforge::WriteAheadLog wal(wal_path, false);
    ASSERT_TRUE(wal.Replay([&](const kvforge::WalRecord& rec) { replayed.push_back(rec); }, true).ok());

    ASSERT_EQ(replayed.size(), 3U);
    EXPECT_EQ(replayed[0].key, "alpha");
    EXPECT_EQ(replayed[0].value, "100");
    EXPECT_EQ(replayed[1].key, "beta");
    EXPECT_EQ(replayed[2].op, kvforge::WalOpType::kDelete);
}

TEST(WalTest, DetectsMidFileChecksumCorruption) {
    const std::string wal_path = MakeTempFile("crc_corrupt.wal");
    {
        kvforge::WriteAheadLog wal(wal_path, false);
        ASSERT_TRUE(wal.Open().ok());
        ASSERT_TRUE(wal.Append(kvforge::WalOpType::kPut, "k1", "payload_value_1").ok());
        ASSERT_TRUE(wal.Append(kvforge::WalOpType::kPut, "k2", "payload_value_2").ok());
    }

    // Flip a byte inside the first record's value payload
    {
        std::fstream f(wal_path, std::ios::binary | std::ios::in | std::ios::out);
        f.seekp(16);
        char b = 0;
        f.read(&b, 1);
        b ^= 0xFF;
        f.seekp(16);
        f.write(&b, 1);
    }

    kvforge::WriteAheadLog wal(wal_path, false);
    kvforge::Status s = wal.Replay([](const kvforge::WalRecord&) {}, true);
    EXPECT_TRUE(s.IsCorruption());
}

}  // namespace
