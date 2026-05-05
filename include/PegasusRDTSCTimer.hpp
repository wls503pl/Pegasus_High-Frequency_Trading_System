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
 * with minimal overhead.
 */
namespace pegasus {

/**
 * @brief Reads the CPU's Time Stamp Counter (TSC).
 * 
 * Uses the 'rdtsc' assembly instruction to get the current number of CPU cycles
 * since reset. This is the fastest way to get high-resolution timing data.
 * 
 * @return uint64_t Current CPU cycles.
 */
inline uint64_t rdtsc() {
    /**
     * __asm__: 告诉编译器这里要插入一段原始的汇编代码。
     * __volatile__: 一个关键的限定符。告知编译器：“不要动（优化）这段代码！”。
     如果没有它，编译器可能会认为这段代码有副作用而将其优化掉，或者为了性能将其移动到循环外面,
     这会导致测量的计时区间完全错误。
     * "rdtsc": 这是指令助记符（Read Time-Stamp Counter）。它会读取 CPU 自上电以来的时钟周期数。
     该指令将 64 位的结果拆分成两部分：高 32 位存入 EDX 寄存器，低 32 位存入 EAX 寄存器。
     * "=a"(lo): 这是输出约束（Output Operand）。
        - a 代表 EAX 寄存器.
        - = 表示该操作数是只写的。
        - 这句话的意思是：“指令执行完后，把 EAX 寄存器的值赋给 C++ 变量 lo。”
     * "=d"(hi): 同上。
        - d 代表 EDX 寄存器。
        - 这句话的意思是：“把 EDX 寄存器的值赋给 C++ 变量 hi。”
     * :,:: 这里跳过了输入操作数（Input Operands），因为 rdtsc 不需要输入。
     * "memory": 这是Broker列表（Clobber List），也是最体现 HFT 功底的地方。
        - 它充当了内存屏障（Memory Barrier）。它告诉编译器：“这段汇编可能会读取或修改任何内存位置”。
        - 核心作用：强制编译器在执行 rdtsc 之前，必须完成所有之前的内存读写操作；并且在 rdtsc 执行完之前，
        不能开始任何之后的内存操作。
        - 为什么重要？：如果没有这个屏障，编译器为了优化可能会把你想测量的代码挪到 rdtsc 之外，
        导致你测出来的延迟是 0 或者负数(编译器过早优化计时)
     */
    uint32_t lo, hi;
    __asm__ __volatile__(
        "rdtsc"
        : "=a"(lo), "=d"(hi)
        :: "memory" // Memory barrier to prevent compiler reordering
    );
    return (static_cast<uint64_t>(hi) << 32) | lo;
}

/**
 * @brief Calibrates the CPU frequency to determine cycles per nanosecond.
 * 
 * This function measures the number of TSC cycles over a known wall-clock 
 * duration (using std::chrono) to calculate the conversion factor.
 * 
 * @return double The number of CPU cycles per one nanosecond.
 */
inline double calibrateCyclesPerNs() {
    using namespace std::chrono;
    constexpr int SLEEP_MS = 200; // Calibration window

    uint64_t c0 = rdtsc();
    auto     t0 = steady_clock::now();

    std::this_thread::sleep_for(milliseconds(SLEEP_MS));

    uint64_t c1 = rdtsc();
    auto     t1 = steady_clock::now();

    double ns = static_cast<double>(
        duration_cast<nanoseconds>(t1 - t0).count());
    return static_cast<double>(c1 - c0) / ns;
}

/**
 * @brief Retrieves the calibrated cycles-per-nanosecond factor.
 * 
 * Uses a thread-safe static local variable (C++11 "Magic Statics") to 
 * ensure calibration happens exactly once upon the first call.
 * 
 * @return double The cached cycles-per-nanosecond factor.
 */
inline double getCyclesPerNs() {
    static double cpns = calibrateCyclesPerNs();
    return cpns;
}

/**
 * @brief Converts raw CPU cycles to nanoseconds.
 * @param cycles The number of cycles measured via rdtsc().
 * @return double The equivalent duration in nanoseconds.
 */
inline double cyclesToNs(uint64_t cycles) {
    return static_cast<double>(cycles) / getCyclesPerNs();
}

/**
 * @class RDTSCScopeTimer
 * @brief RAII-based(Resource Acquisition Is Initialization 资源获取即初始化)
   scope timer for nanosecond-level profiling.
 * 
 * Automatically measures the duration of a code block and prints the 
 * result in both CPU cycles and nanoseconds upon destruction.
 * 
 * Example:
 * {
 *     RDTSCScopeTimer timer("CriticalPath");
 *     // ... high-performance code ...
 * }
 */
class RDTSCScopeTimer {
public:
    /**
     * @brief Starts the timer with a specific label.
     * @param label The name of the code block being measured.
     */
    explicit RDTSCScopeTimer(const std::string& label)
        : label_(label), start_(rdtsc()) {}

    /**
     * @brief Stops the timer and logs the elapsed time.
     */
    ~RDTSCScopeTimer() {
        uint64_t cycles  = rdtsc() - start_;
        double   ns      = cyclesToNs(cycles);
        std::cout << "[RDTSC] " << label_
                  << " : " << cycles << " cycles"
                  << " (" << ns << " ns)\n";
    }

private:
    std::string label_;
    uint64_t    start_;
};

} // namespace pegasus
