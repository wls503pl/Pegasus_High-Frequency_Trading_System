# Pegasus Phase 1 — Core Infrastructure

> *Before anything trades, we need to decide: what does a message look like, how does it travel between processes, and how do we measure if it's fast enough?*
> *Phase 1 answers all three.*

---

## The Starting Point

Pegasus is built as three independent processes:

```
Market Data Gateway  ──────▶  Matching Engine  ──────▶  Custody Verifier
   (receives quotes)            (matches orders)           (verifies trades)
```

Each arrow is a channel where one process sends data to another — thousands of times per second. Before writing a single line of matching logic, we need to settle three things that everything else depends on:

1. **What shape is the data?** — the structs both sides agree on
2. **How does data cross the process boundary?** — the IPC mechanism
3. **How do we know if it's actually fast?** — the timing tool

These are the three headers in Phase 1.

---

## Part 1 — The Data: `PegasusDataTypes.hpp`

### Why not just use normal structs?

Let's say we write a naive order struct:

```cpp
struct Order {
    std::string symbol;   // "BTC-USDT"
    double      price;    // 680.00
    int         quantity;
    bool        is_buy;
};
```

This works fine in a normal application. In an HFT system, it has two fatal problems.

**Problem 1: `std::string` allocates memory on the heap.**

Every time you construct or copy a `std::string`, there's a call to `malloc` somewhere underneath. Heap allocation is slow (it needs a lock in the allocator), unpredictable in latency, and puts your string data somewhere else in memory — far from the struct itself. To read the symbol, the CPU must first read the struct to find the pointer, then follow that pointer to another location. Two memory fetches where one would do.

**Problem 2: `double` is non-deterministic.**

`680.00 * 10` might not equal `6800.00` in IEEE 754 floating point — it depends on the CPU, the compiler flags, and the FPU rounding mode. In financial systems, price arithmetic must be **exact and reproducible**. A matching engine where `price_a + price_b` gives a different result on two different machines is simply wrong.

### The solution: two design rules

**Rule 1: No heap. All data lives inside the struct.**

We replace `std::string` with a fixed-size character buffer embedded directly in the struct:

```cpp
template<size_t N>
struct FixedString {
    char data[N];   // the characters live HERE, not somewhere on the heap
};
```

A `FixedString<10>` occupies exactly 10 bytes, right inside the struct. Reading the symbol and reading the price are the same memory fetch.

**Rule 2: No floats. Prices are scaled integers.**

```cpp
using Price = int64_t;

// $680.00 is stored as 6800000  (scaled by 10,000)
// $680.01 is stored as 6800100
// arithmetic is exact, always
```

Multiplication, addition, comparison — all integer operations. No rounding surprises.

### The cache line: why 64 bytes matters

A CPU doesn't read individual bytes from RAM. It reads **64 bytes at a time** — one cache line. This is the fundamental unit of memory transfer:

```
RAM ──── 64 bytes at a time ────▶ L3 ──▶ L2 ──▶ L1 ──▶ CPU registers
```

If a struct is 72 bytes, reading it takes **two** cache line fetches. If it's exactly 64 bytes and starts on a 64-byte boundary, it takes **one** — the theoretical minimum.

So every message struct in Pegasus is declared with:

```cpp
struct alignas(64) Order { ... };
static_assert(sizeof(Order) == 64, "Order must be exactly 64 bytes");
```

`alignas(64)` tells the compiler: any instance of `Order` must start at a memory address divisible by 64. The `static_assert` fails at **compile time** if the layout is ever accidentally broken — no runtime surprise.

### Field ordering: a practical trick

To hit exactly 64 bytes with no wasted space, field ordering matters. Consider:

```cpp
// ❌ Wastes 7 bytes
struct Bad {
    char    side;      // 1 byte
    // [7 bytes implicit padding — compiler inserted]
    int64_t price;     // needs 8-byte alignment
};

// ✅ Zero waste
struct Good {
    int64_t price;     // 8 bytes, aligned
    char    side;      // 1 byte, no alignment requirement after an 8-byte field
};
```

The rule: put 8-byte fields first, smaller fields last. This way the compiler never needs to insert padding between fields. Every byte in the struct is intentional.

