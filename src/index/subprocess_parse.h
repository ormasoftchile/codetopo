#pragma once
// subprocess_parse.h — Run parse-file in a child process for crash isolation.
//
// Invokes the same codetopo binary with the `parse-file` subcommand.
// If tree-sitter crashes, only the child process dies. The parent reads
// the exit code and marks the file accordingly.

#include "index/extractor.h"
#include <string>
#include <sstream>
#include <cstdio>
#include <array>
#include <filesystem>
#include <yyjson.h>

#ifdef _WIN32
#define popen _popen
#define pclose _pclose
#endif

namespace codetopo {

struct SubprocessResult {
    bool success = false;
    bool crashed = false;
    std::string parse_status;   // ok, partial, failed, skipped
    std::string parse_error;
    std::string content_hash;
    ExtractionResult extraction;
};

// Run parse-file in a subprocess. Returns the parsed result or crash indicator.
inline SubprocessResult subprocess_parse(const std::string& exe_path,
                                          const std::string& file_path,
                                          const std::string& language,
                                          const std::string& repo_root,
                                          int max_symbols = 50000,
                                          int max_depth = 500) {
    SubprocessResult result;

    // Build command line. Quote paths for safety.
    std::ostringstream cmd;
    cmd << "\"" << exe_path << "\""
        << " parse-file"
        << " --file \"" << file_path << "\""
        << " --lang " << language
        << " --root \"" << repo_root << "\""
        << " --max-symbols " << max_symbols
        << " --max-depth " << max_depth
        << " 2>"; // suppress stderr
#ifdef _WIN32
    cmd << "NUL";
#else
    cmd << "/dev/null";
#endif

    // Run subprocess and capture stdout
    std::string output;
    {
        FILE* pipe = popen(cmd.str().c_str(), "r");
        if (!pipe) {
            result.crashed = true;
            result.parse_error = "failed to launch subprocess";
            return result;
        }
        std::array<char, 4096> buf;
        while (fgets(buf.data(), static_cast<int>(buf.size()), pipe)) {
            output += buf.data();
        }
        int status = pclose(pipe);
#ifdef _WIN32
        // On Windows, pclose returns the process exit code directly
        if (status != 0 && output.empty()) {
            result.crashed = true;
            result.parse_error = "subprocess crashed (exit " + std::to_string(status) + ")";
            return result;
        }
#else
        if (WIFEXITED(status)) {
            if (WEXITSTATUS(status) != 0 && output.empty()) {
                result.crashed = true;
                result.parse_error = "subprocess failed (exit " + std::to_string(WEXITSTATUS(status)) + ")";
                return result;
            }
        } else if (WIFSIGNALED(status)) {
            result.crashed = true;
            result.parse_error = "subprocess crashed (signal " + std::to_string(WTERMSIG(status)) + ")";
            return result;
        }
#endif
    }

    if (output.empty()) {
        result.crashed = true;
        result.parse_error = "subprocess produced no output";
        return result;
    }

    // Parse JSON output
    yyjson_doc* doc = yyjson_read(output.c_str(), output.size(), 0);
    if (!doc) {
        result.parse_error = "subprocess output is not valid JSON";
        return result;
    }

    yyjson_val* root = yyjson_doc_get_root(doc);
    auto* st = yyjson_obj_get(root, "status");
    auto* err = yyjson_obj_get(root, "error");
    auto* ch = yyjson_obj_get(root, "content_hash");

    if (st) result.parse_status = yyjson_get_str(st);
    if (err) result.parse_error = yyjson_get_str(err);
    if (ch) result.content_hash = yyjson_get_str(ch);

    // Parse symbols
    auto* syms = yyjson_obj_get(root, "symbols");
    if (syms && yyjson_is_arr(syms)) {
        size_t idx, max;
        yyjson_val* val;
        yyjson_arr_foreach(syms, idx, max, val) {
            ExtractedSymbol sym;
            auto* k = yyjson_obj_get(val, "kind");
            auto* n = yyjson_obj_get(val, "name");
            auto* q = yyjson_obj_get(val, "qualname");
            auto* sig = yyjson_obj_get(val, "signature");
            auto* sl = yyjson_obj_get(val, "start_line");
            auto* sc = yyjson_obj_get(val, "start_col");
            auto* el = yyjson_obj_get(val, "end_line");
            auto* ec = yyjson_obj_get(val, "end_col");
            auto* id = yyjson_obj_get(val, "is_definition");
            auto* vis = yyjson_obj_get(val, "visibility");
            auto* sk = yyjson_obj_get(val, "stable_key");
            if (k) sym.kind = yyjson_get_str(k);
            if (n) sym.name = yyjson_get_str(n);
            if (q) sym.qualname = yyjson_get_str(q);
            if (sig) sym.signature = yyjson_get_str(sig);
            if (sl) sym.start_line = static_cast<int>(yyjson_get_int(sl));
            if (sc) sym.start_col = static_cast<int>(yyjson_get_int(sc));
            if (el) sym.end_line = static_cast<int>(yyjson_get_int(el));
            if (ec) sym.end_col = static_cast<int>(yyjson_get_int(ec));
            if (id) sym.is_definition = yyjson_get_bool(id);
            if (vis) sym.visibility = yyjson_get_str(vis);
            if (sk) sym.stable_key = yyjson_get_str(sk);
            result.extraction.symbols.push_back(std::move(sym));
        }
    }

    // Parse refs
    auto* refs_arr = yyjson_obj_get(root, "refs");
    if (refs_arr && yyjson_is_arr(refs_arr)) {
        size_t idx, max;
        yyjson_val* val;
        yyjson_arr_foreach(refs_arr, idx, max, val) {
            ExtractedRef ref;
            auto* k = yyjson_obj_get(val, "kind");
            auto* n = yyjson_obj_get(val, "name");
            auto* sl = yyjson_obj_get(val, "start_line");
            auto* sc = yyjson_obj_get(val, "start_col");
            auto* ev = yyjson_obj_get(val, "evidence");
            if (k) ref.kind = yyjson_get_str(k);
            if (n) ref.name = yyjson_get_str(n);
            if (sl) ref.start_line = static_cast<int>(yyjson_get_int(sl));
            if (sc) ref.start_col = static_cast<int>(yyjson_get_int(sc));
            if (ev) ref.evidence = yyjson_get_str(ev);
            result.extraction.refs.push_back(std::move(ref));
        }
    }

    // Parse edges
    auto* edges_arr = yyjson_obj_get(root, "edges");
    if (edges_arr && yyjson_is_arr(edges_arr)) {
        size_t idx, max;
        yyjson_val* val;
        yyjson_arr_foreach(edges_arr, idx, max, val) {
            ExtractedEdge edge;
            auto* si = yyjson_obj_get(val, "src_index");
            auto* di = yyjson_obj_get(val, "dst_index");
            auto* dn = yyjson_obj_get(val, "dst_name");
            auto* k = yyjson_obj_get(val, "kind");
            auto* c = yyjson_obj_get(val, "confidence");
            if (si) edge.src_index = static_cast<int>(yyjson_get_int(si));
            if (di) edge.dst_index = static_cast<int>(yyjson_get_int(di));
            if (dn) edge.dst_name = yyjson_get_str(dn);
            if (k) edge.kind = yyjson_get_str(k);
            if (c) edge.confidence = yyjson_get_real(c);
            result.extraction.edges.push_back(std::move(edge));
        }
    }

    yyjson_doc_free(doc);
    result.success = (result.parse_status == "ok" || result.parse_status == "partial");
    return result;
}

} // namespace codetopo
