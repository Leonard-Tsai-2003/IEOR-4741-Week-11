// fix_bench.cpp — Batch benchmark comparing current zero-alloc fix_parser.hpp
// against a naive SOH/'=' split-based parser using tests/bench.hpp.
//
// Build & run from repo root:
//   g++ -std=c++17 -O2 -Wall -Wextra -pthread -Iinclude -Itests fix_bench.cpp -o /tmp/fix_bench && /tmp/fix_bench

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "fix_parser.hpp"
#include "bench.hpp"

// ---------------------------------------------------------------------------
// 1. Naive Split-Based Baseline Parser (allocates std::string / std::vector)
// ---------------------------------------------------------------------------
struct NaiveNewOrder {
    std::string clordid;
    std::string symbol;
    char side = 0;
    uint32_t qty = 0;
    double price = 0.0;
};

static bool parse_new_order_naive(const char* buf, int len, NaiveNewOrder& out) {
    std::string msg(buf, static_cast<size_t>(len));

    // Split on SOH ('\x01') into vector of "tag=value" strings
    std::vector<std::string> fields;
    size_t start = 0;
    while (start < msg.size()) {
        size_t pos = msg.find('\x01', start);
        if (pos == std::string::npos) {
            fields.push_back(msg.substr(start));
            break;
        }
        fields.push_back(msg.substr(start, pos - start));
        start = pos + 1;
    }

    bool id = false, sym = false, side = false, qty = false, px = false;

    for (const std::string& kv : fields) {
        size_t eq = kv.find('=');
        if (eq == std::string::npos || eq == 0) return false;

        std::string tag_str = kv.substr(0, eq);
        std::string val_str = kv.substr(eq + 1);

        int tag = 0;
        try {
            tag = std::stoi(tag_str);
        } catch (...) {
            return false;
        }

        switch (tag) {
            case 11:
                out.clordid = val_str;
                id = true;
                break;
            case 55:
                out.symbol = val_str;
                sym = true;
                break;
            case 54:
                if (!val_str.empty()) {
                    out.side = val_str[0];
                    side = true;
                }
                break;
            case 38:
                try {
                    out.qty = static_cast<uint32_t>(std::stoul(val_str));
                    qty = true;
                } catch (...) {
                    return false;
                }
                break;
            case 44:
                try {
                    out.price = std::stod(val_str);
                    px = true;
                } catch (...) {
                    return false;
                }
                break;
            default:
                break;
        }
    }

    return id && sym && side && qty && px;
}

// ---------------------------------------------------------------------------
// 2. FIX 4.2 Message Generator & BodyLength / CheckSum Framing Helper
// ---------------------------------------------------------------------------
static std::string build_fix_new_order(int seq,
                                       const char* clordid,
                                       const char* symbol,
                                       char side,
                                       uint32_t qty,
                                       double price) {
    char px_buf[32];
    std::snprintf(px_buf, sizeof(px_buf), "%.2f", price);

    // Body starts after 9=<BodyLength>\x01 and runs up to (including) SOH before 10=
    std::string body;
    body.reserve(128);
    body += "35=D\x01";
    body += "49=ALGO_CLIENT\x01";
    body += "56=EXCH_SIM\x01";
    body += "34=" + std::to_string(seq) + "\x01";
    body += "52=20261118-16:30:00.125\x01";
    body += std::string("11=") + clordid + "\x01";
    body += "21=1\x01";
    body += std::string("55=") + symbol + "\x01";
    body += std::string("54=") + side + "\x01";
    body += "38=" + std::to_string(qty) + "\x01";
    body += "40=2\x01";
    body += std::string("44=") + px_buf + "\x01";
    body += "59=0\x01";

    std::string header = "8=FIX.4.2\x01" "9=" + std::to_string(body.size()) + "\x01";
    std::string frame = header + body;

    // FIX CheckSum: sum of all bytes from '8' through SOH before 10=, modulo 256
    unsigned int csum = 0;
    for (unsigned char c : frame) {
        csum += c;
    }
    csum &= 0xFFu;

    char trailer[16];
    std::snprintf(trailer, sizeof(trailer), "10=%03u\x01", csum);
    frame += trailer;
    return frame;
}

