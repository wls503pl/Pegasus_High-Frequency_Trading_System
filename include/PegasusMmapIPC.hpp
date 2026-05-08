#pragma once
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <atomic>
#include <cstring>
#include <stdexcept>
#include <string>
#include <type_traits>

#include "PegasusDataTypes.hpp"

/**
 * @file PegasusMmapIPC.hpp
 * @brief Shared memory IPC for Pegasus: Market Data Gateway → Matching Engine
 *        and Matching Engine → Custody Verifier.
 *
 * Design philosophy: simple, maintainable, correct.
 *
 *   - One counter only. The producer writes a monotonically increasing
 *     write_counter after copying each message. The consumer reads it to
 *     check for new data. No second counter, no two-phase commit, no
 *     opportunity to leave the counters in an inconsistent state.
 *
 *   - Three classes, three responsibilities:
 *       MmapRegion   — owns the mmap lifetime (open, size, map, unmap).
 *       ShmProducer  — writes messages and advances write_counter.
 *       ShmConsumer  — reads messages via its own private cursor.
 *
 *   - Consumer maps read-only. The kernel enforces this at the hardware
 *     level: a stray consumer write segfaults immediately rather than
 *     silently corrupting the producer's data.
 *
 * Segment layout (flat, no hidden offsets):
 *
 *   [ ProtocolHeader  : 64 bytes ] — magic, version, capacity
 *   [ write_counter   : 64 bytes ] — alignas(64), producer writes only
 *   [ T[Capacity]                ] — ring buffer of POD message structs
 */

namespace pegasus {

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

// "PEGASUS\0" in ASCII — recognisable in a hex dump.
static constexpr uint64_t PEGASUS_MAGIC        = 0x5045474153555300ULL;
static constexpr uint32_t PROTOCOL_VERSION_MAJOR = 1;
static constexpr uint32_t PROTOCOL_VERSION_MINOR = 0;

// ---------------------------------------------------------------------------
// ProtocolHeader
// ---------------------------------------------------------------------------

/**
 * @struct ProtocolHeader
 * @brief First 64 bytes of every mmap segment.
 *
 * Each segment is opened independently by each process (separate shm_open +
 * mmap calls, no shared context). The header lets the consumer verify it
 * opened the right file before touching any ring buffer data:
 *
 *   magic    — detects wrong file path or a segment left by a different binary.
 *   version  — detects producer/consumer ABI mismatch after a redeploy.
 *   capacity — lets the consumer compute slot offsets without hardcoding size.
 */
struct ProtocolHeader {
    uint64_t magic;            // Must equal PEGASUS_MAGIC
    uint32_t version_major;    // Incompatible change → bump major
    uint32_t version_minor;    // Additive change → bump minor
    uint64_t capacity;         // Ring buffer slot count
    uint8_t  reserved[40];     // Future use — keeps struct at 64 bytes
};
static_assert(sizeof(ProtocolHeader) == 64,
    "ProtocolHeader must be 64 bytes so write_counter starts on a fresh cache line.");

// ---------------------------------------------------------------------------
// MmapRegion
// ---------------------------------------------------------------------------

/**
 * @class MmapRegion
 * @brief RAII owner of a memory-mapped file. One responsibility: lifetime.
 *
 * Does not know about ring buffers, counters, or message types.
 * ShmProducer and ShmConsumer receive a raw pointer from this class
 * and interpret the layout themselves.
 *
 * Move-only: two owners of the same mapping would make it impossible to
 * know which one should call munmap on destruction.
 */
class MmapRegion {
public:
    /**
     * @brief Producer factory: creates (or truncates) the backing file and
     *        maps it read-write. Zeroes the entire region so padding bytes
     *        in POD structs are deterministic — important for ECDSA signing
     *        in the Custody layer where non-deterministic bytes produce
     *        non-deterministic signatures.
     */
    static MmapRegion create(const std::string& path, size_t size) {
        // O_TRUNC: wipe any stale segment from a previous run so a consumer
        // never reads a header written by an older, incompatible binary.
        int fd = ::open(path.c_str(), O_CREAT | O_RDWR | O_TRUNC, 0666);
        if (fd < 0)
            throw std::runtime_error("MmapRegion::create open failed: " + path);

        if (::ftruncate(fd, static_cast<off_t>(size)) < 0) {
            ::close(fd);
            throw std::runtime_error("MmapRegion::create ftruncate failed: " + path);
        }

        void* ptr = ::mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        ::close(fd); // fd not needed after mmap
        if (ptr == MAP_FAILED)
            throw std::runtime_error("MmapRegion::create mmap failed: " + path);

        std::memset(ptr, 0, size);
        return MmapRegion{ptr, size};
    }

