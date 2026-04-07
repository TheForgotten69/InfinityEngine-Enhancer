#include "config.h"
#include "logger.h"
#include <algorithm>
#include <charconv>
#include <fstream>
#include <optional>
#ifdef _WIN64
#include <windows.h>
#endif

namespace iee::core {
    static std::string trim(std::string s) {
        auto issp = [](unsigned char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
        s.erase(s.begin(), std::find_if(s.begin(), s.end(), [&](unsigned char c) { return !issp(c); }));
        s.erase(std::find_if(s.rbegin(), s.rend(), [&](unsigned char c) { return !issp(c); }).base(), s.end());
        return s;
    }

    static bool ensure_parent_dir(const std::filesystem::path &p) {
        std::error_code ec;
        const auto d = p.parent_path();
        if (d.empty() || std::filesystem::exists(d)) return true;
        return std::filesystem::create_directories(d, ec) && !ec;
    }

    static bool iequals(std::string a, std::string b) {
        std::transform(a.begin(), a.end(), a.begin(), ::tolower);
        std::transform(b.begin(), b.end(), b.begin(), ::tolower);
        return a == b;
    }

    static bool parse_bool(const std::string &s, bool def) {
        if (iequals(s, "1") || iequals(s, "true") || iequals(s, "yes") || iequals(s, "on")) return true;
        if (iequals(s, "0") || iequals(s, "false") || iequals(s, "no") || iequals(s, "off")) return false;
        return def;
    }

    static float parse_float(const std::string &s, float def) {
        try { return std::stof(s); } catch (...) { return def; }
    }

    static int parse_int(const std::string &s, int def) {
        try {
            std::size_t consumed = 0;
            const auto value = std::stoi(trim(s), &consumed, 0);
            return consumed > 0 ? value : def;
        } catch (...) {
            return def;
        }
    }

    static std::optional<std::uint32_t> parse_u32_dec_or_hex(std::string s) {
        s = trim(std::move(s));
        if (s.rfind("0x", 0) == 0 || s.rfind("0X", 0) == 0) {
            std::uint32_t v{};
            auto *b = s.data() + 2;
            auto *e = s.data() + s.size();
            if (std::from_chars(b, e, v, 16).ec == std::errc{}) return v;
            return std::nullopt;
        }
        {
            std::uint32_t v{};
            auto *b = s.data();
            auto *e = s.data() + s.size();
            if (std::from_chars(b, e, v, 10).ec == std::errc{}) return v;
        }
        {
            std::uint32_t v{};
            auto *b = s.data();
            auto *e = s.data() + s.size();
            if (std::from_chars(b, e, v, 16).ec == std::errc{}) return v;
        }
        return std::nullopt;
    }


    static void apply_kv(EngineConfig &cfg,
                         const std::string &section,
                         const std::string &key,
                         const std::string &val) {
        // [Core]
        if (iequals(section, "core")) {
            if (iequals(key, "VerboseLogs")) cfg.enableVerboseLogging = parse_bool(val, cfg.enableVerboseLogging);
            else if (iequals(key, "EnableShaderTracing"))
                cfg.enableShaderTracing = parse_bool(val, cfg.enableShaderTracing);
            else if (iequals(key, "EnableSpriteTcScaleInjection"))
                cfg.enableSpriteTcScaleInjection = parse_bool(val, cfg.enableSpriteTcScaleInjection);
            else if (iequals(key, "EnableBatchHighlightProbe"))
                cfg.enableBatchHighlightProbe = parse_bool(val, cfg.enableBatchHighlightProbe);
            else if (iequals(key, "EnableBatchSuppressProbe"))
                cfg.enableBatchSuppressProbe = parse_bool(val, cfg.enableBatchSuppressProbe);
            else if (iequals(key, "EnableGlProgramSuppressProbe"))
                cfg.enableGlProgramSuppressProbe = parse_bool(val, cfg.enableGlProgramSuppressProbe);
            else if (iequals(key, "EnableGlTextureSuppressProbe"))
                cfg.enableGlTextureSuppressProbe = parse_bool(val, cfg.enableGlTextureSuppressProbe);
            else if (iequals(key, "BatchHighlightProgram"))
                cfg.batchHighlightProgram = parse_int(val, cfg.batchHighlightProgram);
            else if (iequals(key, "BatchHighlightTextureId"))
                cfg.batchHighlightTextureId = parse_int(val, cfg.batchHighlightTextureId);
            else if (iequals(key, "BatchSuppressMinScreenWidth"))
                cfg.batchSuppressMinScreenWidth = parse_int(val, cfg.batchSuppressMinScreenWidth);
            else if (iequals(key, "BatchSuppressMaxScreenWidth"))
                cfg.batchSuppressMaxScreenWidth = parse_int(val, cfg.batchSuppressMaxScreenWidth);
            else if (iequals(key, "BatchSuppressMinScreenHeight"))
                cfg.batchSuppressMinScreenHeight = parse_int(val, cfg.batchSuppressMinScreenHeight);
            else if (iequals(key, "BatchSuppressMaxScreenHeight"))
                cfg.batchSuppressMaxScreenHeight = parse_int(val, cfg.batchSuppressMaxScreenHeight);
            else if (iequals(key, "BatchSuppressMinCenterX"))
                cfg.batchSuppressMinCenterX = parse_int(val, cfg.batchSuppressMinCenterX);
            else if (iequals(key, "BatchSuppressMaxCenterX"))
                cfg.batchSuppressMaxCenterX = parse_int(val, cfg.batchSuppressMaxCenterX);
            else if (iequals(key, "BatchSuppressMinCenterY"))
                cfg.batchSuppressMinCenterY = parse_int(val, cfg.batchSuppressMinCenterY);
            else if (iequals(key, "BatchSuppressMaxCenterY"))
                cfg.batchSuppressMaxCenterY = parse_int(val, cfg.batchSuppressMaxCenterY);
            else if (iequals(key, "GlProgramSuppress"))
                cfg.glProgramSuppress = parse_int(val, cfg.glProgramSuppress);
            else if (iequals(key, "GlTextureSuppressProgram"))
                cfg.glTextureSuppressProgram = parse_int(val, cfg.glTextureSuppressProgram);
            else if (iequals(key, "GlTextureSuppressTexture"))
                cfg.glTextureSuppressTexture = parse_int(val, cfg.glTextureSuppressTexture);
            else if (iequals(key, "ShaderTraceRuntimeProgramFilter"))
                cfg.shaderTraceRuntimeProgramFilter = parse_int(val, cfg.shaderTraceRuntimeProgramFilter);
            else if (iequals(key, "ShaderTraceRuntimeTextureFilter"))
                cfg.shaderTraceRuntimeTextureFilter = parse_int(val, cfg.shaderTraceRuntimeTextureFilter);
            return;
        }

        // [Auto-Generated] - cached addresses from pattern scanning
        if (iequals(section, "auto-generated")) {
            if (iequals(key, "CachedLoadAreaRVA")) {
                if (auto v = parse_u32_dec_or_hex(val)) cfg.cachedLoadAreaRVA = *v;
            } else if (iequals(key, "CachedRenderTextureRVA")) {
                if (auto v = parse_u32_dec_or_hex(val)) cfg.cachedRenderTextureRVA = *v;
            }
            return;
        }

        // [Rendering]
        if (iequals(section, "rendering")) {
            if (iequals(key, "EnableAnisotropicFiltering"))
                cfg.enableAnisotropicFiltering = parse_bool(
                    val, cfg.enableAnisotropicFiltering);
            else if (iequals(key, "MaxAnisotropy")) cfg.maxAnisotropy = parse_float(val, cfg.maxAnisotropy);
            else if (iequals(key, "LODBias")) cfg.lodBias = parse_float(val, cfg.lodBias);
            return;
        }

        // [Addresses]
        if (iequals(section, "addresses")) {
            if (iequals(key, "FallbackLoadAreaRVA")) {
                if (auto v = parse_u32_dec_or_hex(val)) cfg.cachedLoadAreaRVA = *v;
            } else if (iequals(key, "FallbackRenderTextureRVA")) {
                if (auto v = parse_u32_dec_or_hex(val)) cfg.cachedRenderTextureRVA = *v;
            }
        }
    }

    static void write_section(std::ofstream &f, const char *name) {
        f << "\n[" << name << "]\n";
    }

    static void write_bool(std::ofstream &f, const char *key, bool v) {
        f << key << " = " << (v ? "true" : "false") << "\n";
    }


    std::filesystem::path ConfigManager::config_path() {
#ifdef _WIN64
        wchar_t wpath[MAX_PATH]{};
        GetModuleFileNameW(GetModuleHandleW(nullptr), wpath, MAX_PATH);
        std::filesystem::path p(wpath);
        p = p.remove_filename() / "InfinityEngine-Enhancer.ini";
        return p;
#else
        return std::filesystem::current_path() / "InfinityEngine-Enhancer.ini";
#endif
    }

    bool ConfigManager::load(const std::filesystem::path &path, EngineConfig &out) {
        std::ifstream in(path);
        if (!in) {
            return false;
        }

        EngineConfig cfg = out;
        std::string section;
        std::string line;
        size_t lineno = 0;

        while (std::getline(in, line)) {
            ++lineno;
            auto raw = trim(line);
            if (raw.empty() || raw[0] == ';' || raw[0] == '#') continue;

            if (raw.front() == '[' && raw.back() == ']') {
                section = trim(raw.substr(1, raw.size() - 2));
                continue;
            }

            auto eq = raw.find('=');
            if (eq == std::string::npos) {
                LOG_WARN("Malformed INI line {}: {}", lineno, raw);
                continue;
            }

            auto key = trim(raw.substr(0, eq));
            auto val = trim(raw.substr(eq + 1));
            apply_kv(cfg, section, key, val);
        }

        out = cfg;
        return true;
    }

    bool ConfigManager::save(const std::filesystem::path &path, const EngineConfig &cfg) {
        if (!ensure_parent_dir(path)) return false;

        std::ofstream f(path, std::ios::trunc);
        if (!f) return false;

        f << "; InfinityEngine-Enhancer configuration\n";
        f << "; Lines starting with ';' or '#' are comments.\n";
        f << "; You can use decimal or hex (0x...) for RVAs.\n\n";

        write_section(f, "Core");
        write_bool(f, "VerboseLogs", cfg.enableVerboseLogging);
        write_bool(f, "EnableShaderTracing", cfg.enableShaderTracing);
        write_bool(f, "EnableSpriteTcScaleInjection", cfg.enableSpriteTcScaleInjection);
        write_bool(f, "EnableBatchHighlightProbe", cfg.enableBatchHighlightProbe);
        write_bool(f, "EnableBatchSuppressProbe", cfg.enableBatchSuppressProbe);
        write_bool(f, "EnableGlProgramSuppressProbe", cfg.enableGlProgramSuppressProbe);
        write_bool(f, "EnableGlTextureSuppressProbe", cfg.enableGlTextureSuppressProbe);
        f << "BatchHighlightProgram = " << cfg.batchHighlightProgram << "\n";
        f << "BatchHighlightTextureId = " << cfg.batchHighlightTextureId << "\n";
        f << "BatchSuppressMinScreenWidth = " << cfg.batchSuppressMinScreenWidth << "\n";
        f << "BatchSuppressMaxScreenWidth = " << cfg.batchSuppressMaxScreenWidth << "\n";
        f << "BatchSuppressMinScreenHeight = " << cfg.batchSuppressMinScreenHeight << "\n";
        f << "BatchSuppressMaxScreenHeight = " << cfg.batchSuppressMaxScreenHeight << "\n";
        f << "BatchSuppressMinCenterX = " << cfg.batchSuppressMinCenterX << "\n";
        f << "BatchSuppressMaxCenterX = " << cfg.batchSuppressMaxCenterX << "\n";
        f << "BatchSuppressMinCenterY = " << cfg.batchSuppressMinCenterY << "\n";
        f << "BatchSuppressMaxCenterY = " << cfg.batchSuppressMaxCenterY << "\n";
        f << "GlProgramSuppress = " << cfg.glProgramSuppress << "\n";
        f << "GlTextureSuppressProgram = " << cfg.glTextureSuppressProgram << "\n";
        f << "GlTextureSuppressTexture = " << cfg.glTextureSuppressTexture << "\n";
        f << "ShaderTraceRuntimeProgramFilter = " << cfg.shaderTraceRuntimeProgramFilter << "\n";
        f << "ShaderTraceRuntimeTextureFilter = " << cfg.shaderTraceRuntimeTextureFilter << "\n";

        write_section(f, "Auto-Generated");
        f << "CachedLoadAreaRVA = 0x" << std::hex << cfg.cachedLoadAreaRVA << std::dec << "\n";
        f << "CachedRenderTextureRVA = 0x" << std::hex << cfg.cachedRenderTextureRVA << std::dec << "\n";

        write_section(f, "Rendering");
        write_bool(f, "EnableAnisotropicFiltering", cfg.enableAnisotropicFiltering);
        f << "MaxAnisotropy = " << cfg.maxAnisotropy << "\n";
        f << "LODBias = " << cfg.lodBias << "\n";

        write_section(f, "Addresses");
        f << "FallbackLoadAreaRVA = 0x" << std::hex << std::uppercase
                << static_cast<std::uint32_t>(cfg.cachedLoadAreaRVA) << std::dec << "\n";
        f << "FallbackRenderTextureRVA = 0x" << std::hex << std::uppercase
                << static_cast<std::uint32_t>(cfg.cachedRenderTextureRVA) << std::dec << "\n";

        return true;
    }

    EngineConfig ConfigManager::load_or_default() {
        EngineConfig cfg{};
        const auto path = config_path();
        if (!std::filesystem::exists(path)) {
            (void) save(path, cfg);
            return cfg;
        }
        (void) load(path, cfg);
        return cfg;
    }
}