### The final layouts

**Order** — one cache line, zero implicit padding:

```
[  0 –  7]  order_id          uint64_t   — internal unique ID
[  8 – 15]  client_order_id   uint64_t   — ID provided by the client
[ 16 – 23]  price             int64_t    — scaled, e.g. 6800000 = $680.00
[ 24 – 31]  quantity          int64_t    — total requested
[ 32 – 39]  filled_quantity   int64_t    — accumulated fills
[ 40 – 47]  timestamp_ns      int64_t    — nanoseconds since epoch
[ 48 – 57]  symbol            char[10]   — e.g. "BTC-USDT\0"
[ 58]       side              char       — 'B' (buy) or 'S' (sell)
[ 59]       status            char       — 'N' 'P' 'F' 'C'
[ 60 – 63]  padding           char[4]    — explicit, zeroed
─────────────────────────────────────────────────────────
            Total             64 bytes
```

`Trade` and `Quote` follow the same principle — 64 bytes each, `alignas(64)`, `static_assert` guarded.

One last detail: the `Side` and `OrdStatus` enums use `char` as their underlying type:

```cpp
enum class Side : char { BUY = 'B', SELL = 'S' };
```

A plain `enum class` defaults to `int` — 4 bytes. Adding `: char` makes it exactly 1 byte, which is how these fields fit into the tail of the struct without bloating the size.

---

## Part 2 — The Channel: `PegasusMmapIPC.hpp`

### Why not sockets or pipes?

Every C++ programmer knows how to connect two processes: open a socket, call `send()` and `recv()`. Simple and reliable.

The problem is that `send()` and `recv()` are **syscalls**. Every syscall crosses the user-kernel boundary: the CPU switches privilege level, the kernel validates arguments, copies data to a kernel buffer, switches back. Even on a modern Linux system, this overhead is measured in **microseconds** — thousands of nanoseconds.

When the goal is a hot path under 100 nanoseconds, spending a microsecond per message on IPC is not an option.

### Memory-mapped files: a zero-copy channel

`mmap()` maps a file into a process's virtual address space. When two processes map the **same file**, they see the same physical memory pages:

```
Process A                   Physical RAM                   Process B
virtual address             ─────────────                  virtual address
0x7f...1000  ─────────────▶ [same pages] ◀─────────────  0x7f...8000
     │                                                          │
     └── write here ══════ no syscall, no copy ══════ read here ┘
```

After the initial `mmap()` setup — which happens once, at startup — reading and writing the shared region is just **memory access**. The kernel is not involved on the hot path. No context switches, no copies, no locks.

This is the mechanism Pegasus uses for both channels: Gateway → Engine (quotes), and Engine → Custody (trades).

### The segment layout

Every shared memory segment has the same three-zone layout:

```
┌─────────────────────────────────────────────┐  offset 0
│  ProtocolHeader  (64 bytes)                 │
│  magic | version | capacity                 │
├─────────────────────────────────────────────┤  offset 64
│  write_counter   (64 bytes)                 │
│  std::atomic<uint64_t>                      │
├─────────────────────────────────────────────┤  offset 128
│  Ring Buffer  (sizeof(T) × Capacity)        │
│  T[0] | T[1] | ... | T[Capacity-1]          │
└─────────────────────────────────────────────┘
```

**Zone 1 — ProtocolHeader**

Before reading a single byte of ring buffer data, the consumer checks this header:

```cpp
struct ProtocolHeader {
    uint64_t magic;          // "PEGASUS\0" in ASCII bytes
    uint32_t version_major;  // breaking ABI change → bump this
    uint32_t version_minor;  // additive change → bump this
    uint64_t capacity;       // ring buffer slot count
    uint8_t  reserved[40];   // pads to exactly 64 bytes
};
```

If a process crashes and leaves a stale segment file, the next run of the consumer opens it, reads the magic — it doesn't match — and throws an error immediately, rather than silently reading garbage as if it were valid market data.

**Zone 2 — write_counter on its own cache line**

