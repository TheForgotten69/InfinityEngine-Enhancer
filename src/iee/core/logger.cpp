#include "logger.h"
#include <mutex>
#include <spdlog/sinks/basic_file_sink.h>

namespace iee::core {
    static std::once_flag g_once;
    static std::shared_ptr<spdlog::logger> g_logger;

    void init_logger(std::string_view log_path_utf8, bool verbose) {
        std::call_once(g_once, [&] {
            auto file_sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(
                std::string{log_path_utf8}, false);

            g_logger = std::make_shared<spdlog::logger>("iee", file_sink);
            spdlog::register_logger(g_logger);

            g_logger->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] %v");
            g_logger->set_level(verbose ? spdlog::level::trace : spdlog::level::info);
            g_logger->flush_on(spdlog::level::info);
        });
    }

    std::shared_ptr<spdlog::logger> logger() {
        if (!g_logger) return spdlog::default_logger();
        return g_logger;
    }
}
