================================================================================
PROFILING TOOLS GUIDE FOR HFT C++ - COMPLETE REFERENCE
================================================================================

OVERVIEW:
---------
This directory contains comprehensive guides for profiling and optimizing
High-Frequency Trading (HFT) C++ applications. Each guide covers theory,
practical examples, workflows, and HFT-specific optimization strategies.

TARGET AUDIENCE:
- HFT developers and engineers
- Performance engineers
- System architects working on low-latency systems
- Anyone optimizing C++ for microsecond-level latency

CONTENTS:
---------

01_perf_cpu_profiling.txt
    Tool: perf (Linux Performance Analyzer)
    Focus: CPU profiling, cache analysis, instruction-level optimization
    Use When:
        - Identifying CPU hotspots
        - Analyzing cache efficiency
        - Finding branch mispredictions
        - Profiling with hardware counters
    Key Topics:
        - perf record/report
        - Cache miss analysis
        - Flamegraph generation
        - IPC (Instructions Per Cycle) optimization

02_valgrind_memory_profiling.txt
    Tool: Valgrind (Memcheck, Massif, Cachegrind, Callgrind)
    Focus: Memory errors, heap profiling, cache simulation
    Use When:
        - Detecting memory leaks
        - Finding buffer overflows
        - Analyzing heap allocation patterns
        - Profiling cache behavior
    Key Topics:
        - Memcheck for error detection
        - Massif for heap profiling
        - Cachegrind for cache analysis
        - Callgrind for call-graph profiling

03_gprof_cpu_profiling.txt
    Tool: gprof (GNU Profiler)
    Focus: Function-level profiling, call graphs
    Use When:
        - Quick function-level profiling
        - Understanding call relationships
        - Finding expensive function calls
        - Portable profiling across systems
    Key Topics:
        - Flat profiles
        - Call graphs
        - Function call frequency
        - Time-per-call analysis

04_memory_advanced_profiling.txt
    Tools: Heaptrack, AddressSanitizer, perf mem, NUMA tools
    Focus: Advanced memory optimization for HFT
    Use When:
        - Optimizing memory pools
        - Detecting memory errors with low overhead (ASan)
        - Analyzing memory bandwidth
        - NUMA-aware optimization
    Key Topics:
        - Memory pool design
        - ASan for fast error detection
        - NUMA profiling and optimization
        - Memory bandwidth analysis
        - Huge pages configuration

05_thread_concurrency_profiling.txt
    Tools: Helgrind, ThreadSanitizer, mutrace, perf
    Focus: Multi-threading, race conditions, lock contention
    Use When:
        - Debugging race conditions
        - Analyzing lock contention
        - Detecting false sharing
        - Optimizing thread synchronization
    Key Topics:
        - Race detection with TSan
        - Lock-free programming
        - False sharing prevention
        - CPU pinning and affinity
        - Context switch analysis

06_network_profiling.txt
    Tools: tcpdump, iperf3, ss, ethtool, perf, bpftrace
    Focus: Network latency, throughput, kernel tuning
    Use When:
        - Analyzing network latency
        - Debugging packet loss
        - Tuning NIC parameters
        - Measuring jitter
    Key Topics:
        - Packet capture and analysis
        - Network stack profiling
        - NIC tuning (interrupt coalescing, ring buffers)
        - Kernel network tuning
        - Timestamping for latency measurement
        - Kernel bypass overview (DPDK, OpenOnload)

07_latency_profiling.txt
    Tools: RDTSC, clock_gettime, perf, ftrace
    Focus: Microsecond/nanosecond latency measurement
    Use When:
        - Measuring sub-microsecond latencies
        - Tracking tail latencies (P99, P99.9)
        - Identifying latency sources
        - Optimizing critical paths
    Key Topics:
        - RDTSC cycle-accurate timing
        - Nanosecond precision with clock_gettime
        - Latency statistics and histograms
        - Tail latency optimization
        - System-wide latency sources
        - Complete HFT latency profiling framework

08_os_system_profiling.txt
    Tools: top/htop, vmstat, iostat, mpstat, pidstat, sar, strace
    Focus: Operating system behavior, resource utilization
    Use When:
        - Monitoring system resources
        - Analyzing context switches
        - Identifying syscall overhead
        - System tuning and configuration
    Key Topics:
        - Process monitoring
        - Virtual memory statistics
        - Per-CPU statistics
        - Context switch analysis
        - Page fault monitoring
        - System call tracing
        - OS tuning for HFT (CPU isolation, huge pages, etc.)

================================================================================
QUICK START GUIDE
================================================================================

STEP 1: IDENTIFY YOUR BOTTLENECK
---------------------------------

Ask yourself:
- Is my code CPU-bound? → Start with 01_perf_cpu_profiling.txt
- Do I have memory issues/leaks? → Start with 02_valgrind_memory_profiling.txt
- Are threads causing problems? → Start with 05_thread_concurrency_profiling.txt
- Is network latency high? → Start with 06_network_profiling.txt
- Do I need to measure precise latency? → Start with 07_latency_profiling.txt
- Is the OS interfering? → Start with 08_os_system_profiling.txt

