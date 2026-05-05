#pragma once
#include <cstdint>
#include <cstring>

namespace pegasus {

using Price = int64_t;
using Qty   = int64_t;
using OrdId = uint64_t;
using TrdId = uint64_t;

enum class Side      : char { BUY = 'B', SELL = 'S' };
enum class OrdStatus : char { NEW = 'N', PARTIAL = 'P', FILLED = 'F', CANCELED = 'C' };

// Manus 建议4：替代 strncpy，固定长度字符串，强制末尾 \0
template<size_t N>
struct FixedString {
    char data[N];

    FixedString() { std::memset(data, 0, N); }

    void set(const char* src) {
        size_t len = 0;
        while (len < N - 1 && src[len] != '\0') {
            data[len] = src[len];
            ++len;
        }
        data[len] = '\0'; // 强制末尾 \0
    }

    const char* c_str() const { return data; }
};

struct alignas(64) Order {
    OrdId           order_id;           // 8
    OrdId           client_order_id;    // 8
    Price           price;              // 8
    Qty             quantity;           // 8
    Qty             filled_quantity;    // 8
    Side            side;               // 1
    OrdStatus       status;             // 1
    FixedString<10> symbol;             // 10
    int64_t         timestamp_ns;       // 8
    char            padding[2];         // 2 → total = 64
};
static_assert(sizeof(Order) == 64, "Order must be 64 bytes");

struct alignas(64) Trade {
    TrdId           trade_id;           // 8
    OrdId           aggressor_order_id; // 8
    OrdId           passive_order_id;   // 8
    Price           price;              // 8
    Qty             quantity;           // 8
    Side            aggressor_side;     // 1
    FixedString<10> symbol;             // 10
    int64_t         timestamp_ns;       // 8
    char            padding[13];        // 13 → total = 64
};
static_assert(sizeof(Trade) == 64, "Trade must be 64 bytes");

struct alignas(64) Quote {
    Price           bid_price;          // 8
    Qty             bid_quantity;       // 8
    Price           ask_price;          // 8
    Qty             ask_quantity;       // 8
    FixedString<10> symbol;             // 10
    int64_t         timestamp_ns;       // 8
    char            padding[14];        // 14 → total = 64
};
static_assert(sizeof(Quote) == 64, "Quote must be 64 bytes");

} // namespace pegasus