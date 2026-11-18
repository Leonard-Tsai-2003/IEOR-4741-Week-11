# IEOR 4741 — Week 11: Low-Latency Zero-Allocation FIX Parser

High-performance, single-pass C++17 FIX 4.2 (`NewOrderSingle`, `35=D`) parser and HFT core data structures for **IEOR 4741** (`Leonard-Tsai-2003/IEOR-4741-Week-11`).

## Repository Structure

```text
IEOR-4741-Week-11-main/
├── Makefile                        # Quick single-target builds & local CI runner
├── .github/workflows/grade.yml     # GitHub Actions CI workflow (g++, TSan, run_ci.py)
├── include/
│   ├── fix_parser.hpp              # Week 11: Single-pass zero-alloc FIX NewOrder parser
│   ├── order_book.hpp              # Week 7: Flat price-indexed OrderBook + open-addressed SymMap
│   ├── pool.hpp                    # Week 4: O(1) intrusive free-list fixed-size object Pool
│   ├── rolling_counter.hpp         # Week 8: Sliding-window event counter (uint64_t underflow-safe)
│   ├── spsc_ring.hpp               # Week 10: Cache-line padded lock-free SPSC ring buffer
│   ├── shm_ring.hpp                # Phase 4: Shared-memory POD SPSC ring (stub)
│   └── u64toa.hpp                  # Week 12: Fast uint64-to-ASCII serializer (stub)
└── tests/
    ├── bench.hpp                   # Portable ns_per_op() and hardware cycles_per_op() harness
    ├── fix_test.cpp                # Correctness & single-message microbenchmark for fix_parser.hpp
    ├── fix_bench.cpp               # 64-message concatenated batch benchmark vs. naive split baseline
    ├── run_ci.py                   # Autograder harness producing report.json
    └── *_test.cpp / spsc_*.cpp     # Unit, concurrency, and ThreadSanitizer test suites
```

## Build & Run

Requires a C++17 compiler (`g++` or `clang++`) and Python 3.

```bash
# Build and run the Week 11 FIX parser unit test & single-message benchmark
make fix

# Build and run the 64-message batch benchmark vs. naive split-based baseline
g++ -std=c++17 -O2 -Wall -Wextra -Werror -pthread -Iinclude -Itests tests/fix_bench.cpp -o /tmp/fix_bench && /tmp/fix_bench

# Run the full autograder suite (writes report.json)
make test

# Clean build artifacts
make clean
```

## Architecture & Protocol Design

### 1. TCP vs. UDP Transport Context