STEP 2: PROFILE WITH APPROPRIATE TOOL
--------------------------------------

For each profiling session:
1. Establish baseline metrics
2. Use the appropriate tool from the guides
3. Collect data with realistic workload
4. Analyze results
5. Identify optimization opportunities

STEP 3: OPTIMIZE
----------------

Focus on:
- Highest impact items first (80/20 rule)
- Tail latencies (P99, P99.9) not just averages
- Eliminating allocations in hot paths
- Improving cache locality
- Reducing syscalls and context switches

STEP 4: VERIFY
--------------

After optimization:
1. Re-profile with same workload
2. Verify improvement in key metrics
3. Check for regressions in other areas
4. Test under stress/extended runs
5. Monitor in production

================================================================================
TOOL SELECTION MATRIX
================================================================================

PROFILING GOAL               | PRIMARY TOOL        | SECONDARY TOOLS
-----------------------------|---------------------|-------------------
CPU Hotspots                 | perf               | gprof, VTune
Cache Optimization           | perf, cachegrind   | perf mem
Memory Leaks                 | Valgrind Memcheck  | Heaptrack, ASan
Heap Profiling               | Heaptrack          | Massif
Memory Errors                | AddressSanitizer   | Valgrind Memcheck
Race Conditions              | ThreadSanitizer    | Helgrind, DRD
Lock Contention              | mutrace            | perf, TSan
False Sharing                | perf c2c           | TSan
Network Latency              | tcpdump, perf      | iperf3, bpftrace
Latency Measurement          | RDTSC              | clock_gettime, perf
Context Switches             | perf, pidstat      | vmstat
System Resources             | htop, mpstat       | sar, top
Syscall Overhead             | strace, perf       | ftrace, bpftrace

================================================================================
OVERHEAD COMPARISON
================================================================================

Tool                  | Overhead      | Use Case
----------------------|---------------|----------------------------------------
RDTSC                 | ~30 cycles    | Ultra-low latency measurement
clock_gettime         | ~50 ns        | Nanosecond timing
perf (sampling)       | 1-5%          | Production profiling
gprof                 | 10-30%        | Development profiling
Heaptrack             | 5-10%         | Heap profiling
AddressSanitizer      | 2-3x          | Memory error detection (dev/test)
ThreadSanitizer       | 5-15x         | Race detection (dev/test)
Valgrind Memcheck     | 10-50x        | Memory debugging (dev only)
Valgrind Massif       | 20x           | Heap profiling (dev only)
Valgrind Cachegrind   | 20-100x       | Cache simulation (dev only)
Valgrind Callgrind    | 10-50x        | Call-graph profiling (dev only)
strace                | 10-100x       | Syscall debugging (dev only)

RECOMMENDATION:
- Use low-overhead tools (perf, heaptrack, ASan, TSan) in development
- Use very low overhead tools (perf sampling) in production
- Never use Valgrind or strace in production
- Use RDTSC for micro-benchmarking

================================================================================
HFT-SPECIFIC OPTIMIZATION CHECKLIST
================================================================================

CPU OPTIMIZATION:
☐ Profile with perf to identify hotspots
☐ Achieve IPC > 1.5 (ideally > 2.0)
☐ Cache miss rate < 3% for L1, < 10% for LLC
☐ Branch miss rate < 5%
☐ Use SIMD instructions (AVX2/AVX-512) where applicable
☐ Minimize unpredictable branches

MEMORY OPTIMIZATION:
☐ Zero allocations in hot path (use memory pools)
☐ All memory locked (mlockall)
☐ Zero page faults during trading
☐ Cache-line aligned critical structures (64 bytes)
☐ NUMA-aware memory allocation
☐ Consider huge pages for large allocations

THREADING OPTIMIZATION:
☐ Lock-free data structures where possible
☐ No false sharing (verify with perf c2c)
☐ Threads pinned to dedicated cores
☐ Minimal context switches (<100/sec)
☐ Real-time scheduling (SCHED_FIFO)

NETWORK OPTIMIZATION:
☐ Interrupt coalescing disabled
☐ NIC ring buffers sized appropriately
☐ Network IRQs pinned to dedicated cores
☐ UDP for market data (multicast if possible)
☐ Kernel tuned (large socket buffers, etc.)
☐ Consider kernel bypass (DPDK/OpenOnload)

LATENCY OPTIMIZATION:
☐ Measure tail latencies (P99, P99.9, P99.99)
☐ End-to-end latency < target (typically <100μs)
☐ Jitter minimal (<1μs standard deviation)
☐ No outliers > 10x median

OS CONFIGURATION:
☐ CPU frequency governor: performance
☐ Turbo boost disabled (for consistency)
☐ C-states disabled beyond C1
☐ CPUs isolated (isolcpus kernel parameter)
☐ Swap disabled entirely
☐ Swappiness = 0
☐ Unnecessary services disabled
☐ IRQ affinity configured

