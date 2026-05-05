#pragma once
#include <cstdint>
#include <cstring>

/**
 * @namespace pegasus
 * @brief Core data structures for the Pegasus High-Frequency Trading System.
 * 
 * This namespace contains the fundamental entities used across the system, 
 * optimized for low-latency inter-process communication (IPC) via shared memory.
 */
namespace pegasus {

// --- Type Aliases for Clarity and Precision ---
using Price = int64_t;  // Prices are represented as scaled integers (e.g., 10000 = 100.00) to avoid floating-point errors.
using Qty   = int64_t;  // Quantities for orders and trades.
using OrdId = uint64_t; // Unique identifier for orders.
using TrdId = uint64_t; // Unique identifier for trades.

/**
 * @enum Side
 * @brief Represents the side of an order or trade.
 */
enum class Side : char { 
    BUY = 'B', 
    SELL = 'S' 
};

/**
 * @enum OrdStatus
 * @brief Represents the current lifecycle state of an order.
 */
enum class OrdStatus : char { 
    NEW = 'N', 
    PARTIAL = 'P', 
    FILLED = 'F', 
    CANCELED = 'C' 
};

/**
 * @struct FixedString
 * @brief A fixed-length string buffer optimized for HFT.
 * 
 * Replaces standard string types to ensure predictable memory layout and 
 * zero dynamic allocation. It enforces a null-terminator at the end.
 * @tparam N The total size of the buffer including the null-terminator.
 */
template<size_t N>
struct FixedString {
    char data[N];

    FixedString() { std::memset(data, 0, N); }

    /**
     * @brief Safely copies a C-string into the fixed buffer.
     * @param src The source null-terminated string.
     */
    void set(const char* src) {
        size_t len = 0;
        while (len < N - 1 && src[len] != '\0') {
            data[len] = src[len];
            ++len;
        }
        data[len] = '\0'; // Guaranteed null-termination
    }

    const char* c_str() const { return data; }
};

/**
 * @struct Order
 * @brief Represents a single order in the system.
 * 
 * Aligned to 64 bytes (standard CPU cache line) to prevent false sharing 
 * and optimize memory throughput during high-frequency matching.
 */
struct alignas(64) Order {
    OrdId           order_id;           // Internal unique order ID (8 bytes)
    OrdId           client_order_id;    // ID provided by the client/gateway (8 bytes)
    Price           price;              // Limit price of the order (8 bytes)
    Qty             quantity;           // Total requested quantity (8 bytes)
    Qty             filled_quantity;    // Accumulated filled quantity (8 bytes)
    Side            side;               // Buy or Sell (1 byte)
    OrdStatus       status;             // Current status (1 byte)
    FixedString<10> symbol;             // Trading pair symbol, e.g., "BTC-USDT" (10 bytes)
    int64_t         timestamp_ns;       // Creation timestamp in nanoseconds (8 bytes)
    char            padding[2];         // Explicit padding to reach exactly 64 bytes
};
static_assert(sizeof(Order) == 64, "Order must be exactly 64 bytes for cache-line alignment");

/**
 * @struct Trade
 * @brief Represents a successful execution between two orders.
 * 
 * Aligned to 64 bytes to ensure efficient logging and asynchronous 
 * processing by the Custody Verifier.
 */
struct alignas(64) Trade {
    TrdId           trade_id;           // Unique trade identifier (8 bytes)
    OrdId           aggressor_order_id; // ID of the order that initiated the match (8 bytes)
    OrdId           passive_order_id;   // ID of the resting order that was matched (8 bytes)
    Price           price;              // Execution price (8 bytes)
    Qty             quantity;           // Executed quantity (8 bytes)
    Side            aggressor_side;     // Side of the aggressor (1 byte)
    FixedString<10> symbol;             // Trading pair symbol (10 bytes)
    int64_t         timestamp_ns;       // Execution timestamp in nanoseconds (8 bytes)
    char            padding[13];        // Explicit padding to reach exactly 64 bytes
};
static_assert(sizeof(Trade) == 64, "Trade must be exactly 64 bytes for cache-line alignment");

/**
 * @struct Quote
 * @brief Represents the best bid and offer (BBO) for a symbol.
 * 
 * Used for market data dissemination and real-time valuation.
 */
struct alignas(64) Quote {
    Price           bid_price;          // Best bid price (8 bytes)
    Qty             bid_quantity;       // Quantity available at best bid (8 bytes)
    Price           ask_price;          // Best ask price (8 bytes)
    Qty             ask_quantity;       // Quantity available at best ask (8 bytes)
    FixedString<10> symbol;             // Trading pair symbol (10 bytes)
    int64_t         timestamp_ns;       // Quote update timestamp in nanoseconds (8 bytes)
    char            padding[14];        // Explicit padding to reach exactly 64 bytes
};
static_assert(sizeof(Quote) == 64, "Quote must be exactly 64 bytes for cache-line alignment");

} // namespace pegasus
