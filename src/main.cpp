#include <cstdlib>
#include <iostream>
#include <sstream>
#include <string>

#include "kvforge/db.h"

namespace {

void PrintUsage() {
    std::cout << "KVForge LSM Engine CLI\n"
              << "Usage: kvforge_cli [--db <dir>] [--memtable-kb <kb>] [--bloom-bpk <bits>] [--l0-trigger <n>] [--no-sync]\n"
              << "Commands (stdin):\n"
              << "  PUT <key> <value>\n"
              << "  GET <key>\n"
              << "  DEL <key>\n"
              << "  FLUSH\n"
              << "  COMPACT\n"
              << "  STATS\n"
              << "  EXIT\n";
}

}  // namespace

int main(int argc, char** argv) {
    std::string db_dir = "./data/kvforge";
    kvforge::Options options;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--db" && i + 1 < argc) {
            db_dir = argv[++i];
        } else if (arg == "--memtable-kb" && i + 1 < argc) {
            options.memtable_max_bytes = static_cast<std::size_t>(std::stoul(argv[++i])) * 1024ULL;
        } else if (arg == "--bloom-bpk" && i + 1 < argc) {
            options.bloom_bits_per_key = static_cast<std::size_t>(std::stoul(argv[++i]));
        } else if (arg == "--l0-trigger" && i + 1 < argc) {
            options.l0_compaction_trigger = static_cast<std::size_t>(std::stoul(argv[++i]));
        } else if (arg == "--no-sync") {
            options.sync_wal = false;
        } else if (arg == "--help" || arg == "-h") {
            PrintUsage();
            return 0;
        }
    }

    std::unique_ptr<kvforge::DB> db;
    kvforge::Status s = kvforge::DB::Open(db_dir, options, &db);
    if (!s.ok()) {
        std::cerr << "ERROR: " << s.ToString() << "\n";
        return 1;
    }

    std::string line;
    while (std::getline(std::cin, line)) {
        if (line.empty()) continue;
        std::istringstream iss(line);
        std::string cmd;
        iss >> cmd;

        if (cmd == "PUT") {
            std::string key, value;
            iss >> key;
            std::getline(iss >> std::ws, value);
            s = db->Put(key, value);
            std::cout << (s.ok() ? "OK" : "ERR " + s.ToString()) << "\n";
        } else if (cmd == "GET") {
            std::string key, value;
            iss >> key;
            s = db->Get(key, &value);
            if (s.ok()) {
                std::cout << "VAL " << value << "\n";
            } else if (s.IsNotFound()) {
                std::cout << "NOT_FOUND\n";
            } else {
                std::cout << "ERR " << s.ToString() << "\n";
            }
        } else if (cmd == "DEL") {
            std::string key;
            iss >> key;
            s = db->Delete(key);
            std::cout << (s.ok() ? "OK" : "ERR " + s.ToString()) << "\n";
        } else if (cmd == "FLUSH") {
            s = db->Flush();
            std::cout << (s.ok() ? "OK" : "ERR " + s.ToString()) << "\n";
        } else if (cmd == "COMPACT") {
            kvforge::CompactionResult res;
            s = db->Compact(&res);
            if (s.ok()) {
                std::cout << "OK in_tables=" << res.input_tables
                          << " in_rec=" << res.input_records
                          << " out_rec=" << res.output_records
                          << " dropped_tomb=" << res.dropped_tombstones << "\n";
            } else {
                std::cout << "ERR " << s.ToString() << "\n";
            }
        } else if (cmd == "STATS") {
            auto st = db->GetStats();
            std::cout << "STATS mem_entries=" << st.memtable_entries
                      << " mem_bytes=" << st.memtable_bytes
                      << " l0=" << st.l0_table_count
                      << " l1=" << st.l1_table_count
                      << " puts=" << st.total_puts
                      << " dels=" << st.total_deletes
                      << " gets=" << st.total_gets
                      << " bloom_skips=" << st.bloom_filter_skips
                      << " flushes=" << st.total_flushes
                      << " compactions=" << st.total_compactions
                      << " disk_bytes=" << st.total_disk_bytes << "\n";
        } else if (cmd == "EXIT" || cmd == "QUIT") {
            break;
        } else {
            std::cout << "ERR Unknown command: " << cmd << "\n";
        }
    }

    return 0;
}