* **UDP Multicast (Market Data):** Message boundaries align with datagram boundaries (`recvfrom()` returns one packet per read), though packet drops must be detected via `MsgSeqNum` (tag `34`).
* **TCP Stream (Order Entry / FIX):** TCP is an unframed byte stream—a single `recv()` call may deliver **multiple concatenated FIX messages** (coalesced by Nagle's algorithm or burst order flow) or a **partial message** split mid-tag or mid-value. The parser separates **stream framing** from **field decoding** and operates over a reusable caller-owned receive buffer without per-message heap allocations.

### 2. Single-Pass Zero-Allocation Parsing (`include/fix_parser.hpp`)

`parse_new_order(const char* buf, int len, NewOrder& out)` scans the input buffer once from `buf` to `buf + len` using raw pointer arithmetic (`p`, `end`) delimited by `SOH` (`'\x01'`):

```cpp
struct NewOrder {
    const char* clordid; int clordid_len;   // Tag 11: zero-copy pointer view into rx buffer
    char symbol[16];                        // Tag 55: fixed-capacity stack buffer (null-terminated)
    char side;                              // Tag 54: single ASCII byte ('1' = Buy, '2' = Sell)
    uint32_t qty;                           // Tag 38: inline base-10 integer accumulation
    double price;                           // Tag 44: parsed decimal price
};
```

* **Zero `std::string` / `std::vector` allocations:** `ClOrdID` (tag `11`) is stored as a `(const char*, int)` slice pointing directly into the caller's reusable receive buffer; `Symbol` (tag `55`) is copied into a fixed 16-byte stack buffer via `std::memcpy`.
* **Selective field extraction:** Only tags `11`, `55`, `54`, `38`, and `44` are decoded. Session/header/trailer tags (`8`, `9`, `35`, `49`, `56`, `34`, `52`, `10`, etc.) are skipped in-place by advancing `p` to the next `SOH`.

### 3. Batch Framing, Bounds & Checksum Validation

To handle concatenated messages and partial TCP reads safely without crashing on malformed frames:

1. **Header & `BodyLength` Framing (Tags `8` & `9`):** Locate `8=FIX.4.x\x019=<BodyLength>\x01`. `BodyLength` specifies the exact byte count starting immediately after the `SOH` following tag `9` up to (but excluding) `10=<CheckSum>\x01`.
2. **Partial-Read Handling:** If fewer bytes remain in the receive buffer than `header_len + body_length + 7` (where `10=XXX\x01` is 7 bytes), return `0` / `Incomplete` so the caller retains the unconsumed prefix and appends the next `recv()` chunk.
3. **Concatenated Batch Loop:** Once a complete frame of `frame_len` bytes is verified, invoke `parse_new_order(frame_ptr, frame_len, out)` and advance the read cursor by `frame_len` until the buffer is drained or an incomplete tail frame is reached.
4. **Checksum (`10=`) & Bounds Validation:**
   * Verify `frame_ptr + frame_len <= buf_end` before reading any byte, and confirm the frame terminates with `10=ddd\x01`.
   * Compute the byte sum from `'8'` of `8=FIX` up to the `SOH` preceding `10=` modulo $256$, and verify it matches the 3-digit integer in tag `10`.
   * Reject non-numeric tags, missing `=`, truncated values, non-digit `OrderQty` characters, unparseable `Price` tokens (ensuring floating-point parsing stops at `v + vlen` rather than scanning past `end`), and messages missing any required tag (`11`, `55`, `54`, `38`, `44`).

## Performance & Benchmarks

### Benchmark Setup (`tests/fix_bench.cpp` & `tests/bench.hpp`)

* **Hardware & OS:** Apple M2 (8 GB unified memory, `arm64`, macOS).
* **Workload:** Batch of $64$ concatenated FIX 4.2 `NewOrderSingle` (`35=D`) messages ($9{,}735\text{ bytes/batch}$, $\approx 152.1\text{ bytes/msg}$) containing full session headers (`8`, `9`, `35`, `49`, `56`, `34`, `52`) and trailers (`10`), benchmarked over $50{,}000$ batches ($3.2\text{M}$ parsed messages).
* **Hardware Counter Note (`arm64`):** On Apple M2, `cycles_per_op()` reads the architectural virtual timer `cntvct_el0` at $24\text{ MHz}$ ($0.024\text{ ticks/ns}$, or $\approx 41.67\text{ ns/tick}$). Both the raw `cycles_per_op()` timer ticks and wall-clock `ns_per_op()` (along with estimated M2 core cycles at $3.49\text{ GHz}$) are reported below.

### Measured Batch Results (Apple M2, 8 GB)

| Parser Variant | `cycles/op` (`cntvct_el0` ticks) | Wall Latency (`ns/op`) | Est. Core Cycles (@ $3.49\text{ GHz}$) | Speedup (`cycles/op`) | Speedup (`ns/op`) |
| :--- | :---: | :---: | :---: | :---: | :---: |
| **Naive Split Baseline** (`std::string` + `std::vector` SOH/`=` split + `std::stoi`/`std::stod`) | $20.56$ | $872.55\text{ ns}$ | $\approx 3{,}045$ | $1.00\times$ | $1.00\times$ |
| **Current `fix_parser.hpp`** (`parse_new_order` zero-alloc single pass) | **$2.61$** | **$136.27\text{ ns}$** | $\approx 476$ | **$7.88\times$** | **$6.40\times$** |
| **Current `fix_parser.hpp` + `BodyLength` Framing** (`parse_concatenated_batch`) | **$2.72$** | **$112.54\text{ ns}$** | $\approx 393$ | **$7.55\times$** | **$7.75\times$** |

* **Sustained Batch Throughput (`parse_new_order`):** **$7.34\text{ M msgs/s}$** (**$1{,}064.50\text{ MB/s}$**).
* **Additional Suite Metrics (`make test` on `x86_64` CI):** `fix_parse_ns_per_op = 184.88 ns`, `spsc_ops_per_sec = 74.87M ops/s`, `pool_ns_per_op = 4.05 ns`, `book_bbo_ns_per_op = 2.13 ns`, `symmap_get_ns_per_op = 13.52 ns`, `rolling_ns_per_op = 5.48 ns`.

### Single Change That Helped Most

Eliminating per-message and per-field dynamic heap allocations by replacing `std::string` / `std::vector` tokenization with a single-pass pointer scan (`const char* p` to `end`) that stores `ClOrdID` as a zero-copy `(const char*, int)` slice and copies `Symbol` into a fixed 16-byte stack array produced the largest single speedup ($7.88\times$ in `cycles/op` and $6.40\times$ in wall-clock `ns/op`). In the naive split-based parser, every FIX message triggers more than a dozen `malloc`/`free` calls and buffer copies (splitting on `SOH`, splitting each pair on `=`, and allocating temporary `std::string` objects for numeric conversion), dominating latency with allocator overhead and cache misses. Scanning the contiguous receive buffer once in-place reduces parsing to sequential L1 cache-line reads and register arithmetic, driving throughput above $1\text{ GB/s}$ on a single Apple M2 core.

## AlgoArena JSON-to-Codec Type Mapping

When bridging the AlgoArena WebSocket/HTTP JSON order protocol (`hft/cpp_client`) to the internal zero-allocation FIX `NewOrder` codec, JSON fields map directly to fixed-width POD members without heap allocation:

| AlgoArena JSON Field | JSON Type | FIX Tag | `NewOrder` C++ Field | Codec Representation & Notes |
| :--- | :--- | :---: | :--- | :--- |
| `"cl_ord_id"` / `"order_id"` | `string` | `11` (`ClOrdID`) | `const char* clordid`<br>`int clordid_len` | Zero-copy pointer + length view into the ingress message buffer |
| `"symbol"` | `string` | `55` (`Symbol`) | `char symbol[16]` | Null-terminated fixed ASCII buffer (up to 15 chars, e.g., `"AAPL"`, `"BRK.B"`) |
| `"side"` | `string` (`"BUY"`/`"SELL"`) | `54` (`Side`) | `char side` | Single ASCII byte (`'1'` for Buy / `'B'`, `'2'` for Sell / `'S'`) |
| `"qty"` / `"quantity"` | `integer` | `38` (`OrderQty`) | `uint32_t qty` | Unsigned 32-bit integer parsed via inline base-10 accumulation |
| `"price"` | `number` (float) | `44` (`Price`) | `double price` | IEEE-754 `double` (converted to integer cent ticks `px / 0.01 + 0.5` for `order_book.hpp`) |
| `"type"` (`"LIMIT"`) | `string` | `35` / `40` | *(validated / skipped)* | `35=D` (`NewOrderSingle`) / `40=2` (`Limit`); non-order heartbeats (`35=0`) skipped |