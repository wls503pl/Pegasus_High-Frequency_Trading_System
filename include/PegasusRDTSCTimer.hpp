#pragma once
#include <cstdint>
#include <string>
#include <iostream>
#include <chrono>
#include <thread>

/**
 * @namespace pegasus
 * @brief High-precision timing utilities using the CPU's Time Stamp Counter (TSC).
 *
 * In High-Frequency Trading (HFT), measuring latency in nanoseconds is critical.
 * This header provides tools to read the TSC and convert cycles to nanoseconds
 * with minimal overhead and maximum accuracy, accounting for CPU out-of-order
 * execution effects.
 */
namespace pegasus {

/**
 * @brief Executes the CPUID instruction to serialize instruction execution.
 *
 * CPUID acts as a full memory and instruction barrier, ensuring that all
 * instructions before it complete before any instructions after it begin.
 * This is crucial for accurate TSC readings, especially when measuring
 * very short code paths, to prevent CPU out-of-order execution from distorting
 * timing measurements.
 */
inline void cpuid() {
    __asm__ __volatile__("cpuid" : : : "eax", "ebx", "ecx", "edx");
}

/**
 * @brief Reads the CPU's Time Stamp Counter (TSC) with additional serialization.
 *
 * Uses the 'rdtscp' assembly instruction, which is a serializing instruction.
 * This means it waits for all previous instructions to complete before reading
 * the TSC, providing a more accurate measurement point than 'rdtsc' alone.
 * The 'lfence' instruction is also used as an additional memory barrier to
 * prevent reordering by the CPU, ensuring precise timing for critical sections.
 *
 * @return uint64_t Current CPU cycles.
 */
inline uint64_t rdtscp() {
    uint32_t lo, hi;
    // lfence acts as a load barrier, ensuring all previous loads are complete.
    // It also prevents subsequent instructions from executing until prior loads are finished.
    __asm__ __volatile__("lfence" : : : "memory");
    // rdtscp reads the TSC and also writes the processor ID to ECX (not used here).
    // It is a serializing instruction, meaning it waits for all prior instructions
    // to complete before reading the TSC.
    __asm__ __volatile__("rdtscp" : "=a"(lo), "=d"(hi) : : "ecx");
    // lfence after rdtscp ensures that no subsequent instructions are reordered
    // before the rdtscp instruction completes, further enhancing measurement accuracy.
    __asm__ __volatile__("lfence" : : : "memory");
    return (static_cast<uint64_t>(hi) << 32) | lo;
}

/**
 * @brief Calibrates the CPU frequency to determine cycles per nanosecond.
 *
 * This function measures the number of TSC cycles over a known wall-clock
 * duration (using std::chrono) to calculate the conversion factor.
 * It's crucial for converting raw cycle counts into meaningful time units.
 * Calibration is performed once at startup to minimize overhead during runtime.
 *
 * @return double The number of CPU cycles per one nanosecond.
 */
inline double calibrateCyclesPerNs() {
    using namespace std::chrono;
    constexpr int SLEEP_MS = 200; // Calibration window: 200 milliseconds

    // Ensure CPU is not in a power-saving state during calibration.
    // cpuid() acts as a serialization barrier before starting the measurement.
    cpuid();
    uint64_t c0 = rdtscp();
    auto     t0 = steady_clock::now();

    // Sleep for a short duration to get a reliable wall-clock measurement.
    std::this_thread::sleep_for(milliseconds(SLEEP_MS));

    // cpuid() acts as a serialization barrier after the measurement.
    cpuid();
    uint64_t c1 = rdtscp();
    auto     t1 = steady_clock::now();

    double ns = static_cast<double>(
        duration_cast<nanoseconds>(t1 - t0).count());
    return static_cast<double>(c1 - c0) / ns;
}

/**
 * @brief Retrieves the calibrated cycles-per-nanosecond factor.
 *
 * Uses a thread-safe static local variable (C++11 "Magic Statics") to
 * ensure calibration happens exactly once upon the first call in a thread-safe manner.
 * This avoids repeated calibration overhead.
 *
 * @return double The cached cycles-per-nanosecond factor.
 */
inline double getCyclesPerNs() {
    static double cpns = calibrateCyclesPerNs();
    return cpns;
}

/**
 * @brief Converts raw CPU cycles to nanoseconds.
 * @param cycles The number of cycles measured via rdtscp().
 * @return double The equivalent duration in nanoseconds.
 */
inline double cyclesToNs(uint64_t cycles) {
    return static_cast<double>(cycles) / getCyclesPerNs();
}

/**
 * @class RDTSCScopeTimer
 * @brief RAII-based (Resource Acquisition Is Initialization) scope timer for
 *        nanosecond-level profiling.
 *
 * Automatically measures the duration of a code block and prints the
 * result in both CPU cycles and nanoseconds upon destruction. This is useful
 * for quickly profiling critical sections of code.
 *
 * Example:
 * @code
 * {
 *     RDTSCScopeTimer timer("CriticalPath");
 *     // ... high-performance code ...
 * } // Timer automatically stops and prints duration here
 * @endcode
 */
class RDTSCScopeTimer {
public:
    /**
     * @brief Constructs the timer and starts measurement.
     * @param label The name of the code block being measured, for logging purposes.
     */
    explicit RDTSCScopeTimer(const std::string& label)
        : label_(label), start_(rdtscp()) {}

    /**
     * @brief Destructor stops the timer and logs the elapsed time.
     * Ensures that timing results are always reported when the scope is exited.
     */
    ~RDTSCScopeTimer() {
        uint64_t cycles  = rdtscp() - start_;
        double   ns      = cyclesToNs(cycles);
        std::cout << "[RDTSC] " << label_
                  << " : " << cycles << " cycles"
                  << " (" << ns << " ns)\n";
    }

private:
    std::string label_; // Label for the timed code block
    uint64_t    start_; // Starting TSC value
};

} // namespace pegasus
