#pragma once
#include <cstdint>
#include <cstring>
#include <string>

/**
 * @namespace pegasus
 * @brief Core data structures for the Pegasus High-Frequency Trading System.
 *
 * This namespace contains the fundamental entities used across the system,
 * optimized for low-latency inter-process communication (IPC) via shared memory.
 * Structures are designed with explicit memory alignment and padding to prevent
 * false sharing and ensure predictable memory layout across different compilers
 * and architectures, which is critical for nanosecond-level performance.
 */
namespace pegasus {

// --- Type Aliases for Clarity and Precision ---
/**
 * @brief Price type, represented as a scaled integer.
 * Prices are scaled (e.g., 10000 = 100.00) to avoid floating-point inaccuracies
 * and ensure deterministic arithmetic in high-frequency environments.
 */
using Price = int64_t;

/**
 * @brief Quantity type for orders and trades.
 * Represents the volume of an asset, typically an integer or scaled integer.
 */
using Qty   = int64_t;

/**
 * @brief Order ID type.
 * A unique identifier for each order within the system.
 */
using OrdId = uint64_t;

/**
 * @brief Trade ID type.
 * A unique identifier for each trade execution.
 */
using TrdId = uint64_t;

/**
 * @enum Side
 * @brief Represents the side of an order or trade (Buy/Sell).
 * Stored as a char for minimal memory footprint and direct comparison.
 */
enum class Side : char {
    BUY = 'B',
    SELL = 'S'
};

/**
 * @enum OrdStatus
 * @brief Represents the current lifecycle state of an order.
 * Stored as a char for efficiency.
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
 * Critical for avoiding heap allocations and ensuring cache locality.
 * @tparam N The total size of the buffer including the null-terminator.
 */
template<size_t N>
struct FixedString {
    char data[N];

    FixedString() { std::memset(data, 0, N); }

    /**
     * @brief Safely copies a C-string into the fixed buffer.
     * Ensures null-termination and prevents buffer overflows.
     * @param src The source null-terminated string.
     */
    void set(const char* src) {
        // Use strncpy to safely copy, ensuring null-termination within bounds.
        std::strncpy(data, src, N - 1);
        data[N - 1] = '\0'; // Guarantee null-termination
    }

    /**
     * @brief Returns a const pointer to the internal C-string.
     */
    const char* c_str() const { return data; }

    /**
     * @brief Provides access to the underlying character array.
     */
    char* data_ptr() { return data; }

    /**
     * @brief Compares two FixedString objects.
     */
    bool operator==(const FixedString& other) const {
        return std::strcmp(data, other.data) == 0;
    }

    /**
     * @brief Compares FixedString with a C-string.
     */
    bool operator==(const char* other) const {
        return std::strcmp(data, other) == 0;
    }
};

/**
 * @struct Order
 * @brief Represents a single order in the system.
 *
 * Aligned to 64 bytes (standard CPU cache line) to prevent false sharing
 * and optimize memory throughput during high-frequency matching.
 * Fields are ordered to minimize implicit padding and maximize cache utilization.
 *
 * Layout (64 bytes, zero implicit padding):
 *   [0..47]  6x 8-byte fields
 *   [48..57] symbol[10]
 *   [58]     side, [59] status
 *   [60..63] padding[4]
 */
struct alignas(64) Order {
    // 8-byte aligned fields first — no implicit padding between them.
    OrdId           order_id;           // Internal unique order ID (8 bytes)
    OrdId           client_order_id;    // ID provided by the client/gateway (8 bytes)
    Price           price;              // Limit price of the order (8 bytes)
    Qty             quantity;           // Total requested quantity (8 bytes)
    Qty             filled_quantity;    // Accumulated filled quantity (8 bytes)
    int64_t         timestamp_ns;       // Creation timestamp in nanoseconds (8 bytes)
    // 1-byte aligned fields — no alignment gap after the 8-byte block above.
    FixedString<10> symbol;             // Trading pair symbol, e.g., "BTC-USDT" (10 bytes)
    Side            side;               // Buy or Sell (1 byte)
    OrdStatus       status;             // Current status (1 byte)
    char            padding[4];         // Explicit padding to reach exactly 64 bytes
};
static_assert(sizeof(Order) == 64, "Order must be exactly 64 bytes for cache-line alignment");

/**
 * @struct Trade
 * @brief Represents a successful execution between two orders.
 *
 * Aligned to 64 bytes to ensure efficient logging and asynchronous
 * processing by the Custody Verifier. Fields are ordered for optimal memory access.
 */
struct alignas(64) Trade {
    TrdId           trade_id;           // Unique trade identifier (8 bytes)
    OrdId           aggressor_order_id; // ID of the order that initiated the match (8 bytes)
    OrdId           passive_order_id;   // ID of the resting order that was matched (8 bytes)
    Price           price;              // Execution price (8 bytes)
    Qty             quantity;           // Executed quantity (8 bytes)
    int64_t         timestamp_ns;       // Execution timestamp in nanoseconds (8 bytes)
    FixedString<10> symbol;             // Trading pair symbol (10 bytes)
    Side            aggressor_side;     // Side of the aggressor (1 byte)
    char            padding[5];         // Explicit padding to reach exactly 64 bytes
};
static_assert(sizeof(Trade) == 64, "Trade must be exactly 64 bytes for cache-line alignment");

/**
 * @struct Quote
 * @brief Represents the best bid and offer (BBO) for a symbol.
 *
 * Used for market data dissemination and real-time valuation. Aligned to 64 bytes
 * for efficient cache utilization when being read or written.
 *
 * sequence is a monotonically increasing counter per symbol. The Matching Engine
 * detects gaps (received != last + 1) to catch dropped quotes from the Market
 * Data Gateway and suppress order generation until resync.
 */
struct alignas(64) Quote {
    Price           bid_price;          // Best bid price (8 bytes)
    Qty             bid_quantity;       // Quantity available at best bid (8 bytes)
    Price           ask_price;          // Best ask price (8 bytes)
    Qty             ask_quantity;       // Quantity available at best ask (8 bytes)
    int64_t         timestamp_ns;       // Quote update timestamp in nanoseconds (8 bytes)
    uint64_t        sequence;           // Monotonic sequence number for gap detection (8 bytes)
    FixedString<10> symbol;             // Trading pair symbol (10 bytes)
    char            padding[6];         // Explicit padding to reach exactly 64 bytes
};
static_assert(sizeof(Quote) == 64, "Quote must be exactly 64 bytes for cache-line alignment");

} // namespace pegasus