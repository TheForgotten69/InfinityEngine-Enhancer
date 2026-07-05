#include "water_textures.h"

#include <cstdint>
#include <cstring>
#include <fstream>
#include <vector>

#include "iee/core/gl_state_guard.h"
#include "iee/core/logger.h"
#include "iee/game/opengl_types.h"

namespace iee::water {
    namespace {
        struct RawImage {
            std::uint32_t width{};
            std::uint32_t height{};
            std::vector<std::uint8_t> rgba;
        };

        // Reads the authoring-time blob format: "IRGB" magic, u32 width,
        // u32 height (little endian), then width*height*4 bytes of RGBA.
        bool read_irgb(const std::filesystem::path &path, RawImage &out) {
            std::ifstream file(path, std::ios::binary);
            if (!file) {
                return false;
            }
            char magic[4] = {};
            file.read(magic, 4);
            if (std::memcmp(magic, "IRGB", 4) != 0) {
                LOG_WARN("Water texture {} has a bad magic header", path.filename().string());
                return false;
            }
            std::uint32_t dims[2] = {};
            file.read(reinterpret_cast<char *>(dims), sizeof(dims));
            if (dims[0] == 0 || dims[1] == 0 || dims[0] > 8192 || dims[1] > 8192) {
                LOG_WARN("Water texture {} has implausible dimensions {}x{}",
                         path.filename().string(), dims[0], dims[1]);
                return false;
            }
            const std::size_t byteCount = std::size_t{dims[0]} * dims[1] * 4;
            out.rgba.resize(byteCount);
            file.read(reinterpret_cast<char *>(out.rgba.data()),
                      static_cast<std::streamsize>(byteCount));
            if (!file) {
                LOG_WARN("Water texture {} is truncated", path.filename().string());
                return false;
            }
            out.width = dims[0];
            out.height = dims[1];
            return true;
        }

        bool upload_to_unit(const RawImage &image, unsigned unit) {
            auto &gl = game::gl::get_gl_functions();
            if (!gl.textureUploadAvailable) {
                return false;
            }
            static unsigned s_textures[8] = {};
            if (unit >= 8) {
                return false;
            }
            if (s_textures[unit] == 0) {
                gl.glGenTextures(1, &s_textures[unit]);
            }
            gl.glActiveTexture(game::gl::TEXTURE0 + unit);
            gl.glBindTexture(game::gl::TEXTURE_2D, s_textures[unit]);
            gl.glPixelStorei(game::gl::UNPACK_ALIGNMENT, 1);
            gl.glTexImage2D(game::gl::TEXTURE_2D, 0, game::gl::RGBA8,
                            static_cast<int>(image.width), static_cast<int>(image.height), 0,
                            game::gl::RGBA, game::gl::UNSIGNED_BYTE, image.rgba.data());
            gl.glTexParameteri(game::gl::TEXTURE_2D, game::gl::TEXTURE_MIN_FILTER, game::gl::LINEAR);
            gl.glTexParameteri(game::gl::TEXTURE_2D, game::gl::TEXTURE_MAG_FILTER, game::gl::LINEAR);
            gl.glTexParameteri(game::gl::TEXTURE_2D, game::gl::TEXTURE_WRAP_S, game::gl::REPEAT);
            gl.glTexParameteri(game::gl::TEXTURE_2D, game::gl::TEXTURE_WRAP_T, game::gl::REPEAT);
            return game::gl::check_error("water texture upload");
        }
    }

    bool load_water_textures(const std::filesystem::path &dir) {
        struct Entry {
            const char *file;
            unsigned unit;
        };
        constexpr Entry kEntries[] = {
            {"iee_water_normal.rgba", 3},
            {"iee_water_dudv.rgba", 4},
            {"iee_water_foam.rgba", 5},
        };

        core::GlStateGuard guard;
        bool anyUploaded = false;
        for (const auto &entry : kEntries) {
            RawImage image;
            const auto path = dir / entry.file;
            if (!read_irgb(path, image)) {
                LOG_WARN("Water texture missing or unreadable: {}", path.string());
                continue;
            }
            if (upload_to_unit(image, entry.unit)) {
                LOG_INFO("Water texture uploaded: {} {}x{} (unit {})",
                         entry.file, image.width, image.height, entry.unit);
                anyUploaded = true;
            }
        }
        return anyUploaded;
    }
}