struct MsgSlice {
    int offset;
    int length;
};

// Frames concatenated FIX messages on 8= / 9=<BodyLength> / 10=<CheckSum>\x01
// and parses each complete frame into a reused NewOrder struct.
static int parse_concatenated_batch(const char* buf, int total_len, NewOrder& out) {
    int pos = 0;
    int parsed_count = 0;

    while (pos < total_len) {
        // Require at least "8=FIX.4.2\x019=1\x01"
        if (total_len - pos < 14) break;
        if (std::memcmp(buf + pos, "8=FIX.", 6) != 0) return -1;

        // Find SOH ending tag 8
        const char* p = buf + pos + 6;
        const char* end = buf + total_len;
        while (p < end && *p != '\x01') ++p;
        if (p >= end) break; // partial read
        ++p; // consume SOH

        // Expect "9=<BodyLength>\x01"
        if (end - p < 3 || p[0] != '9' || p[1] != '=') return -1;
        p += 2;
        int body_len = 0;
        const char* digits_start = p;
        while (p < end && *p != '\x01') {
            if (*p < '0' || *p > '9') return -1;
            body_len = body_len * 10 + (*p - '0');
            ++p;
        }
        if (p == digits_start || p >= end) break; // partial read or empty tag 9
        ++p; // consume SOH after BodyLength

        // Full frame length = header_len + body_len + 7 ("10=XXX\x01")
        int header_len = static_cast<int>(p - (buf + pos));
        int frame_len = header_len + body_len + 7;
        if (total_len - pos < frame_len) break; // partial message at end of buffer

        // Verify trailer "10=ddd\x01"
        const char* trailer = buf + pos + header_len + body_len;
        if (trailer[0] != '1' || trailer[1] != '0' || trailer[2] != '=' || trailer[6] != '\x01') {
            return -1;
        }

        if (!parse_new_order(buf + pos, frame_len, out)) {
            return -1;
        }
        doNotOptimize(out.qty);
        doNotOptimize(out.price);
        doNotOptimize(out.side);
        doNotOptimize(out.symbol[0]);
        doNotOptimize(out.clordid_len);

        ++parsed_count;
        pos += frame_len;
    }

    return parsed_count;
}

// Calibrate hardware ticks/ns so we can display GHz and cross-check cycles vs ns
static double measure_ticks_per_ns() {
    uint64_t hz = cycle_count_hz();
    if (hz > 0) {
        return static_cast<double>(hz) / 1e9;
    }
    auto t0 = std::chrono::steady_clock::now();
    uint64_t c0 = cycle_count();
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    uint64_t c1 = cycle_count();
    auto t1 = std::chrono::steady_clock::now();
    double elapsed_ns = std::chrono::duration<double, std::nano>(t1 - t0).count();
    return static_cast<double>(c1 - c0) / elapsed_ns;
}