The counter gets its own dedicated 64-byte zone, separate from both the header and the ring buffer. The reason: the producer updates this counter on every single message. If it shared a cache line with an adjacent field, every counter update would cause the CPU cache coherence protocol to invalidate those adjacent bytes on the consumer's CPU — unnecessary traffic that slows reads. Isolation eliminates this.

**Zone 3 — Ring buffer**

Contiguous message slots. The producer writes into slot `write_counter & (Capacity - 1)`, which arithmetically equivalent to `write_counter % Capacity`, but this is faster(Because Capacity is a power of two, & (Capacity - 1) is arithmetically identical to (% Capacity) — the CPU executes a bitwise AND in a single cycle instead of an integer division which takes ~20–40 cycles.), then advances the counter. The consumer reads from slot `cursor & (Capacity - 1)`, then advances its cursor.

### The one-counter protocol

Many ring buffer designs keep two counters in shared memory: one for the producer's write position, one for the consumer's read position. Pegasus uses **one** — and the consumer's cursor lives entirely in the consumer's own private memory.

**Producer:**
```cpp
// Step 1: write the message data into the ring slot
std::memcpy(&buffer_[write & (Capacity - 1)], &msg, sizeof(T));

// Step 2: only THEN advance the counter
counter_->store(write + 1, std::memory_order_release);
//                          ↑ release: guarantees step 1 is visible first
```

**Consumer:**
```cpp
// acquire: pairs with producer's release store
uint64_t available = counter_->load(std::memory_order_acquire);

if (available == cursor_) return false;  // nothing new

// safe to read — the release/acquire pair guarantees the data is there
std::memcpy(&out, &buffer_[cursor_ & (Capacity - 1)], sizeof(T));
++cursor_;  // private to this consumer, not in shared memory
```

The `memory_order_release` / `memory_order_acquire` pair is the heart of correctness. In the C++ memory model, a release store **happens-before** any acquire load that observes its value. Concretely: if the consumer sees `write_counter == n+1`, it is **guaranteed** that the producer's `memcpy` into slot `n` is fully visible. Without this ordering, the consumer could read the counter, see it advanced, then load stale bytes from the slot — because CPUs and compilers are permitted to reorder memory operations unless told otherwise.

The consumer's cursor stays private because there's no reason to share it. The producer only needs it when checking whether the ring is full — and gets it as a parameter. This avoids an extra atomic write on every `consume()` call.

### Three classes, one job each

The design deliberately separates three concerns:

**`MmapRegion`** owns the `mmap` lifetime. Nothing else. It opens the file, maps it, and calls `munmap` on destruction. It is move-only: if two objects owned the same mapping, both would call `munmap` on destruction — a double-free. Move semantics ensure exactly one owner at all times.

**`ShmProducer<T, Capacity>`** knows how to write messages. It writes the `ProtocolHeader` on construction — so the consumer can validate before touching any data — then exposes one method: `publish()`.

**`ShmConsumer<T, Capacity>`** knows how to read messages. It validates the header on construction, then exposes one method: `consume()`. It owns its private cursor.

The consumer maps the segment **read-only** (`PROT_READ`, no `PROT_WRITE`). The kernel's page table enforces this at the hardware level: any accidental write by the consumer causes an immediate `SIGSEGV`. The bug surfaces as a crash, not as silent corruption of the producer's ring buffer.

### The power-of-two trick

Slot index for message `n` is `n % Capacity`. But `%` compiles to a division — slow. When `Capacity` is a power of two, modulo becomes a bitwise AND:

```cpp
n % 1024      // division — ~20 cycles
n & (1023)    // AND      —   1 cycle
```

This is enforced at compile time:

```cpp
static_assert((Capacity & (Capacity - 1)) == 0, "must be power of two");
```

`(Capacity & (Capacity - 1)) == 0` is a classic bit trick: subtracting 1 from a power of two flips all the trailing zero bits to ones and clears the leading one. ANDing with the original gives zero — and only for powers of two.

---

## Part 3 — The Ruler: `PegasusRDTSCTimer.hpp`

### Why `clock_gettime()` isn't enough

