#pragma once

// Small helpers shared by the two simulator binaries: argument parsing,
// logging and Ctrl+C handling. Deliberately minimal, a full CLI library
// would be more code than the simulators themselves.

#include <atomic>
#include <charconv>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <initializer_list>
#include <iostream>
#include <map>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>

namespace sim {

// Parses "--name value" pairs. Anything else is rejected, so a typo in a
// flag name fails loudly instead of being ignored.
class Args {
public:
    Args(int argc, char** argv, std::initializer_list<std::string_view> known) {
        for (int i = 1; i < argc; i++) {
            std::string_view flag(argv[i]);
            if (flag == "--help" || flag == "-h") {
                help_ = true;
                continue;
            }
            if (flag.substr(0, 2) != "--" || i + 1 >= argc) {
                throw std::invalid_argument("expected --name value, got '" + std::string(flag) + "'");
            }
            std::string name(flag.substr(2));
            bool is_known = false;
            for (auto k : known) is_known = is_known || k == name;
            if (!is_known) throw std::invalid_argument("unknown option --" + name);
            values_[name] = argv[++i];
        }
    }

    [[nodiscard]] bool help() const noexcept { return help_; }

    [[nodiscard]] std::string get(const std::string& name, std::string fallback) const {
        auto it = values_.find(name);
        return it == values_.end() ? std::move(fallback) : it->second;
    }

    template <typename T>
        requires std::is_arithmetic_v<T>
    [[nodiscard]] T get(const std::string& name, T fallback) const {
        auto it = values_.find(name);
        if (it == values_.end()) return fallback;
        const std::string& text = it->second;
        T value{};
        auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
        if (error != std::errc{} || end != text.data() + text.size()) {
            throw std::invalid_argument("--" + name + " expects a number, got '" + text + "'");
        }
        return value;
    }

private:
    std::map<std::string, std::string> values_;
    bool help_ = false;
};

// Prints "[  1.234] who: message" with the time since program start. The
// output is flushed every line, the system tests read it through a pipe.
class Logger {
public:
    explicit Logger(std::string who) : who_(std::move(who)), start_(std::chrono::steady_clock::now()) {}

    template <typename... Parts>
    void info(const Parts&... parts) const {
        std::ostringstream line;
        const auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start_);
        char stamp[32];
        std::snprintf(stamp, sizeof(stamp), "[%7.3f] ", elapsed.count());
        line << stamp << who_ << ": ";
        (line << ... << parts);
        std::cout << line.str() << std::endl;
    }

private:
    std::string who_;
    std::chrono::steady_clock::time_point start_;
};

// Set from the signal handler, read by the main loops. A lock free atomic
// is one of the few things that is safe to touch inside a signal handler.
inline std::atomic<bool> stop_requested{false};

inline void install_stop_handler() {
    auto handler = [](int) { stop_requested.store(true); };
    std::signal(SIGINT, handler);
    std::signal(SIGTERM, handler);
#ifdef SIGPIPE
    std::signal(SIGPIPE, SIG_IGN);
#endif
}

}  // namespace sim