MONITORING:
☐ Continuous latency tracking
☐ Alert on tail latency degradation
☐ Monitor context switches
☐ Monitor page faults
☐ Track memory usage over time
☐ Historical analysis with sar

================================================================================
COMMON PITFALLS & SOLUTIONS
================================================================================

PITFALL: Profiling with optimizations disabled (-O0)
SOLUTION: Always profile with production optimization level (-O2 or -O3)

PITFALL: Measuring cold code (no warmup)
SOLUTION: Always warmup before measurement (run 1000+ iterations)

PITFALL: Profiling on different hardware than production
SOLUTION: Profile on production-like hardware

PITFALL: Focusing only on average latency
SOLUTION: Focus on tail latencies (P99, P99.9, P99.99)

PITFALL: Optimizing code that doesn't matter
SOLUTION: Profile first, optimize second (data-driven optimization)

PITFALL: Using high-overhead tools in production
SOLUTION: Use perf (low overhead) in production, Valgrind only in dev

PITFALL: Not testing under realistic load
SOLUTION: Profile with production-like message rates and volumes

PITFALL: Ignoring OS interference
SOLUTION: Tune OS (CPU isolation, disable frequency scaling, etc.)

PITFALL: Allocating in hot path
SOLUTION: Pre-allocate everything, use memory pools

PITFALL: Lock contention
SOLUTION: Use lock-free data structures or partition data

================================================================================
RECOMMENDED WORKFLOW
================================================================================

1. BASELINE MEASUREMENT
   - Measure current performance (latency percentiles, throughput)
   - Use perf stat for broad metrics
   - Use RDTSC/clock_gettime for precise latency

2. IDENTIFY BOTTLENECKS
   - CPU profiling with perf record/report
   - Memory profiling with heaptrack
   - Thread profiling with perf sched
   - Network profiling with tcpdump/perf

3. ANALYZE ROOT CAUSE
   - Drill down into hot functions
   - Check cache behavior
   - Verify lock-free operation
   - Analyze syscall overhead

4. OPTIMIZE
   - Address highest impact items first
   - Use techniques from relevant guides
   - One optimization at a time
   - Measure after each change

5. VERIFY & ITERATE
   - Re-measure with same workload
   - Verify improvement
   - Check for regressions
   - Repeat until targets met

6. STRESS TEST
   - Extended run (hours/days)
   - Peak load testing
   - Monitor for degradation
   - Verify tail latencies

7. PRODUCTION MONITORING
   - Continuous latency tracking
   - Alert on anomalies
   - Regular profiling sessions
   - Capacity planning

================================================================================
ADDITIONAL RESOURCES
================================================================================

TOOLS:
- perf: https://perf.wiki.kernel.org/
- Valgrind: https://valgrind.org/
- Intel VTune: https://software.intel.com/vtune
- DPDK: https://www.dpdk.org/
- OpenOnload: https://www.openonload.org/

BOOKS:
- "Systems Performance" by Brendan Gregg
- "BPF Performance Tools" by Brendan Gregg
- "The Art of Writing Efficient Programs" by Fedor G. Pikus

WEBSITES:
- Brendan Gregg's Blog: https://www.brendangregg.com/
- LWN.net (Linux kernel articles)
- Mechanical Sympathy (Martin Thompson's blog)

================================================================================
SUPPORT & CONTRIBUTION
================================================================================

These guides are living documents. As tools evolve and new techniques emerge,
updates will be needed.

For questions, corrections, or additions:
- Review the specific guide for your use case
- Cross-reference with tool documentation
- Test in safe environment before production
- Share findings with team

Remember: In HFT, every nanosecond counts. Profile, optimize, and verify
relentlessly. The market waits for no one.

================================================================================
QUICK REFERENCE COMMANDS
================================================================================

# CPU Profiling
perf record -g ./app && perf report
perf stat -e cache-misses,branch-misses ./app

# Memory Profiling
valgrind --tool=memcheck --leak-check=full ./app
heaptrack ./app && heaptrack_gui heaptrack.app.*.gz

# Thread Profiling
g++ -fsanitize=thread app.cpp && ./a.out
perf stat -e context-switches ./app

# Network Profiling
sudo tcpdump -i eth0 port 12345 -w capture.pcap
iperf3 -c server_ip -u -b 1G

# Latency Measurement
perf stat ./app
# Or use RDTSC in code (see 07_latency_profiling.txt)

# System Monitoring
htop
vmstat 1
mpstat -P ALL 1
pidstat -w -p PID 1

# System Tuning
sudo cpupower frequency-set -g performance
echo 0 | sudo tee /proc/sys/vm/swappiness
sudo ethtool -C eth0 rx-usecs 0

================================================================================
VERSION & CHANGELOG
================================================================================

Version: 1.0
Created: 2025
Target: HFT C++ Applications on Linux

This reference guide is optimized for:
- Linux kernel 5.x and 6.x
- GCC 9+ and Clang 10+
- x86_64 architecture with modern CPUs (Intel/AMD)
- High-frequency trading and ultra-low latency applications

Good luck with your optimization journey!
