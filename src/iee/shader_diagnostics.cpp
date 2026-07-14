#include "shader_diagnostics.h"

#include <dbghelp.h>
#include <spdlog/fmt/fmt.h>
#include <windows.h>

#include <array>
#include <cctype>
#include <fstream>
#include <mutex>
#include <optional>

#include "iee/core/logger.h"
#include "iee/core/pattern_scanner.h"

namespace iee::probe::diagnostics {
namespace {

struct CallerInfo {
  std::uintptr_t address{};
  std::optional<std::uintptr_t> executableRva;
  std::string moduleName;
  std::string symbolName;
  std::uint64_t symbolDisplacement{};
};

std::string basename(std::string path) {
  const auto separator = path.find_last_of("\\/");
  return separator == std::string::npos ? path : path.substr(separator + 1);
}

std::string safe_filename(std::string_view name) {
  std::string filename(name);
  for (char& character : filename) {
    const auto value = static_cast<unsigned char>(character);
    if (!std::isalnum(value) && character != '_' && character != '-' && character != '.') {
      character = '_';
    }
  }
  return filename;
}

CallerInfo resolve_caller(std::uintptr_t address) {
  CallerInfo info{};
  info.address = address;
  if (const auto module = core::get_module_span(nullptr)) {
    const auto moduleBase = reinterpret_cast<std::uintptr_t>(module->base);
    const auto moduleEnd = moduleBase + module->size;
    if (address >= moduleBase && address < moduleEnd) {
      info.executableRva = address - moduleBase;
    }
  }

  HMODULE callerModule = nullptr;
  if (GetModuleHandleExA(
          GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
          reinterpret_cast<LPCSTR>(address), &callerModule)) {
    std::array<char, MAX_PATH> pathBuffer{};
    const auto written =
        GetModuleFileNameA(callerModule, pathBuffer.data(), static_cast<DWORD>(pathBuffer.size()));
    if (written > 0) {
      info.moduleName = basename(std::string(pathBuffer.data(), static_cast<std::size_t>(written)));
    }
  }

  static std::once_flag initializeSymbols;
  static bool symbolsReady = false;
  std::call_once(initializeSymbols,
                 [] { symbolsReady = SymInitialize(GetCurrentProcess(), nullptr, TRUE) == TRUE; });
  if (!symbolsReady) {
    return info;
  }

  std::array<char, sizeof(SYMBOL_INFO) + MAX_SYM_NAME> symbolBuffer{};
  auto* symbol = reinterpret_cast<SYMBOL_INFO*>(symbolBuffer.data());
  symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
  symbol->MaxNameLen = MAX_SYM_NAME;
  DWORD64 displacement = 0;
  if (SymFromAddr(GetCurrentProcess(), static_cast<DWORD64>(address), &displacement, symbol)) {
    info.symbolName.assign(symbol->Name, symbol->NameLen);
    info.symbolDisplacement = displacement;
  }
  return info;
}

}  // namespace

std::string caller_summary(std::uintptr_t caller) {
  const auto info = resolve_caller(caller);
  std::string summary = fmt::format("caller=0x{:X}", info.address);
  if (info.executableRva) {
    summary += fmt::format(" rva=0x{:X}", *info.executableRva);
  }
  if (!info.moduleName.empty()) {
    summary += fmt::format(" module={}", info.moduleName);
  }
  if (!info.symbolName.empty()) {
    summary += fmt::format(" symbol={}", info.symbolName);
    if (info.symbolDisplacement != 0) {
      summary += fmt::format("+0x{:X}", info.symbolDisplacement);
    }
  }
  return summary;
}

void dump_shader_source(const std::filesystem::path& directory, std::string_view name,
                        std::string_view source) {
  if (directory.empty() || name.empty()) {
    return;
  }

  std::error_code error;
  std::filesystem::create_directories(directory, error);
  if (error) {
    LOG_WARN("Could not create shader dump directory {}: {}", directory.string(), error.message());
    return;
  }

  const auto filePath = directory / (safe_filename(name) + ".glsl");
  std::ofstream output(filePath, std::ios::trunc | std::ios::binary);
  if (!output) {
    LOG_WARN("Could not open shader dump {}", filePath.string());
    return;
  }
  output.write(source.data(), static_cast<std::streamsize>(source.size()));
  LOG_DEBUG("Dumped shader {} ({} bytes)", filePath.string(), source.size());
}

}  // namespace iee::probe::diagnostics