int main() {
    // Build a realistic batch of 64 concatenated FIX 4.2 NewOrderSingle messages
    constexpr int BATCH_SIZE = 64;
    const char* symbols[] = {
        "AAPL", "MSFT", "NVDA", "BRK.B", "GOOGL", "AMZN", "META", "TSLA"
    };

    std::string batch_buf;
    std::vector<MsgSlice> slices;
    slices.reserve(BATCH_SIZE);

    for (int i = 0; i < BATCH_SIZE; ++i) {
        char clordid[32];
        std::snprintf(clordid, sizeof(clordid), "ORD20261118-%05d", 10000 + i);
        const char* sym = symbols[i & 7];
        char side = (i & 1) ? '1' : '2';
        uint32_t qty = static_cast<uint32_t>(100 + (i % 25) * 10);
        double price = 150.25 + (i % 40) * 0.15;

        std::string msg = build_fix_new_order(i + 1, clordid, sym, side, qty, price);
        int offset = static_cast<int>(batch_buf.size());
        int length = static_cast<int>(msg.size());
        batch_buf += msg;
        slices.push_back({offset, length});
    }

    const char* raw_batch = batch_buf.data();
    const int total_bytes = static_cast<int>(batch_buf.size());

    // -----------------------------------------------------------------------
    // Step 1: Verify exact equivalence between naive and fast parsers
    // -----------------------------------------------------------------------
    NewOrder fast_out{};
    NaiveNewOrder naive_out{};

    for (int i = 0; i < BATCH_SIZE; ++i) {
        const char* msg_ptr = raw_batch + slices[i].offset;
        const int msg_len = slices[i].length;

        std::memset(&fast_out, 0, sizeof(fast_out));
        bool ok_fast = parse_new_order(msg_ptr, msg_len, fast_out);
        bool ok_naive = parse_new_order_naive(msg_ptr, msg_len, naive_out);

        bool match = ok_fast && ok_naive
            && std::string(fast_out.clordid, fast_out.clordid_len) == naive_out.clordid
            && std::string(fast_out.symbol) == naive_out.symbol
            && fast_out.side == naive_out.side
            && fast_out.qty == naive_out.qty
            && std::fabs(fast_out.price - naive_out.price) < 1e-9;

        if (!match) {
            std::cerr << "ERROR: Parser mismatch on batch message " << i << "\n";
            return 1;
        }
    }

    int framed_count = parse_concatenated_batch(raw_batch, total_bytes, fast_out);
    if (framed_count != BATCH_SIZE) {
        std::cerr << "ERROR: Framed batch parsed " << framed_count
                  << " messages, expected " << BATCH_SIZE << "\n";
        return 1;
    }

    // -----------------------------------------------------------------------
    // Step 2: Benchmark batch parsing (cycles/op and ns/op per message)
    // -----------------------------------------------------------------------
    const double ticks_per_ns = measure_ticks_per_ns();
    const long batch_iters = 50000; // 50,000 batches * 64 msgs = 3.2M messages

    // A. Naive split-based parser across the 64-message batch
    double naive_batch_cycles = cycles_per_op([&] {
        NaiveNewOrder out;
        for (int i = 0; i < BATCH_SIZE; ++i) {
            bool ok = parse_new_order_naive(raw_batch + slices[i].offset, slices[i].length, out);
            doNotOptimize(ok);
            doNotOptimize(out.qty);
            doNotOptimize(out.price);
        }
    }, batch_iters / 5); // naive parser is much slower; 10k batches = 640k msgs

    double naive_batch_ns = ns_per_op([&] {
        NaiveNewOrder out;
        for (int i = 0; i < BATCH_SIZE; ++i) {
            bool ok = parse_new_order_naive(raw_batch + slices[i].offset, slices[i].length, out);
            doNotOptimize(ok);
            doNotOptimize(out.qty);
            doNotOptimize(out.price);
        }
    }, batch_iters / 5);

    // B. Current fix_parser.hpp (parse_new_order) across the 64-message batch
    double fast_batch_cycles = cycles_per_op([&] {
        NewOrder out;
        for (int i = 0; i < BATCH_SIZE; ++i) {
            bool ok = parse_new_order(raw_batch + slices[i].offset, slices[i].length, out);
            doNotOptimize(ok);
            doNotOptimize(out.qty);
            doNotOptimize(out.price);
            doNotOptimize(out.side);
            doNotOptimize(out.symbol[0]);
            doNotOptimize(out.clordid_len);
        }
    }, batch_iters);

    double fast_batch_ns = ns_per_op([&] {
        NewOrder out;
        for (int i = 0; i < BATCH_SIZE; ++i) {
            bool ok = parse_new_order(raw_batch + slices[i].offset, slices[i].length, out);
            doNotOptimize(ok);
            doNotOptimize(out.qty);
            doNotOptimize(out.price);
            doNotOptimize(out.side);
            doNotOptimize(out.symbol[0]);
            doNotOptimize(out.clordid_len);
        }
    }, batch_iters);

    // C. End-to-end concatenated batch (BodyLength framing + parse_new_order)
    double framed_batch_cycles = cycles_per_op([&] {
        NewOrder out;
        int n = parse_concatenated_batch(raw_batch, total_bytes, out);
        doNotOptimize(n);
    }, batch_iters);

    double framed_batch_ns = ns_per_op([&] {
        NewOrder out;
        int n = parse_concatenated_batch(raw_batch, total_bytes, out);
        doNotOptimize(n);
    }, batch_iters);

    // Normalize per message ("op" = 1 parsed FIX NewOrderSingle message)
    const double naive_cyc_per_msg  = naive_batch_cycles  / BATCH_SIZE;
    const double naive_ns_per_msg   = naive_batch_ns      / BATCH_SIZE;
    const double fast_cyc_per_msg   = fast_batch_cycles   / BATCH_SIZE;
    const double fast_ns_per_msg    = fast_batch_ns       / BATCH_SIZE;
    const double framed_cyc_per_msg = framed_batch_cycles / BATCH_SIZE;
    const double framed_ns_per_msg  = framed_batch_ns     / BATCH_SIZE;

    const double speedup_cycles = naive_cyc_per_msg / fast_cyc_per_msg;
    const double speedup_ns     = naive_ns_per_msg  / fast_ns_per_msg;
    const double avg_msg_bytes  = static_cast<double>(total_bytes) / BATCH_SIZE;
    const double msgs_per_sec   = 1e9 / fast_ns_per_msg;
    const double mb_per_sec     = (msgs_per_sec * avg_msg_bytes) / (1024.0 * 1024.0);

    std::cout << std::fixed << std::setprecision(2);
    std::cout << "=====================================================================\n";
    std::cout << " FIX 4.2 Batch Parser Microbenchmark (" << BATCH_SIZE << " msgs/batch, "
              << total_bytes << " bytes/batch)\n";
    std::cout << " Counter rate: ~" << ticks_per_ns << " ticks/ns\n";
    std::cout << "=====================================================================\n";
    std::cout << std::left << std::setw(34) << "Parser Variant"
              << std::right << std::setw(13) << "cycles/op"
              << std::setw(12) << "ns/op"
              << std::setw(11) << "Speedup" << "\n";
    std::cout << "---------------------------------------------------------------------\n";
    std::cout << std::left << std::setw(34) << "Naive split (std::string/vector)"
              << std::right << std::setw(13) << naive_cyc_per_msg
              << std::setw(12) << naive_ns_per_msg
              << std::setw(10) << 1.00 << "x\n";
    std::cout << std::left << std::setw(34) << "Current fix_parser.hpp (parse)"
              << std::right << std::setw(13) << fast_cyc_per_msg
              << std::setw(12) << fast_ns_per_msg
              << std::setw(10) << speedup_cycles << "x\n";
    std::cout << std::left << std::setw(34) << "Current fix_parser.hpp + framing"
              << std::right << std::setw(13) << framed_cyc_per_msg
              << std::setw(12) << framed_ns_per_msg
              << std::setw(10) << (naive_cyc_per_msg / framed_cyc_per_msg) << "x\n";
    std::cout << "=====================================================================\n";
    std::cout << "Throughput (parse_new_order): " << (msgs_per_sec / 1e6)
              << " M msgs/s  (" << mb_per_sec << " MB/s)\n";
    std::cout << "Speedup vs. naive (cycles):   " << speedup_cycles << "x\n";
    std::cout << "Speedup vs. naive (wall ns):  " << speedup_ns << "x\n";

    return 0;
}
