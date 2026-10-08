#include <catch2/catch_test_macros.hpp>
#include "core/config.h"
#include "index/scanner.h"
#include "util/hash.h"
#include "util/json.h"
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

using namespace codetopo;
namespace fs = std::filesystem;

TEST_CASE("Scan-only benchmark records full source manifest and phase timings",
          "[.][scan-performance]") {
    const char* requested_root = std::getenv("CODETOPO_SCAN_ROOT");
    const char* requested_report = std::getenv("CODETOPO_SCAN_REPORT");
    REQUIRE(requested_root != nullptr);
    REQUIRE(requested_report != nullptr);
    Config config;
    config.repo_root = requested_root;
    std::map<std::string, double> phase_seconds;
    std::map<std::string, ScanProgress> phase_counts;
    ScanProgress last;
    auto started = std::chrono::steady_clock::now();
    auto phase_started = started;
    Scanner scanner(config, [&](const ScanProgress& progress) {
        auto now = std::chrono::steady_clock::now();
        if (last.phase != progress.phase) {
            if (!last.phase.empty()) {
                phase_seconds[last.phase] +=
                    std::chrono::duration<double>(now - phase_started).count();
                phase_counts[last.phase] = last;
            }
            phase_started = now;
            std::cerr << "benchmark phase=" << progress.phase << '\n' << std::flush;
        }
        last = progress;
    });
    auto files = scanner.scan();
    auto elapsed = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - started).count();
    phase_counts[last.phase] = last;
    std::sort(files.begin(), files.end(), [](const ScannedFile& left, const ScannedFile& right) {
        return left.relative_path < right.relative_path;
    });
    std::string manifest;
    std::map<std::string, int64_t> languages;
    for (const auto& file : files) {
        manifest += file.relative_path + '\t' + file.absolute_path.string() + '\t' +
            file.language + '\t' + std::to_string(file.size_bytes) + '\t' +
            std::to_string(file.mtime_ns) + '\n';
        ++languages[file.language];
    }
    fs::path report_path(requested_report);
    fs::create_directories(report_path.parent_path());
    std::ofstream manifest_file(report_path.string() + ".manifest", std::ios::binary);
    manifest_file << manifest;
    manifest_file.close();
    REQUIRE(manifest_file.good());

    JsonMutDoc report;
    auto* root = report.new_obj();
    report.set_root(root);
    yyjson_mut_obj_add_strcpy(report.doc, root, "root", fs::canonical(config.repo_root).string().c_str());
    yyjson_mut_obj_add_real(report.doc, root, "elapsed_seconds", elapsed);
    yyjson_mut_obj_add_uint(report.doc, root, "source_files", files.size());
    yyjson_mut_obj_add_strcpy(report.doc, root, "manifest_hash", hash_string(manifest).c_str());
    yyjson_mut_obj_add_bool(report.doc, root, "database_opened", false);
    yyjson_mut_obj_add_uint(report.doc, root, "directory_enumerations",
        scanner.metrics().directory_enumerations);
    yyjson_mut_obj_add_uint(report.doc, root, "canonicalizations",
        scanner.metrics().canonicalizations);
    yyjson_mut_obj_add_uint(report.doc, root, "metadata_probes",
        scanner.metrics().metadata_probes);
    auto* phases = report.new_obj();
    for (const auto& [name, seconds] : phase_seconds) {
        auto* phase = report.new_obj();
        yyjson_mut_obj_add_real(report.doc, phase, "seconds", seconds);
        yyjson_mut_obj_add_uint(report.doc, phase, "directories", phase_counts[name].directories);
        yyjson_mut_obj_add_uint(report.doc, phase, "entries", phase_counts[name].entries);
        yyjson_mut_obj_add_uint(report.doc, phase, "sources", phase_counts[name].sources);
        yyjson_mut_obj_add_val(report.doc, phases, name.c_str(), phase);
    }
    yyjson_mut_obj_add_val(report.doc, root, "phases", phases);
    auto* language_counts = report.new_obj();
    for (const auto& [name, count] : languages) {
        yyjson_mut_obj_add_sint(report.doc, language_counts, name.c_str(), count);
    }
    yyjson_mut_obj_add_val(report.doc, root, "languages", language_counts);
    std::ofstream output(report_path, std::ios::binary);
    output << report.to_string() << '\n';
    output.close();
    REQUIRE(output.good());
    std::cerr << "benchmark sources=" << files.size() << " elapsed=" << elapsed
              << "s report=" << report_path.string() << '\n' << std::flush;
}
