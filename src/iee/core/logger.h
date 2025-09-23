#pragma once
#include <memory>
#include <string_view>
#include <spdlog/spdlog.h>

namespace iee::core {
    void init_logger(std::string_view log_path_utf8, bool verbose = false);

    std::shared_ptr<spdlog::logger> logger();

#define LOG_TRACE(...)    ::iee::core::logger()->trace(__VA_ARGS__)
#define LOG_DEBUG(...)    ::iee::core::logger()->debug(__VA_ARGS__)
#define LOG_INFO(...)     ::iee::core::logger()->info(__VA_ARGS__)
#define LOG_WARN(...)     ::iee::core::logger()->warn(__VA_ARGS__)
#define LOG_ERROR(...)    ::iee::core::logger()->error(__VA_ARGS__)
#define LOG_CRITICAL(...) ::iee::core::logger()->critical(__VA_ARGS__)

#define LOG_DEBUG_FAST(...)    do { if (::iee::core::logger()->should_log(spdlog::level::debug)) { ::iee::core::logger()->debug(__VA_ARGS__); } } while(0)
#define LOG_TRACE_FAST(...)    do { if (::iee::core::logger()->should_log(spdlog::level::trace)) { ::iee::core::logger()->trace(__VA_ARGS__); } } while(0)
}
