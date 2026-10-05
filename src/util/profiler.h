#pragma once

#include <atomic>
#include <chrono>
#include <iostream>
#include <iomanip>

#if defined(__APPLE__) || defined(__linux__)
#include <sys/resource.h>
#endif
#include <fstream>

namespace codetopo {

inline double get_peak_rss_mb() {
#if defined(__APPLE__)
    struct rusage ru;
    if (getrusage(RUSAGE_SELF, &ru) == 0) {
        return ru.ru_maxrss / (1024.0 * 1024.0); // bytes on macOS
    }
#elif defined(__linux__)
    struct rusage ru;
    if (getrusage(RUSAGE_SELF, &ru) == 0) {
        return ru.ru_maxrss / 1024.0; // kilobytes on Linux
    }
#endif
    return 0.0;
}

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
    ScopedPhase(const ScopedPhase&) = delete("ScopedPhase times a specific lexical scope and cannot be copied");
    ScopedPhase& operator=(const ScopedPhase&) = delete("ScopedPhase times a specific lexical scope and cannot be copied");
};

// Per-index-run profiler with named phase accumulators.
struct Profiler {
    bool enabled = false;

    PhaseTimer scan;
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
    PhaseTimer pagerank;
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
        double peak_rss = get_peak_rss_mb();

        std::cerr << "\n=== Profile Report ===\n";
        std::cerr << "Total: " << std::fixed << std::setprecision(1) << total_ms << " ms"
                  << " | " << total_files << " files"
                  << " | " << std::setprecision(0) << files_per_sec << " files/s"
                  << " | " << thread_count << " threads"
                  << " | Peak RSS: " << std::fixed << std::setprecision(1) << peak_rss << " MB\n\n";

        print_phase("scan", scan);
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
        print_phase("pagerank", pagerank);
        print_phase("metadata", metadata);
        print_phase("wal_ckpt", wal_ckpt);
        std::cerr << "======================\n";
    }

    void write_json(const std::string& path, int64_t total_us, int total_files, int thread_count) const {
        if (path.empty()) return;
        std::ofstream out(path);
        if (!out.is_open()) return;

        double total_ms = total_us / 1000.0;
        double files_per_sec = total_ms > 0 ? (total_files * 1000.0 / total_ms) : 0;
        double peak_rss = get_peak_rss_mb();

        out << "{\n";
        out << "  \"total_ms\": " << std::fixed << std::setprecision(1) << total_ms << ",\n";
        out << "  \"total_files\": " << total_files << ",\n";
        out << "  \"files_per_sec\": " << std::fixed << std::setprecision(1) << files_per_sec << ",\n";
        out << "  \"thread_count\": " << thread_count << ",\n";
        out << "  \"peak_rss_mb\": " << std::fixed << std::setprecision(1) << peak_rss << ",\n";
        out << "  \"phases\": {\n";

        auto write_phase = [&](const char* name, const PhaseTimer& p, bool is_last = false) {
            double ms = p.total_us.load(std::memory_order_relaxed) / 1000.0;
            int64_t n = p.count.load(std::memory_order_relaxed);
            double avg_ms = n > 0 ? ms / n : 0;
            out << "    \"" << name << "\": {\"ms\": " << std::fixed << std::setprecision(1) << ms
                << ", \"calls\": " << n
                << ", \"avg_ms\": " << std::fixed << std::setprecision(3) << avg_ms << "}"
                << (is_last ? "" : ",") << "\n";
        };

        write_phase("scan", scan);
        write_phase("arena_lease", arena_lease);
        write_phase("file_read", file_read);
        write_phase("hash", hash);
        write_phase("parse", parse);
        write_phase("extract", extract);
        write_phase("contention", contention);
        write_phase("persist", persist);
        write_phase("persist_wait", persist_wait);
        write_phase("flush", flush);
        write_phase("idx_read", idx_read);
        write_phase("resolve_refs", resolve_refs);
        write_phase("idx_write", idx_write);
        write_phase("fts_rebuild", fts_rebuild);
        write_phase("pagerank", pagerank);
        write_phase("metadata", metadata);
        write_phase("wal_ckpt", wal_ckpt, true);

        out << "  }\n";
        out << "}\n";
    }
};

} // namespace codetopo
