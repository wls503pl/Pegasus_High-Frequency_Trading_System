# Pegasus Phase 1 — Test Case Design

> *What we test, and why we test it that way.*
> *This document covers design rationale only. Expected results are recorded separately in `verify_results/`.*

---

## The Testing Philosophy

Phase 1 is pure infrastructure — no business logic, no user interaction. Every line of code exists to make a guarantee:

- "This struct is exactly 64 bytes."
- "This message arrived at the consumer intact."
- "This timer reads faster than the thing it's measuring."

When infrastructure breaks, it breaks silently. A misaligned struct doesn't crash — it just causes the consumer to read garbage as if it were valid market data. A counter with wrong memory ordering compiles cleanly and fails once every ten thousand messages under load.

This shapes the testing approach: **we don't test behavior, we test contracts**. Each test is a formal check that a specific guarantee holds. The moment a guarantee is violated, the test tells you exactly which one and where.

---

## `test_phase1_datatypes` — The Data Contract

### What this test covers

The core question: *do the structs actually look the way the design document says they do?*

This sounds trivial. It isn't. The C++ compiler is free to insert implicit padding bytes between fields, reorder nothing (but the developer might), and widen enum types beyond what was intended. Any of these silently break the memory layout that the IPC layer depends on.

### Case 1 — Size and alignment

We verify `sizeof` and `alignof` for `Order`, `Trade`, and `Quote`.

The `static_assert` in the header already catches this at compile time — if it compiles, the sizes are right. The runtime check here is redundant by design: it makes the contract visible in the test output, so a reader scanning results can confirm the guarantee without reading header code.

### Case 2 & 3 & 4 — Field offsets for Order, Trade, Quote

We check `offsetof()` for every field in every struct.

This is the most important group of tests. Two processes share memory by agreeing on a layout. If the producer's compiler places `price` at byte 16 and the consumer's compiler places it at byte 24, no error is raised — but every price read is wrong. `offsetof()` catches this before it reaches production.

The offsets are checked individually rather than as a block so that a failure points directly to the specific field that moved.

### Case 5 — FixedString behavior

`FixedString<N>` has three behavioral contracts that are easy to accidentally break:

- **Zero initialization on construction.** If the buffer isn't zeroed, a freshly created symbol field contains stack garbage. Any string comparison against it will behave randomly.
- **Null-termination after `set()`**, including when the input is exactly `N-1` characters long. The off-by-one boundary is where bugs live.
- **Equality comparison correctness.** Both the `FixedString == FixedString` and `FixedString == const char*` operators are tested, because they delegate to `strcmp` and any mistake there produces wrong order routing.

### Case 6 — Enum underlying values

`Side` and `OrdStatus` are declared with `: char` as their underlying type. We verify that `Side::BUY` is literally the character `'B'`, not an integer that happens to equal 66.

Why does this matter? These fields are stored inside the 64-byte struct and transmitted through shared memory. If the underlying type silently became `int`, each enum field would expand from 1 byte to 4 bytes, the struct would exceed 64 bytes, and the `static_assert` would catch it — but only at compile time, not if someone reads a binary blob from an older segment. The char-value test confirms the semantic contract independently of the size check.

### Case 7 — Order instantiation and stack alignment

We construct a real `Order` on the stack, fill every field, and read them back.

This is a sanity check that the struct is usable as an aggregate — no hidden constructor issues, no field access problems. We also verify that `alignas(64)` produces a stack-allocated instance whose address is divisible by 64. `alignas` on a stack variable is only a request to the compiler; this test confirms the compiler honored it.

---

## `test_phase1_ipc` — The Transport Contract

### What this test covers

The core question: *does a message written by the producer arrive at the consumer byte-for-byte identical, under all conditions?*

The IPC layer has four distinct failure modes, each tested separately:

1. The segment layout is computed wrong → wrong slot addresses
2. The protocol header is wrong → consumer rejects valid segments or accepts invalid ones
3. Data is corrupted in transit → values don't match
4. Ring buffer index arithmetic wraps incorrectly → wrong slot is read after wrap-around

All tests run in a single process. We use real `mmap()` files in `/tmp/` — not mocked memory — because the actual `MmapRegion` code path (including `PROT_READ` enforcement and `O_TRUNC` semantics) is part of what we're validating.

### Case 1 — Constants and REGION_SIZE

Before writing a single message, we verify that `REGION_SIZE` matches the formula `64 + 64 + sizeof(T) * Capacity`.

This matters because `ShmProducer` and `ShmConsumer` must agree on the total segment size to map the correct number of bytes. If either side computes a different size, one of them maps too little memory and the ring buffer access goes out of bounds. Catching this before any message is sent isolates the arithmetic from the messaging logic.

### Case 2 — Produce and consume

We publish four `Quote` messages and consume them one by one, verifying `bid_price`, `sequence`, and `symbol` on each.

Three fields are checked rather than one to cover different data widths: `bid_price` is a 64-bit integer, `sequence` is a monotonic counter (the gap-detection field), and `symbol` is a `FixedString` — a struct-within-a-struct. If `memcpy` across the shared mapping truncates or misaligns any of these, the mismatch will appear here.

