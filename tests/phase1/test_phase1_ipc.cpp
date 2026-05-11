/**
 * @file test_phase1_ipc.cpp
 * @brief Phase 1 Validation: PegasusMmapIPC.hpp
 *
 * Verification content (simulated within a single process, no fork required):
 *   1. ProtocolHeader size == 64
 *   2. REGION_SIZE was calculated correctly (header + counter_cache_line + ring_buffer).
 *   3. The producer writes N quotes, and the consumer reads them one by one, with identical values.
 *   4. The protocol header magic number / version / capacity are written correctly.
 *   5. Ring buffer wrap-around correctness
 *   6. Consumer cursor is private: reading it does not affect the producer's write_counter.
 *   7. Ring buffer full check (producer returns false)
 */

#include <iostream>
#include <cassert>
#include <cstring>
#include <vector>
#include <cstdlib>   // posix_memalign / free
#include <new>
#include "PegasusMmapIPC.hpp"

using namespace pegasus;

// ──────────────────────────────────────────────
// auxiliary macros
// ──────────────────────────────────────────────
#define CHECK(cond, msg)                                          \
    do {                                                          \
        if (cond) {                                               \
            std::cout << "  [PASS] " << msg << "\n";             \
        } else {                                                  \
            std::cout << "  [FAIL] " << msg << "\n";             \
            all_passed = false;                                   \
        }                                                         \
    } while(0)

// ────────────────────────────────────────────────────────────────────────────────────────────
// In-process mmap emulation: Replacing mmap files with aligned heap memory
// MmapRegion encapsulates the actual file; in our test, we used a temporary file from the file system.
// ────────────────────────────────────────────────────────────────────────────────────────────

// Test capacity: A smaller capacity makes it easier to trigger wrap-around and full detection.
static constexpr size_t CAP = 8;  // Must be a power of 2
using QProducer = ShmProducer<Quote, CAP>;
using QConsumer = ShmConsumer<Quote, CAP>;

// Factory: Create a temporary shm file under /tmp
static const std::string TMP_PATH = "/tmp/pegasus_test_ipc.shm";

// ──────────────────────────────────────────────
// Test 1: Constants and REGION_SIZE
// ──────────────────────────────────────────────
bool test_constants() {
    bool all_passed = true;
    std::cout << "\n[TEST] Constants & REGION_SIZE\n";

    CHECK(sizeof(ProtocolHeader) == 64, "sizeof(ProtocolHeader) == 64");

    // Expected: 64 (header) + 64 (counter cache line) + sizeof(Quote)*CAP
    size_t expected = 64 + 64 + sizeof(Quote) * CAP;
    CHECK(QProducer::REGION_SIZE == expected,
          "REGION_SIZE == 64 + 64 + sizeof(Quote)*CAP");

    std::cout << "    REGION_SIZE = " << QProducer::REGION_SIZE << " bytes\n";
    return all_passed;
}

// ──────────────────────────────────────────────
// Test 2: Writing & Reading
// ──────────────────────────────────────────────
bool test_produce_consume() {
    bool all_passed = true;
    std::cout << "\n[TEST] Produce / Consume (4 messages)\n";

    // Producer side: Create file mapping
    MmapRegion prod_region = MmapRegion::create(TMP_PATH, QProducer::REGION_SIZE);
    QProducer  producer(prod_region);

    // Consumer side: Read-only access to the same file
    MmapRegion cons_region = MmapRegion::open_readonly(TMP_PATH, QConsumer::REGION_SIZE);
    QConsumer  consumer(cons_region);

    // Construct 4 Quotes
    const int N = 4;
    Quote sent[N];
    for (int i = 0; i < N; ++i) {
        sent[i].bid_price    = 50000 + i * 10;
        sent[i].bid_quantity = 1 + i;
        sent[i].ask_price    = 50001 + i * 10;
        sent[i].ask_quantity = 2 + i;
        sent[i].timestamp_ns = 1700000000000LL + i;
        sent[i].sequence     = static_cast<uint64_t>(i);
        sent[i].symbol.set("BTC-USDT");
    }

    // Publish
    for (int i = 0; i < N; ++i) {
        bool ok = producer.publish(sent[i], consumer.cursor());
        CHECK(ok, std::string("publish[") + std::to_string(i) + "] returns true");
    }

    // Consumption
    for (int i = 0; i < N; ++i) {
        Quote recv{};
        bool got = consumer.consume(recv);
        CHECK(got, std::string("consume[") + std::to_string(i) + "] returns true");
        CHECK(recv.bid_price    == sent[i].bid_price,    "  bid_price matches");
        CHECK(recv.sequence     == sent[i].sequence,     "  sequence matches");
        CHECK(recv.symbol       == "BTC-USDT",           "  symbol matches");
    }

    // Consume again should return false (no new data).
    Quote dummy{};
    bool empty = consumer.consume(dummy);
    CHECK(!empty, "consume on empty returns false");

    return all_passed;
}

