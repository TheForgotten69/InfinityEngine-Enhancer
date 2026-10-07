#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace iee::game {
enum class BranchInstructionKind : std::uint8_t {
  CallRel32,
  JmpRel32,
};

struct BranchInstructionDesc {
  const char* name{};
  std::size_t offset{};
  BranchInstructionKind kind{BranchInstructionKind::CallRel32};
  std::uint8_t opcode{};
  std::size_t displacementOffset{};
  std::size_t instructionSize{};
  bool required{true};

  [[nodiscard]] constexpr bool validate() const noexcept {
    return name != nullptr && name[0] != '\0' && instructionSize > displacementOffset;
  }
};

struct PatternSet {
  std::string_view loadArea{};
  std::string_view renderTexture{};
  // Optional: CGameObjectArray::GetShare. Sourced from EEex's
  // version-independent binding pattern; resolves the static object-array
  // globals for the ARE-animation memory path. Empty (or a non-unique match
  // at runtime) disables that path — the disk reader remains the fallback.
  std::string_view objectArrayGetShare{};
  // Optional: CGameStatic::Render. Hooked to suppress the engine's authored
  // fire/smoke BAM draws while the fpSEAM point effects replace them. Empty
  // or non-unique keeps the engine draws (effects stay additive).
  std::string_view staticRender{};
  // Optional: CGameSprite::Render and the two per-sprite overlay renders that
  // read the same position. Hooked together for sprite movement smoothing;
  // any empty or non-unique entry leaves the feature off.
  std::string_view spriteRender{};
  std::string_view spriteRenderMarkers{};
  std::string_view spriteRenderHealthBar{};
  // Optional: CVidCell frame resolution, hooked for draw-time animation
  // interpolation. GetFrame has its own signature. GetCurrentCenterPoint and
  // GetCurrentFrameSize are byte-identical for their first 150+ bytes, so
  // they share one pattern that is confirmed at each reference RVA instead of
  // being searched for.
  std::string_view vidCellGetFrame{};
  std::string_view vidCellFrameAccessor{};
  // Optional: CInfinity::RenderFog and the GL backend's queue flush
  // (DrawFlush_GL). Hooked/called together for the world post passes (soft
  // fog of war, bloom); either empty or non-unique leaves both off.
  std::string_view renderFog{};
  std::string_view drawFlush{};
  // Optional: TexSubImage_GL, the upload of the streaming sprite atlas at the
  // start of a flush. Hooked to upscale the atlas before it is drawn from.
  std::string_view textureUpload{};
  // Optional pair: CParticle::AsynchronousUpdate and CParticle::Render (rain,
  // snow, sparkles). The update marks the logic tick; the render is wrapped
  // to draw each particle part of the way through its step.
  std::string_view particleUpdate{};
  std::string_view particleRender{};
};

struct ReferenceRvas {
  std::uintptr_t loadArea{};
  std::uintptr_t renderTexture{};
  // Diagnostic only; 0 means "not yet observed on this build".
  std::uintptr_t objectArrayGetShare{};
  std::uintptr_t staticRender{};
  std::uintptr_t spriteRender{};
  std::uintptr_t spriteRenderMarkers{};
  std::uintptr_t spriteRenderHealthBar{};
  std::uintptr_t vidCellGetFrame{};
  std::uintptr_t vidCellGetCurrentCenterPoint{};
  std::uintptr_t vidCellGetCurrentFrameSize{};
  std::uintptr_t renderFog{};
  std::uintptr_t drawFlush{};
  std::uintptr_t particleUpdate{};
  std::uintptr_t particleRender{};
  std::uintptr_t textureUpload{};
};

struct RuntimeOffsets {
  std::uintptr_t vidTileResource{};
  std::uintptr_t tisLinearTilesFlag{};
  std::uintptr_t tisHeaderTileDimension{};
  std::uintptr_t infGameVisibleArea{};
  std::uintptr_t infGameAreas{};
  std::uintptr_t infGameAreaMaster{};
};

struct ExecutableVersion {
  static constexpr std::uint16_t kAnyRevision = 0xFFFF;

  std::uint16_t major{};
  std::uint16_t minor{};
  std::uint16_t patch{};
  std::uint16_t revision{};

  [[nodiscard]] constexpr bool matches(std::uint16_t candidateMajor, std::uint16_t candidateMinor,
                                       std::uint16_t candidatePatch,
                                       std::uint16_t candidateRevision) const noexcept {
    return major == candidateMajor && minor == candidateMinor && patch == candidatePatch &&
           (revision == kAnyRevision || revision == candidateRevision);
  }
};