    /**
     * @brief Consumer factory: opens an existing segment read-only.
     *        The kernel rejects any write through this mapping with SIGSEGV,
     *        providing hardware-enforced protection of the producer's data.
     */
    static MmapRegion open_readonly(const std::string& path, size_t size) {
        int fd = ::open(path.c_str(), O_RDONLY);
        if (fd < 0)
            throw std::runtime_error("MmapRegion::open_readonly open failed: " + path);

        void* ptr = ::mmap(nullptr, size, PROT_READ, MAP_SHARED, fd, 0);
        ::close(fd);
        if (ptr == MAP_FAILED)
            throw std::runtime_error("MmapRegion::open_readonly mmap failed: " + path);

        return MmapRegion{ptr, size};
    }

    void*  ptr()  const { return ptr_; }
    size_t size() const { return size_; }

    ~MmapRegion() {
        if (ptr_ != MAP_FAILED) ::munmap(ptr_, size_);
    }

    MmapRegion(const MmapRegion&)            = delete;
    MmapRegion& operator=(const MmapRegion&) = delete;

    MmapRegion(MmapRegion&& o) noexcept : ptr_(o.ptr_), size_(o.size_) {
        o.ptr_ = MAP_FAILED;
    }
    MmapRegion& operator=(MmapRegion&& o) noexcept {
        if (this != &o) {
            if (ptr_ != MAP_FAILED) ::munmap(ptr_, size_);
            ptr_ = o.ptr_; size_ = o.size_; o.ptr_ = MAP_FAILED;
        }
        return *this;
    }

private:
    MmapRegion(void* ptr, size_t size) : ptr_(ptr), size_(size) {}
    void*  ptr_  = MAP_FAILED;
    size_t size_ = 0;
};

// ---------------------------------------------------------------------------
// ShmProducer
// ---------------------------------------------------------------------------

/**
 * @class ShmProducer<T, Capacity>
 * @brief Writes POD messages into a shared memory ring buffer.
 *
 * One counter design:
 *   write_counter is incremented once, after the memcpy completes.
 *   The consumer sees the increment only after the copy is visible
 *   (release store pairs with the consumer's acquire load).
 *   There is no intermediate state where the counter is advanced but
 *   the data is not yet written — the simplest possible protocol.
 *
 * @tparam T         POD message type (Order, Trade, Quote).
 * @tparam Capacity  Ring buffer slot count. Must be a power of two.
 */
template<typename T, size_t Capacity>
class ShmProducer {
    static_assert(std::is_trivially_copyable<T>::value,
        "T must be trivially copyable — no std::string, no pointers.");
    static_assert((Capacity & (Capacity - 1)) == 0,
        "Capacity must be a power of two for cheap modulo arithmetic.");

public:
    // Total bytes needed for the mmap segment.
    static constexpr size_t REGION_SIZE =
        sizeof(ProtocolHeader) +
        64 +                   // write_counter on its own cache line
        sizeof(T) * Capacity;

    /**
     * @brief Constructs the producer, writes the ProtocolHeader, and
     *        initialises write_counter to zero.
     * @param region  An MmapRegion created with MmapRegion::create().
     */
    explicit ShmProducer(MmapRegion& region) {
        base_     = static_cast<uint8_t*>(region.ptr());
        counter_  = reinterpret_cast<std::atomic<uint64_t>*>(base_ + sizeof(ProtocolHeader));
        buffer_   = reinterpret_cast<T*>(base_ + sizeof(ProtocolHeader) + 64);

        // Write the header so consumers can validate before reading.
        auto* hdr         = reinterpret_cast<ProtocolHeader*>(base_);
        hdr->magic        = PEGASUS_MAGIC;
        hdr->version_major = PROTOCOL_VERSION_MAJOR;
        hdr->version_minor = PROTOCOL_VERSION_MINOR;
        hdr->capacity     = Capacity;

        counter_->store(0, std::memory_order_relaxed);
    }

