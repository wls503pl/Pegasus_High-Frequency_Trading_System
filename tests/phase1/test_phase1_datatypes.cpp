/**
 * @file test_phase1_datatypes.cpp
 * @brief Phase 1 Validation: PegasusDataTypes.hpp
 *
 * Verification content:
 *   1. sizeof(Order) == sizeof(Trade) == sizeof(Quote) == 64
 *   2. The `alignof` parameter is always set to 64 (cache lines).
 *   3. Key field offsets conform to the design document
 *   4. FixedString zero initialization, set/compare behavior
 *   5. Side / OrdStatus enumeration underlying values
 */

#include <iostream>
#include <cstddef>
#include <cassert>
#include <cstring>
#include "PegasusDataTypes.hpp"

using namespace pegasus;

// ────────────────────────────────────────────────────────────────────────────────────────────
// Compile-time assertions (already included in the .hpp documentation, but reiterated here for clarity)
// ────────────────────────────────────────────────────────────────────────────────────────────
static_assert(sizeof(Order) == 64,  "Order must be 64 bytes");
static_assert(sizeof(Trade) == 64,  "Trade must be 64 bytes");
static_assert(sizeof(Quote) == 64,  "Quote must be 64 bytes");
static_assert(alignof(Order) == 64, "Order must be 64-byte aligned");
static_assert(alignof(Trade) == 64, "Trade must be 64-byte aligned");
static_assert(alignof(Quote) == 64, "Quote must be 64-byte aligned");

// ──────────────────────────────────────────────
// Helper macro: Print PASS / FAIL
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

// ──────────────────────────────────────────────
// Test 1: Size & Alignment
// ──────────────────────────────────────────────
bool test_sizes_and_alignment() {
    bool all_passed = true;
    std::cout << "\n[TEST] Sizes & Alignment\n";

    CHECK(sizeof(Order) == 64,  "sizeof(Order) == 64");
    CHECK(sizeof(Trade) == 64,  "sizeof(Trade) == 64");
    CHECK(sizeof(Quote) == 64,  "sizeof(Quote) == 64");
    CHECK(alignof(Order) == 64, "alignof(Order) == 64");
    CHECK(alignof(Trade) == 64, "alignof(Trade) == 64");
    CHECK(alignof(Quote) == 64, "alignof(Quote) == 64");

    return all_passed;
}

// ────────────────────────────────────────────────────────────────────────────────────────────
// Test 2: Order field offset
// Design document: [0..47] 6×8-byte, [48..57] symbol, [58] side, [59] status, [60..63] padding
// ────────────────────────────────────────────────────────────────────────────────────────────
bool test_order_layout() {
    bool all_passed = true;
    std::cout << "\n[TEST] Order Field Offsets\n";

    CHECK(offsetof(Order, order_id)        ==  0,  "order_id        @ byte  0");
    CHECK(offsetof(Order, client_order_id) ==  8,  "client_order_id @ byte  8");
    CHECK(offsetof(Order, price)           == 16,  "price           @ byte 16");
    CHECK(offsetof(Order, quantity)        == 24,  "quantity        @ byte 24");
    CHECK(offsetof(Order, filled_quantity) == 32,  "filled_quantity @ byte 32");
    CHECK(offsetof(Order, timestamp_ns)    == 40,  "timestamp_ns    @ byte 40");
    CHECK(offsetof(Order, symbol)          == 48,  "symbol          @ byte 48");
    CHECK(offsetof(Order, side)            == 58,  "side            @ byte 58");
    CHECK(offsetof(Order, status)          == 59,  "status          @ byte 59");
    CHECK(offsetof(Order, padding)         == 60,  "padding         @ byte 60");

    return all_passed;
}

// ──────────────────────────────────────────────
// Test 3: Trade field offset
// ──────────────────────────────────────────────
bool test_trade_layout() {
    bool all_passed = true;
    std::cout << "\n[TEST] Trade Field Offsets\n";

    CHECK(offsetof(Trade, trade_id)           ==  0, "trade_id           @ byte  0");
    CHECK(offsetof(Trade, aggressor_order_id) ==  8, "aggressor_order_id @ byte  8");
    CHECK(offsetof(Trade, passive_order_id)   == 16, "passive_order_id   @ byte 16");
    CHECK(offsetof(Trade, price)              == 24, "price              @ byte 24");
    CHECK(offsetof(Trade, quantity)           == 32, "quantity           @ byte 32");
    CHECK(offsetof(Trade, timestamp_ns)       == 40, "timestamp_ns       @ byte 40");
    CHECK(offsetof(Trade, symbol)             == 48, "symbol             @ byte 48");
    CHECK(offsetof(Trade, aggressor_side)     == 58, "aggressor_side     @ byte 58");
    CHECK(offsetof(Trade, padding)            == 59, "padding            @ byte 59");

    return all_passed;
}