// One per-object render (virtual Render(CGameArea*, CVidMode*) of a
// CGameObject subclass) that draws from CGameObject::m_pos and can therefore
// be movement-smoothed with the position swap. Each resolves independently.
// The streaming sprite atlases (fx[0], fx[1]) and the texture table, all in
// the same GL state block as the draw queue and addressed relative to gl.n.
// Sprites are composited on the CPU into an atlas's buffer, uploaded at the
// start of each flush, and drawn from the atlas's texture.
struct SpriteAtlasLayout {
  std::uintptr_t userStateBeforeCount{};  // gl.user.state: selected texture in its bits
  std::uintptr_t texturesBeforeCount{};   // gl.textures[0]
  std::size_t textureEntrySize{};         // GL name is the first field of an entry
  std::uintptr_t atlasAfterCount{};       // fx[0]
  std::size_t atlasStride{};
  std::size_t atlasCount{};
  std::size_t widthOffset{};
  std::size_t heightOffset{};
  std::size_t texelsOffset{};
  std::size_t textureIndexOffset{};
  std::uint32_t textureShift{};
  std::uint32_t textureMask{};

  [[nodiscard]] constexpr bool valid() const noexcept {
    return texturesBeforeCount != 0 && textureEntrySize != 0 && atlasStride != 0 &&
           atlasCount != 0 && textureMask != 0;
  }
};

struct ObjectRenderTarget {
  const char* name{};
  std::string_view pattern{};
  std::uintptr_t referenceRva{};
};
inline constexpr std::size_t kMaxSmoothedObjectRenders = 12;

// The GL backend's draw queue (gl.cmds / gl.n), consumed by DrawFlush_GL.
// Used to pick out the engine's additive (light-emitting) draws.
struct DrawQueueLayout {
  // Offset inside DrawFlush_GL of `cmp [rip + gl.n], r14d`.
  std::size_t countCompareOffset{};
  // gl.cmds starts this many bytes before gl.n.
  std::uintptr_t commandsBeforeCount{};
  std::uint32_t maxCommands{};
  // Command state word: blend enable bit, destination factor field (4 bits),
  // and the factor index that means GL_ONE.
  std::uint32_t blendEnableBit{};
  std::uint32_t blendDstShift{};
  std::uint32_t blendFactorOne{};

  [[nodiscard]] constexpr bool valid() const noexcept {
    return countCompareOffset != 0 && commandsBeforeCount != 0 && maxCommands != 0;
  }
};

struct BuildManifest {
  std::string_view buildId{};
  std::array<std::string_view, 2> supportedProductNames{};
  ExecutableVersion executableVersion{};
  PatternSet patterns{};
  ReferenceRvas referenceRvas{};
  RuntimeOffsets offsets{};
  std::array<BranchInstructionDesc, 11> renderTextureCallsites{};
  // Moving non-creature objects (projectiles, spell effect cells, debris).
  // Unused slots stay empty.
  std::array<ObjectRenderTarget, kMaxSmoothedObjectRenders> smoothedObjectRenders{};
  // Optional; an invalid (empty) layout leaves emissive bloom off.
  DrawQueueLayout drawQueue{};
  // Optional; an invalid (empty) layout leaves sprite upscaling off.
  SpriteAtlasLayout spriteAtlas{};

  [[nodiscard]] constexpr bool validate() const noexcept {
    if (buildId.empty() || supportedProductNames[0].empty() || executableVersion.major == 0 ||
        patterns.loadArea.empty() || patterns.renderTexture.empty()) {
      return false;
    }
    if (!referenceRvas.loadArea || !referenceRvas.renderTexture) {
      return false;
    }
    if (!offsets.vidTileResource || !offsets.tisLinearTilesFlag ||
        !offsets.tisHeaderTileDimension) {
      return false;
    }
    if (!offsets.infGameVisibleArea || !offsets.infGameAreas || !offsets.infGameAreaMaster) {
      return false;
    }

    for (const auto& callsite : renderTextureCallsites) {
      if (!callsite.validate()) {
        return false;
      }
    }

    return true;
  }
};

[[nodiscard]] const BuildManifest& current_manifest() noexcept;

[[nodiscard]] std::optional<std::reference_wrapper<const BuildManifest>> find_manifest(
    std::string_view buildId) noexcept;

[[nodiscard]] std::optional<std::reference_wrapper<const BuildManifest>> find_manifest_for_version(
    std::uint16_t major, std::uint16_t minor, std::uint16_t patch, std::uint16_t revision) noexcept;

// Product names are compared case-insensitively after removing ASCII
// punctuation/spacing. This accepts harmless version-resource punctuation
// differences while rejecting sibling Infinity Engine games.
[[nodiscard]] bool supports_product_name(const BuildManifest& manifest,
                                         std::string_view productName);

// Selects a manifest from the main executable's fixed file version. Unknown
// versions are deliberately unsupported and return nullptr before scanning
// or installing any hooks.
[[nodiscard]] const BuildManifest* detect_manifest(
    ExecutableVersion* detectedVersion = nullptr,
    std::string* detectedProductName = nullptr) noexcept;
}  // namespace iee::game