    /**
     * @brief Copies @p msg into the next ring buffer slot and publishes it.
     *
     * Steps (order matters):
     *   1. memcpy the message into the slot — data is written first.
     *   2. Release store on write_counter — makes step 1 visible to consumers.
     *
     * The consumer's acquire load on write_counter pairs with this release
     * to form a happens-before relationship: the consumer always sees the
     * complete message, never a partial write.
     *
     * @return false if the ring buffer is full (consumer hasn't caught up).
     */
    bool publish(const T& msg, uint64_t consumer_read_cursor) {
        uint64_t write = counter_->load(std::memory_order_relaxed);

        // Ring buffer full: consumer is Capacity slots behind.
        if (write - consumer_read_cursor >= Capacity) return false;

        // Step 1: write the data before advancing the counter.
        std::memcpy(&buffer_[write & (Capacity - 1)], &msg, sizeof(T));

        // Step 2: publish — release store ensures step 1 is visible first.
        counter_->store(write + 1, std::memory_order_release);
        return true;
    }

    uint64_t write_count() const {
        return counter_->load(std::memory_order_relaxed);
    }

private:
    uint8_t*                    base_    = nullptr;
    std::atomic<uint64_t>*      counter_ = nullptr; // alignas(64) via memset layout
    T*                          buffer_  = nullptr;
};

// ---------------------------------------------------------------------------
// ShmConsumer
// ---------------------------------------------------------------------------

/**
 * @class ShmConsumer<T, Capacity>
 * @brief Reads POD messages from a shared memory ring buffer.
 *
 * The consumer holds its own private read cursor (cursor_). It is not in
 * shared memory — each consumer process manages its own position independently,
 * with no coordination required with the producer or other consumers.
 *
 * Validates the ProtocolHeader on construction to catch:
 *   - Wrong file path (magic mismatch)
 *   - Producer/consumer binary version mismatch (major version)
 *   - Ring buffer size mismatch (capacity)
 */
template<typename T, size_t Capacity>
class ShmConsumer {
    static_assert(std::is_trivially_copyable<T>::value,
        "T must be trivially copyable.");
    static_assert((Capacity & (Capacity - 1)) == 0,
        "Capacity must be a power of two.");

public:
    static constexpr size_t REGION_SIZE = ShmProducer<T, Capacity>::REGION_SIZE;

    /**
     * @brief Constructs the consumer and validates the ProtocolHeader.
     * @param region  An MmapRegion opened with MmapRegion::open_readonly().
     * @throws std::runtime_error if the header is invalid.
     */
    explicit ShmConsumer(MmapRegion& region) {
        const uint8_t* base = static_cast<const uint8_t*>(region.ptr());
        counter_ = reinterpret_cast<const std::atomic<uint64_t>*>(
            base + sizeof(ProtocolHeader));
        buffer_  = reinterpret_cast<const T*>(base + sizeof(ProtocolHeader) + 64);

        // Validate header before reading any ring buffer data.
        const auto* hdr = reinterpret_cast<const ProtocolHeader*>(base);
        if (hdr->magic != PEGASUS_MAGIC)
            throw std::runtime_error("ShmConsumer: magic mismatch — wrong file or stale segment");
        if (hdr->version_major != PROTOCOL_VERSION_MAJOR)
            throw std::runtime_error("ShmConsumer: version_major mismatch");
        if (hdr->capacity != Capacity)
            throw std::runtime_error("ShmConsumer: capacity mismatch");
    }

    /**
     * @brief Attempts to read the next message. Non-blocking.
     *
     * The acquire load on write_counter pairs with the producer's release
     * store, ensuring the full message is visible before we copy it.
     *
     * @param[out] out  Populated if a new message is available.
     * @return true if a message was consumed, false if nothing new.
     */
    bool consume(T& out) {
        uint64_t available = counter_->load(std::memory_order_acquire);
        if (available == cursor_) return false; // No new messages

        std::memcpy(&out, &buffer_[cursor_ & (Capacity - 1)], sizeof(T));
        ++cursor_;
        return true;
    }

    /** @brief Current read position. Useful for passing to ShmProducer::publish()
     *         so the producer can detect a full ring buffer. */
    uint64_t cursor() const { return cursor_; }

private:
    const std::atomic<uint64_t>* counter_ = nullptr;
    const T*                     buffer_  = nullptr;
    uint64_t                     cursor_  = 0; // Private — not in shared memory
};

// ---------------------------------------------------------------------------
// Convenience aliases
// ---------------------------------------------------------------------------

/** Quote channel: Market Data Gateway (producer) → Matching Engine (consumer). */
using QuoteProducer = ShmProducer<Quote, 1024>;
using QuoteConsumer = ShmConsumer<Quote, 1024>;

/** Trade channel: Matching Engine (producer) → Custody Verifier (consumer). */
using TradeProducer = ShmProducer<Trade, 1024>;
using TradeConsumer = ShmConsumer<Trade, 1024>;

} // namespace pegasus