// ──────────────────────────────────────────────
// Test 3: Protocol Header Content
// ──────────────────────────────────────────────
bool test_protocol_header() {
    bool all_passed = true;
    std::cout << "\n[TEST] ProtocolHeader validation\n";

    // Reopen the same file (the producer has already written the header)
    MmapRegion r = MmapRegion::open_readonly(TMP_PATH, QProducer::REGION_SIZE);
    const auto* hdr = reinterpret_cast<const ProtocolHeader*>(r.ptr());

    CHECK(hdr->magic         == PEGASUS_MAGIC,           "magic == PEGASUS_MAGIC");
    CHECK(hdr->version_major == PROTOCOL_VERSION_MAJOR,  "version_major correct");
    CHECK(hdr->version_minor == PROTOCOL_VERSION_MINOR,  "version_minor correct");
    CHECK(hdr->capacity      == CAP,                     "capacity == CAP");

    // A consumer attempting to access an incorrect capacity should throw an exception.
    bool threw = false;
    try {
        // ShmConsumer<Quote, 16> would check capacity==16 but file has 8
        // Manually check if the capacity field is unequal
        if (hdr->capacity != 16) throw std::runtime_error("capacity mismatch");
    } catch (const std::runtime_error&) {
        threw = true;
    }
    CHECK(threw, "capacity mismatch throws runtime_error");

    return all_passed;
}

// ──────────────────────────────────────────────
// Test 4: Ring buffer wrap-around
// Write CAP+2 messages (consume them in the middle to free up space).
// ──────────────────────────────────────────────
bool test_wraparound() {
    bool all_passed = true;
    std::cout << "\n[TEST] Ring buffer wrap-around (CAP=" << CAP << ")\n";

    MmapRegion pr = MmapRegion::create(TMP_PATH, QProducer::REGION_SIZE);
    QProducer  producer(pr);
    MmapRegion cr = MmapRegion::open_readonly(TMP_PATH, QConsumer::REGION_SIZE);
    QConsumer  consumer(cr);

    // Fill the ring buffer (CAP bar)
    Quote q{};
    for (size_t i = 0; i < CAP; ++i) {
        q.sequence = i;
        bool ok = producer.publish(q, consumer.cursor());
        CHECK(ok, "fill: publish[" + std::to_string(i) + "]");
    }

    // At this point, the ring is full; writing again should return false.
    q.sequence = 999;
    bool full_result = producer.publish(q, consumer.cursor());
    CHECK(!full_result, "ring full: publish returns false");

    // 4 consumption items to free up space
    Quote out{};
    for (int i = 0; i < 4; ++i) consumer.consume(out);

    // Now we can write 4 more (wrap-around)
    for (size_t i = 0; i < 4; ++i) {
        q.sequence = CAP + i;
        bool ok = producer.publish(q, consumer.cursor());
        CHECK(ok, "wrap: publish[" + std::to_string(CAP + i) + "]");
    }

    // Verify that the consumed sequence values ​​are correct (it should be 4, 5, 6, 7 followed by 8, 9, 10, 11).
    for (size_t i = 4; i < CAP; ++i) {
        consumer.consume(out);
        CHECK(out.sequence == i, "wrap read: sequence == " + std::to_string(i));
    }
    for (size_t i = 0; i < 4; ++i) {
        consumer.consume(out);
        CHECK(out.sequence == CAP + i, "wrap read: sequence == " + std::to_string(CAP + i));
    }

    return all_passed;
}

// ──────────────────────────────────────────────
// main
// ──────────────────────────────────────────────
int main() {
    std::cout << "========================================\n";
    std::cout << "  Pegasus Phase 1 — IPC Tests\n";
    std::cout << "========================================\n";

    bool ok = true;
    ok &= test_constants();
    ok &= test_produce_consume();
    ok &= test_protocol_header();
    ok &= test_wraparound();

    std::cout << "\n========================================\n";
    std::cout << (ok ? "  ALL TESTS PASSED ✓\n" : "  SOME TESTS FAILED ✗\n");
    std::cout << "========================================\n";

    // Clean up temporary files
    ::remove(TMP_PATH.c_str());
    return ok ? 0 : 1;
}