// ──────────────────────────────────────────────
// Test 4: Quote field offset
// ──────────────────────────────────────────────
bool test_quote_layout() {
    bool all_passed = true;
    std::cout << "\n[TEST] Quote Field Offsets\n";

    CHECK(offsetof(Quote, bid_price)     ==  0, "bid_price     @ byte  0");
    CHECK(offsetof(Quote, bid_quantity)  ==  8, "bid_quantity  @ byte  8");
    CHECK(offsetof(Quote, ask_price)     == 16, "ask_price     @ byte 16");
    CHECK(offsetof(Quote, ask_quantity)  == 24, "ask_quantity  @ byte 24");
    CHECK(offsetof(Quote, timestamp_ns)  == 32, "timestamp_ns  @ byte 32");
    CHECK(offsetof(Quote, sequence)      == 40, "sequence      @ byte 40");
    CHECK(offsetof(Quote, symbol)        == 48, "symbol        @ byte 48");
    CHECK(offsetof(Quote, padding)       == 58, "padding       @ byte 58");

    return all_passed;
}

// ──────────────────────────────────────────────
// Test 5: FixedString Behavior
// ──────────────────────────────────────────────
bool test_fixed_string() {
    bool all_passed = true;
    std::cout << "\n[TEST] FixedString<10>\n";

    FixedString<10> s;

    // Default zero initialization
    CHECK(s.data[0] == '\0', "default-init: data[0] == 0");

    // Normal write of token pairs
    s.set("BTC-USDT");
    CHECK(s == "BTC-USDT", "set(\"BTC-USDT\") == \"BTC-USDT\"");
    CHECK(s.data[9] == '\0', "null-terminator preserved at [9]");

    // Excessive write truncation (9 characters + \0 → 10 bytes)
    s.set("ABCDEFGHI");  // 9 chars, fits exactly
    CHECK(s.data[9] == '\0', "9-char string: data[9] == 0");

    // Comparison of two FixedStrings
    FixedString<10> a, b;
    a.set("ETH-USDT");
    b.set("ETH-USDT");
    CHECK(a == b, "two FixedStrings with same content are equal");

    b.set("BTC-USDT");
    CHECK(!(a == b), "different content: not equal");

    return all_passed;
}

// ──────────────────────────────────────────────
// Test 6: Enum Underlying Values
// ──────────────────────────────────────────────
bool test_enums() {
    bool all_passed = true;
    std::cout << "\n[TEST] Enum underlying values\n";

    CHECK(static_cast<char>(Side::BUY)      == 'B', "Side::BUY == 'B'");
    CHECK(static_cast<char>(Side::SELL)     == 'S', "Side::SELL == 'S'");
    CHECK(static_cast<char>(OrdStatus::NEW)      == 'N', "OrdStatus::NEW == 'N'");
    CHECK(static_cast<char>(OrdStatus::PARTIAL)  == 'P', "OrdStatus::PARTIAL == 'P'");
    CHECK(static_cast<char>(OrdStatus::FILLED)   == 'F', "OrdStatus::FILLED == 'F'");
    CHECK(static_cast<char>(OrdStatus::CANCELED) == 'C', "OrdStatus::CANCELED == 'C'");

    return all_passed;
}

// ──────────────────────────────────────────────
// Test 7: Instantiate and populate the fields of Order.
// ──────────────────────────────────────────────
bool test_order_instance() {
    bool all_passed = true;
    std::cout << "\n[TEST] Order instantiation\n";

    Order o{};
    o.order_id        = 1001;
    o.client_order_id = 9001;
    o.price           = 6800000;  // 680.00 * 10000
    o.quantity        = 100;
    o.filled_quantity = 0;
    o.timestamp_ns    = 1700000000000000000LL;
    o.symbol.set("BTC-USDT");
    o.side            = Side::BUY;
    o.status          = OrdStatus::NEW;

    CHECK(o.order_id        == 1001,            "order_id == 1001");
    CHECK(o.price           == 6800000,         "price == 6800000");
    CHECK(o.symbol          == "BTC-USDT",      "symbol == \"BTC-USDT\"");
    CHECK(o.side            == Side::BUY,       "side == BUY");
    CHECK(o.status          == OrdStatus::NEW,  "status == NEW");

    // Verify that the instance is 64-byte aligned on the stack (only check if alignas is active).
    // `alignas` ensures alignment on the stack.
    CHECK((reinterpret_cast<uintptr_t>(&o) % 64) == 0, "stack instance 64-byte aligned");

    return all_passed;
}

// ──────────────────────────────────────────────
// main
// ──────────────────────────────────────────────
int main() {
    std::cout << "========================================\n";
    std::cout << "  Pegasus Phase 1 — DataTypes Tests\n";
    std::cout << "========================================\n";

    bool ok = true;
    ok &= test_sizes_and_alignment();
    ok &= test_order_layout();
    ok &= test_trade_layout();
    ok &= test_quote_layout();
    ok &= test_fixed_string();
    ok &= test_enums();
    ok &= test_order_instance();

    std::cout << "\n========================================\n";
    std::cout << (ok ? "  ALL TESTS PASSED ✓\n" : "  SOME TESTS FAILED ✗\n");
    std::cout << "========================================\n";
    return ok ? 0 : 1;
}