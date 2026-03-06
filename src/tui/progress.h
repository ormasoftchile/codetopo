#pragma once

// T046: FTXUI TUI progress display for indexing.
// Shows progress bar, current file, elapsed time, speed, and error log.

#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/screen.hpp>
#include <ftxui/screen/terminal.hpp>
#include <string>
#include <vector>
#include <mutex>
#include <chrono>
#include <atomic>
#include <thread>
#include <sstream>
#include <iomanip>

namespace codetopo {

struct ProgressState {
    std::atomic<int> files_total{0};
    std::atomic<int> files_processed{0};
    std::atomic<int> errors{0};
    std::string current_file;
    std::vector<std::string> error_messages;
    std::chrono::steady_clock::time_point start_time;
    std::mutex mutex;

    void set_current(const std::string& file) {
        std::lock_guard<std::mutex> lock(mutex);
        current_file = file;
    }

    void add_error(const std::string& msg) {
        std::lock_guard<std::mutex> lock(mutex);
        if (error_messages.size() < 20) {
            error_messages.push_back(msg);
        }
    }

    std::string get_current() {
        std::lock_guard<std::mutex> lock(mutex);
        return current_file;
    }

    std::vector<std::string> get_errors() {
        std::lock_guard<std::mutex> lock(mutex);
        return error_messages;
    }
};

class TuiProgress {
public:
    explicit TuiProgress(ProgressState& state) : state_(state) {}

    void render_once() {
        using namespace ftxui;

        int total = state_.files_total;
        int processed = state_.files_processed;
        float ratio = total > 0 ? static_cast<float>(processed) / total : 0.0f;

        auto elapsed = std::chrono::steady_clock::now() - state_.start_time;
        auto elapsed_s = std::chrono::duration_cast<std::chrono::seconds>(elapsed).count();
        int minutes = static_cast<int>(elapsed_s / 60);
        int seconds = static_cast<int>(elapsed_s % 60);

        float speed = elapsed_s > 0 ? static_cast<float>(processed) / elapsed_s : 0.0f;

        std::string current = state_.get_current();
        auto errors = state_.get_errors();

        // Build progress text
        std::ostringstream progress_text;
        progress_text << processed << "/" << total
                      << " (" << std::fixed << std::setprecision(1)
                      << (ratio * 100.0f) << "%)";

        std::ostringstream time_text;
        time_text << std::setfill('0') << std::setw(2) << minutes
                  << ":" << std::setfill('0') << std::setw(2) << seconds
                  << "  |  " << std::fixed << std::setprecision(1)
                  << speed << " files/sec";

        // Build FTXUI elements
        Elements error_elements;
        for (auto& e : errors) {
            error_elements.push_back(text("  ⚠ " + e) | color(Color::Yellow));
        }

        auto doc = vbox({
            text("codetopo index") | bold | color(Color::Cyan),
            separator(),
            hbox({
                text("Progress: "),
                gauge(ratio) | flex,
                text(" " + progress_text.str()),
            }),
            hbox({text("Current:  "), text(current) | dim}),
            hbox({text("Elapsed:  "), text(time_text.str())}),
            separator(),
            text("Errors: " + std::to_string(state_.errors.load())),
            vbox(error_elements),
        }) | border;

        auto screen = Screen::Create(Dimension::Full(), Dimension::Fit(doc));
        Render(screen, doc);

        // Move cursor up to overwrite previous output
        std::cout << screen.ToString() << "\033[" << screen.dimy() << "A";
        std::cout.flush();
    }

    void render_final() {
        using namespace ftxui;

        int total = state_.files_total;
        int processed = state_.files_processed;

        auto elapsed = std::chrono::steady_clock::now() - state_.start_time;
        auto elapsed_s = std::chrono::duration_cast<std::chrono::seconds>(elapsed).count();

        auto doc = vbox({
            text("codetopo index — complete") | bold | color(Color::Green),
            separator(),
            hbox({text("Indexed: "), text(std::to_string(processed) + " files")}),
            hbox({text("Elapsed: "), text(std::to_string(elapsed_s) + "s")}),
            hbox({text("Errors:  "), text(std::to_string(state_.errors.load()))}),
        }) | border;

        auto screen = Screen::Create(Dimension::Full(), Dimension::Fit(doc));
        Render(screen, doc);
        std::cout << screen.ToString() << "\n";
        std::cout.flush();
    }

private:
    ProgressState& state_;
};

// T047: Non-TTY fallback
class FallbackProgress {
public:
    explicit FallbackProgress(ProgressState& state) : state_(state) {}

    void render_once() {
        int processed = state_.files_processed;
        int total = state_.files_total;
        auto current = state_.get_current();

        std::cerr << "\r[" << processed << "/" << total << "] " << current;
    }

    void render_final() {
        int processed = state_.files_processed;
        auto elapsed = std::chrono::steady_clock::now() - state_.start_time;
        auto elapsed_s = std::chrono::duration_cast<std::chrono::seconds>(elapsed).count();

        std::cerr << "\nIndexed " << processed << " files in " << elapsed_s << "s";
        if (state_.errors > 0) std::cerr << " (" << state_.errors << " errors)";
        std::cerr << "\n";
    }

private:
    ProgressState& state_;
};

} // namespace codetopo
