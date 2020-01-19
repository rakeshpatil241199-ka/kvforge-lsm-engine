#include <gtest/gtest.h>

#include <filesystem>
#include <memory>
#include <string>

#include "kvforge/db.h"

namespace fs = std::filesystem;

namespace {

std::string MakeTempDbDir(const std::string& name) {
    fs::path p = fs::temp_directory_path() / "kvforge_db_tests" / name;
    std::error_code ec;
    fs::remove_all(p, ec);
    return p.string();
}

TEST(DBIntegrationTest, CrashRecoveryReplaysUnflushedWal) {
    const std::string dir = MakeTempDbDir("wal_replay");
    kvforge::Options opts;
    opts.sync_wal = false;
    opts.memtable_max_bytes = 1024 * 1024;

    {
        std::unique_ptr<kvforge::DB> db;
        ASSERT_TRUE(kvforge::DB::Open(dir, opts, &db).ok());
        ASSERT_TRUE(db->Put("user:1", "rakesh").ok());
        ASSERT_TRUE(db->Put("user:2", "patil").ok());
        ASSERT_TRUE(db->Delete("user:1").ok());
        // Destroy DB without flushing MemTable to SSTable
    }

    {
        std::unique_ptr<kvforge::DB> reopened;
        ASSERT_TRUE(kvforge::DB::Open(dir, opts, &reopened).ok());
        std::string val;
        EXPECT_TRUE(reopened->Get("user:1", &val).IsNotFound());
        ASSERT_TRUE(reopened->Get("user:2", &val).ok());
        EXPECT_EQ(val, "patil");
    }
}

TEST(DBIntegrationTest, AutomaticFlushesCompactionAndTombstoneReclamation) {
    const std::string dir = MakeTempDbDir("compaction_flow");
    kvforge::Options opts;
    opts.sync_wal = false;
    opts.memtable_max_bytes = 2048;  // Small MemTable to force multiple L0 flushes
    opts.l0_compaction_trigger = 3;
    opts.sparse_index_interval = 4;

    std::unique_ptr<kvforge::DB> db;
    ASSERT_TRUE(kvforge::DB::Open(dir, opts, &db).ok());

    // Write 150 keys, overwrite half, delete a quarter
    for (int i = 0; i < 150; ++i) {
        ASSERT_TRUE(db->Put("key_" + std::to_string(i), "v1_" + std::string(32, 'a')).ok());
    }
    for (int i = 0; i < 75; ++i) {
        ASSERT_TRUE(db->Put("key_" + std::to_string(i), "v2_" + std::string(32, 'b')).ok());
    }
    for (int i = 0; i < 30; ++i) {
        ASSERT_TRUE(db->Delete("key_" + std::to_string(i)).ok());
    }

    ASSERT_TRUE(db->Flush().ok());
    kvforge::CompactionResult compact_res;
    ASSERT_TRUE(db->Compact(&compact_res).ok());

    auto stats = db->GetStats();
    EXPECT_EQ(stats.l0_table_count, 0U);
    EXPECT_EQ(stats.l1_table_count, 1U);
    EXPECT_GT(stats.total_flushes, 2U);
    EXPECT_GE(stats.total_compactions, 1U);

    // Verify logical state after compaction
    std::string val;
    for (int i = 0; i < 30; ++i) {
        EXPECT_TRUE(db->Get("key_" + std::to_string(i), &val).IsNotFound());
    }
    for (int i = 30; i < 75; ++i) {
        ASSERT_TRUE(db->Get("key_" + std::to_string(i), &val).ok());
        EXPECT_EQ(val, "v2_" + std::string(32, 'b'));
    }
    for (int i = 75; i < 150; ++i) {
        ASSERT_TRUE(db->Get("key_" + std::to_string(i), &val).ok());
        EXPECT_EQ(val, "v1_" + std::string(32, 'a'));
    }

    // Query missing keys to confirm Bloom filter rejection counters increment
    for (int i = 0; i < 20; ++i) {
        EXPECT_TRUE(db->Get("nonexistent_" + std::to_string(i), &val).IsNotFound());
    }
    EXPECT_GT(db->GetStats().bloom_filter_skips, 0U);
}

}  // namespace
