#include "logger.h"

#include <spdlog/sinks/basic_file_sink.h>

#include <memory>
#include <mutex>

namespace iee::core {
static std::once_flag g_once;
static std::shared_ptr<spdlog::logger> g_logger;

void init_logger(std::string_view log_path_utf8, bool verbose) {
  std::call_once(g_once, [&] {
    auto file_sink =
        std::make_shared<spdlog::sinks::basic_file_sink_mt>(std::string{log_path_utf8}, false);

    g_logger = std::make_shared<spdlog::logger>("iee", file_sink);
    spdlog::register_logger(g_logger);

    g_logger->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] %v");
    g_logger->set_level(verbose ? spdlog::level::trace : spdlog::level::info);
    // Flush on info: in-game validation reads the log tail after crashes,
    // so buffered INFO lines must survive an abnormal exit.
    g_logger->flush_on(spdlog::level::info);
  });
}

spdlog::logger* logger() noexcept {
  static const auto fallback = spdlog::default_logger();
  return g_logger ? g_logger.get() : fallback.get();
}
}  // namespace iee::core
