#pragma once

#include <atomic>
#include <chrono>
#include <iostream>
#include <iomanip>

namespace codetopo {

// Accumulator for a single profiling phase.
struct PhaseTimer {
    std::atomic<int64_t> total_us{0};
    std::atomic<int64_t> count{0};

    void add(int64_t us) {
        total_us.fetch_add(us, std::memory_order_relaxed);
        count.fetch_add(1, std::memory_order_relaxed);
    }
};

// RAII guard that times a scope and adds to a PhaseTimer.
struct ScopedPhase {
    PhaseTimer& phase;
    std::chrono::steady_clock::time_point start;
    bool enabled;

    explicit ScopedPhase(PhaseTimer& p, bool en = true)
        : phase(p), enabled(en) {
        if (enabled) start = std::chrono::steady_clock::now();
    }
    ~ScopedPhase() {
        if (enabled) {
            auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - start).count();
            phase.add(elapsed);
        }
    }
    ScopedPhase(const ScopedPhase&) = delete;
    ScopedPhase& operator=(const ScopedPhase&) = delete;
};

// Per-index-run profiler with named phase accumulators.
struct Profiler {
    bool enabled = false;

    PhaseTimer arena_lease;
    PhaseTimer file_read;
    PhaseTimer hash;
    PhaseTimer parse;
    PhaseTimer extract;
    PhaseTimer contention;
    PhaseTimer persist;
    PhaseTimer persist_wait;
    PhaseTimer flush;
    PhaseTimer idx_read;
    PhaseTimer resolve_refs;
    PhaseTimer idx_write;
    PhaseTimer fts_rebuild;
    PhaseTimer metadata;
    PhaseTimer wal_ckpt;

    void print_report(int64_t total_us, int total_files, int thread_count) const {
        if (!enabled) return;

        auto print_phase = [&](const char* name, const PhaseTimer& p) {
            double ms = p.total_us.load(std::memory_order_relaxed) / 1000.0;
            int64_t n = p.count.load(std::memory_order_relaxed);
            double avg_ms = n > 0 ? ms / n : 0;
            double pct = total_us > 0 ? (p.total_us.load(std::memory_order_relaxed) * 100.0 / total_us) : 0;
            std::cerr << "  " << std::left << std::setw(16) << name
                      << std::right << std::setw(10) << std::fixed << std::setprecision(1) << ms << " ms"
                      << std::setw(10) << n << " calls"
                      << std::setw(10) << std::setprecision(2) << avg_ms << " ms/call"
                      << std::setw(8) << std::setprecision(1) << pct << "%"
                      << "\n";
        };

        double total_ms = total_us / 1000.0;
        double files_per_sec = total_ms > 0 ? (total_files * 1000.0 / total_ms) : 0;

        std::cerr << "\n=== Profile Report ===\n";
        std::cerr << "Total: " << std::fixed << std::setprecision(1) << total_ms << " ms"
                  << " | " << total_files << " files"
                  << " | " << std::setprecision(0) << files_per_sec << " files/s"
                  << " | " << thread_count << " threads\n\n";

        print_phase("arena_lease", arena_lease);
        print_phase("file_read", file_read);
        print_phase("hash", hash);
        print_phase("parse", parse);
        print_phase("extract", extract);
        print_phase("contention", contention);
        print_phase("persist", persist);
        print_phase("persist_wait", persist_wait);
        print_phase("flush", flush);
        print_phase("idx_read", idx_read);
        print_phase("resolve_refs", resolve_refs);
        print_phase("idx_write", idx_write);
        print_phase("fts_rebuild", fts_rebuild);
        print_phase("metadata", metadata);
        print_phase("wal_ckpt", wal_ckpt);
        std::cerr << "======================\n";
    }
};

} // namespace codetopo