We also attempt one extra `consume()` after the buffer is drained, to verify that the "nothing new" path returns correctly rather than re-reading the last message.

### Case 3 — Protocol header validation

After the producer writes the header, we open the segment read-only and inspect the header fields directly.

The magic number, version, and capacity checks exist in `ShmConsumer`'s constructor — but testing them here means: (a) the producer actually wrote them, and (b) the values are correct, not just present. We also simulate a capacity mismatch by checking that the code path that detects `capacity != 16` raises a `std::runtime_error`. This is the guard against opening a stale segment from a previous run.

### Case 4 — Ring buffer wrap-around

We fill the ring buffer to capacity, verify that the next publish returns false (buffer full), consume four slots, then publish four more messages that must wrap around to slots 0–3.

This tests the index arithmetic `write & (Capacity - 1)` at the boundary where it matters. A naive implementation using `%` with a non-power-of-two capacity, or an off-by-one in the full-detection check, would produce a wrong slot index or allow an overwrite here. We then consume the remaining messages in order and verify their `sequence` values to confirm the wrap-around read is also correct.

---

## `test_phase1_timer` — The Measurement Contract

### What this test covers

The core question: *is the timer accurate enough to be trusted, and cheap enough to not distort what it measures?*

A timer that introduces more latency than the code path being measured is useless. A timer that reads the wrong time due to CPU out-of-order execution is worse than useless — it produces confidently wrong numbers. These tests validate both properties.

### Case 1 — rdtscp() monotonicity

We call `rdtscp()` twice with a small amount of work in between and assert the second value is strictly greater than the first.

This is the most basic sanity check: the TSC must be monotonically increasing. If it isn't — which can happen on systems with unsynchronized per-core TSCs or certain virtualization configurations — every latency measurement in the system is meaningless. Catching this at startup is critical.

### Case 2 — getCyclesPerNs() range

We call `getCyclesPerNs()` and assert the result falls between 0.5 and 10.0.

This bounds-check catches two failure modes: a calibration that produced near-zero (sleep was too short, wall clock returned garbage) and a value above 10 (TSC is not running at the CPU's reference frequency, which happens on some VMs). The 200ms sleep window is long enough relative to typical OS scheduler jitter that the calibration should land close to the true CPU frequency.

### Case 3 — cyclesToNs() accuracy against a known interval

We wrap a `sleep_for(10ms)` in two `rdtscp()` calls and convert the cycle delta to nanoseconds.

The 20% error tolerance is intentional. OS sleep is not precise — the thread may wake late. The purpose of this test is not to verify sleep precision, but to verify that `cyclesToNs()` produces a number in the correct order of magnitude. A factor-of-ten error (e.g., 1ms or 100ms) would indicate a calibration failure.

### Case 4 — RDTSCScopeTimer RAII

We instantiate `RDTSCScopeTimer` in a block scope, do a small amount of work, and let the scope exit.

The test is deliberately simple: the destructor must fire at scope exit and produce output. This confirms the RAII pattern is working — the timer stops automatically without a manual call. In production, any code path that returns early or throws an exception still gets its timing recorded.

### Case 5 — Calibration stability

We call `calibrateCyclesPerNs()` twice in sequence and compare the results.

`getCyclesPerNs()` caches its result via a static variable, so this test calls the calibration function directly to get two independent measurements. If the two runs differ by more than 5%, it indicates CPU frequency instability (aggressive power management, thermal throttling, or a shared VM environment). This isn't a bug in the code — it's a signal that the hardware environment is unsuitable for nanosecond-level measurements.

### Case 6 — rdtscp() overhead estimation

We call `rdtscp()` back-to-back 100 times, record the minimum cycle delta, and convert it to nanoseconds.

The minimum across 100 samples is the best estimate of the true hardware cost of the instruction pair, filtering out OS preemptions and cache misses that inflate individual measurements. This number needs to be significantly smaller than the code paths we intend to measure — if `rdtscp()` itself costs 300 cycles and our target path costs 200 cycles, the measurement is dominated by measurement overhead.

---

## Why These Tests Are Sufficient for Phase 1

Phase 1 has no concurrent writers, no network, and no business logic. The failure modes are:

- Wrong layout → caught by offset and size tests
- Wrong values in transit → caught by produce/consume value comparison
- Wrong index arithmetic → caught by wrap-around test
- Timer not trustworthy → caught by monotonicity, range, accuracy, and overhead tests

What these tests deliberately do *not* cover:

- **Multi-process concurrency.** The one-counter protocol's correctness under true concurrent access (producer and consumer in separate processes running simultaneously) requires a different class of test — stress testing with timing analysis — which belongs in a later phase when the full pipeline exists.
- **Signal handling and crash recovery.** The `O_TRUNC` and magic-check behavior under abnormal termination is partially covered by the header validation test, but full crash-recovery testing requires process lifecycle control beyond unit test scope.

These omissions are known and intentional. Phase 1 tests prove the contracts. Integration tests prove the system.