`clock_gettime(CLOCK_MONOTONIC, &ts)` is the standard way to get the current time in Linux. It's accurate. But it's a syscall, and even with vDSO optimizations, it costs **500–1000 CPU cycles** per call — roughly 150–300 nanoseconds at 3 GHz.

If the code path you want to measure takes 80 nanoseconds, and the timer itself takes 150 nanoseconds, you are not measuring your code. You are measuring the timer. We need something much cheaper.

### The TSC: a clock that costs almost nothing

Every modern x86 CPU has a **Time Stamp Counter** — a 64-bit hardware register that increments once per clock cycle, continuously. It is readable from user space with a single instruction:

```
rdtscp  →  reads EDX:EAX with the current cycle count
```

Cost: roughly **20–40 cycles**. That's 10–20× cheaper than `clock_gettime`. The output is in CPU cycles, not nanoseconds — we convert using a calibration factor calculated once at startup.

### The out-of-order problem

Modern CPUs execute instructions **out of order** to keep execution units busy. That means `rdtscp` might execute before the code you intended to measure has finished, or after code you didn't intend to measure has started. Your timing would be wrong.

The solution is **serializing barriers** around the TSC read:

```cpp
inline uint64_t rdtscp() {
    uint32_t lo, hi;
    __asm__ __volatile__("lfence" : : : "memory");   // drain pending loads
    __asm__ __volatile__("rdtscp" : "=a"(lo), "=d"(hi) : : "ecx");
    __asm__ __volatile__("lfence" : : : "memory");   // prevent reorder past here
    return (static_cast<uint64_t>(hi) << 32) | lo;
}
```

`lfence` is a load fence: it tells the CPU to complete all pending memory reads before proceeding. The pair of `lfence` instructions — one before, one after `rdtscp` — ensures the counter is read at exactly the right moment, with nothing slipping before or after.

### Calibration: cycles → nanoseconds

The TSC ticks at the CPU's reference frequency. A 3.2 GHz CPU increments the TSC 3.2 times per nanosecond. We measure this ratio once at startup, by reading both the TSC and the wall clock over a 200ms window:

```cpp
uint64_t c0 = rdtscp();
auto     t0 = steady_clock::now();

sleep_for(milliseconds(200));

uint64_t c1 = rdtscp();
auto     t1 = steady_clock::now();

double cycles_per_ns = (c1 - c0) / nanoseconds(t1 - t0).count();
// example result: 3.2  (CPU runs at ~3.2 GHz)
```

The result is cached in a static variable. C++11 magic statics guarantee thread-safe initialization, and calibration runs exactly once no matter how many threads call `getCyclesPerNs()`:

```cpp
inline double getCyclesPerNs() {
    static double cpns = calibrateCyclesPerNs();
    return cpns;
}
```

Converting cycles to nanoseconds anywhere in the codebase is then a single division:

```cpp
double ns = static_cast<double>(cycles) / getCyclesPerNs();
```

### RAII scope timer

The most common use case is measuring a block of code. `RDTSCScopeTimer` does this with zero ceremony:

```cpp
{
    RDTSCScopeTimer timer("OrderBookUpdate");

    update_order_book(quote);

}  // ← destructor fires here, prints elapsed time automatically
```

```
[RDTSC] OrderBookUpdate : 187 cycles (58.4 ns)
```

The destructor fires when the object goes out of scope — even if an exception is thrown. You cannot forget to stop the timer, because stopping it is not a call you make: it is a consequence of scope exit.

---

## Putting It Together

With these three headers in place, the system has everything it needs to start building real components:

- **A message shape** that is exactly 64 bytes, fits in one cache line, uses no heap memory, and is validated at compile time.
- **A transport** that moves those messages between processes with no kernel involvement on the hot path — just a `memcpy` into shared memory and an atomic counter update.
- **A ruler** that measures code paths at nanosecond resolution, with overhead so low it doesn't distort the measurement.

Phase 2 builds the Matching Engine on this foundation: an order book that receives `Quote` messages from the IPC channel, maintains sorted price levels, and generates `Trade` messages when orders cross. Every design decision in Phase 2 will be measured with the timer from Phase 1, and every measurement will tell us something true.

---

*Phase 1 complete. Foundation verified.*