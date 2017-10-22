#ifdef LEVI_BENCH

#include <iostream>

#include "../src/index_mvcc_rd.h"
#include "source_fetcher.h"

void kv_read_bench() {
    if (LeviDB::IOEnv::fileExists(src_fname_)) {
        const std::string index_fname = "/tmp/levi_bench_index";
        const std::string data_fname = "/tmp/levi_bench_data";
        if (!LeviDB::IOEnv::fileExists(index_fname) || !LeviDB::IOEnv::fileExists(data_fname)) {
            return;
        }

        LeviDB::SeqGenerator seq_g;
        LeviDB::RandomAccessFile rf(data_fname);
        const LeviDB::IndexRead bdt(index_fname, LeviDB::OffsetToEmpty{LeviDB::IndexConst::disk_null_}, &seq_g, &rf);

        SourceFetcher src;
        for (int i = 0; i < test_times_; ++i) {
            auto item = src.readItem();
            auto r = bdt.find(item.first);
            assert(r.first.size() == item.second.size());
        }

        std::cout << __FUNCTION__ << std::endl;
    }
}

#endif // LEVI_BENCH