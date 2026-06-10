# Enhancement Ideas for InfinityEngine-Enhancer

This document outlines potential rendering enhancements for future development.

## Current Features ✅

### Tile Rendering Enhancement
- **Linear filtering** - Smooths texture edges for upscaled tiles
- **Anisotropic filtering** - Reduces blurriness at distance/angles
- **LOD bias adjustment** - Fine-tune texture sharpness
- **Automatic upscale detection** - Detects 4x upscaled content via UV coordinates
- **Performance optimization** - Thread-local caching, smart hook disabling

### OpenGL Capability Discovery ✅ **COMPLETE**
**Discovered**: BGEE runs in OpenGL 4.6 compatibility mode with full modern features available!

**Context Creation**:
- BGEE uses legacy `wglCreateContext()` (no version specified)
- NVIDIA driver provides OpenGL 4.6.0 compatibility profile by default
- All legacy fixed-function calls work (BGEE's rendering)
- All modern OpenGL 3.3+ features available via `wglGetProcAddress()`

**Verified Available Functions**:
```
✅ Shader Functions:
  - glCreateShader, glCreateProgram, glShaderSource, glCompileShader, glUseProgram
  - Full GLSL 4.60 shader support

✅ Framebuffer Objects (FBO):
  - glGenFramebuffers - Post-processing via render-to-texture

✅ Vertex Buffer Objects (VBO):
  - glGenBuffers - Efficient modern rendering

✅ WGL Extensions:
  - wglCreateContextAttribsARB - Context upgrade capability (not needed)
```

**Key Insight**: **NO CONTEXT UPGRADE NEEDED!** We can use modern shaders, FBOs, and VBOs immediately by loading functions at runtime.

**Implementation Strategy**:
1. Load modern GL functions via `wglGetProcAddress()` on first render
2. Create shader compilation infrastructure
3. Implement FBO-based post-processing pipeline
4. Apply shaders to specific rendering passes (fog, effects, post-process)

**Benefits**:
- ⭐⭐⭐⭐⭐ **Unlimited rendering possibilities** - Full shader support
- ✅ **Zero compatibility risk** - BGEE's code unchanged
- ✅ **No context replacement** - Works with existing setup
- ✅ **Future-proof** - Can use any modern rendering technique

## Potential Future Enhancements

### 1. Animation Frame Interpolation 🎯 HIGH IMPACT
**Goal**: Smooth character/object animations from choppy 15fps to fluid 60fps

**Technical Approach**:
- Detect animation texture sequences (consecutive texture IDs)
- Store previous frame texture data
- Blend between current and previous frame based on sub-frame timing
- Requires frame timing detection and texture caching

**OpenGL Functions Needed**:
```cpp
PFN_glBlendFunc glBlendFunc;           // Already available
// Additional texture storage for previous frames
```

**Research Needed**:
- How does BGEE sequence animation frames?
- Can we detect animation cycles vs static textures?
- Frame timing mechanism in the engine

### 2. Fog-of-War Enhancement ❌ **BLOCKED - FUNCTION TOO SENSITIVE**
**Goal**: Smooth, anti-aliased fog edges instead of pixelated chunks

**BGEE Fog Architecture** ✅ FULLY RESEARCHED:
```cpp
// High-level: CInfinity::RenderFog (sets OpenGL state, iterates tiles)
// - Address: 0x1402A14F0 (RVA: 0x2A14F0)
// - Successfully hookable but NO VISUAL IMPACT (setup only)
DrawPushState();
DrawColor(0xffffffff);                              // White base color
DrawDisable(DRAW_TEXTURE_2D);                      // Pure geometry, no textures
DrawEnable(DRAW_BLEND);
DrawBlendFunc(DRAW_SRC_ALPHA, DRAW_ONE_MINUS_SRC_ALPHA);  // Standard alpha blend
DrawBegin(DRAW_TRIANGLES);
// ... iterate tiles, call BltFogOWar3d per tile

// Low-level: CVisibilityMap::BltFogOWar3d (per 64x64 tile)
// - Address: 0x1402572D1 (RVA: 0x2572D1)
// - CONTROLS VISUAL APPEARANCE but CRASHES on any modification
// - 4 corner fog values (4-bit each: tileNW, tileNE, tileSW, tileSE)
// - 3 fog types: FOGOWARSHADE (black), EXPLORESHADE (gray), CLEARSHADE (clear)
// - Renders 4 triangular sections per tile using DrawVisible()
// - Protected by stack canary (__security_cookie/__security_check_cookie)
```

**Why Fog Enhancement Failed** ❌:

#### **Option 1: Hook `CInfinity::RenderFog` - TESTED ❌**
- ✅ **Hook creation**: Success
- ✅ **Function calls**: Success
- ✅ **Color changes**: Applied successfully
- ❌ **Visual impact**: **ZERO** - function only does setup, no rendering

#### **Option 2: Hook `CVisibilityMap::BltFogOWar3d` - TESTED ❌**
- ✅ **Address resolution**: Success (pattern + fallback RVA)
- ✅ **Function signature**: Correct `void (*)(int, int, void*, void*, int, int, bool)`
- ❌ **Any code additions**: **INSTANT CRASH**
  - Logging variables: CRASH
  - OpenGL state changes: CRASH
  - TILE_CODE data access: CRASH
  - Even minimal passthrough with safety checks: CRASH
- 🛡️ **Root cause**: Stack canary protection + extreme sensitivity

**Technical Analysis**:
```asm
; BltFogOWar3d function start (verified bytes):
1402572d1: 48 81 ec 90 00 00 00    SUB RSP, 0x90       ; Large stack frame
1402572d8: 48 33 c4                XOR RAX, RSP        ; Stack canary setup
1402572db: 48 89 84 24 88 00 00 00 MOV [RSP+0x88], RAX ; Store canary
...
140257494: e8 27 f3 29 00          CALL __security_check_cookie ; Canary validation
```

The function has:
- **144-byte stack frame** with complex local variables
- **Stack canary protection** that validates on return
- **Extremely sensitive timing** - any additional stack operations cause crashes

**Alternative Approaches Evaluated**:
1. **Draw function hooking** ❌ - Too global (used throughout entire render pipeline)
2. **Global variable modification** ❌ - No clear access to FOGOWARSHADE/EXPLORESHADE globals
3. **OpenGL context changes** ❌ - Crashes even with simple glEnable calls
4. **Post-processing** ❌ - Would require external framework (ENB/ReShade)

**Final Verdict**:
**FOG ENHANCEMENT IS NOT FEASIBLE** with current hooking approach. The fog rendering system is too tightly coupled and protected to modify safely without major engine changes or external post-processing tools.

**However** - **MASSIVE VISUAL IMPACT POTENTIAL** ⭐⭐⭐⭐⭐:
Looking at current fog-of-war rendering, the chunky, pixelated fog edges are **extremely noticeable** and make the game look dated. Smooth, anti-aliased fog-of-war would provide one of the most dramatic visual improvements possible - far more impactful than font or even spell enhancements.

**BREAKTHROUGH: DrawVisible Function Discovery** ✅ **NEW APPROACH**:

**Complete Fog Rendering Pipeline** (Re-researched):
```cpp
// 1. CInfinity::RenderFog - Setup OpenGL state, iterate tiles
DrawPushState();
DrawColor(0xffffffff);
DrawDisable(DRAW_TEXTURE_2D);                    // No textures - pure geometry
DrawEnable(DRAW_BLEND);
DrawBlendFunc(DRAW_SRC_ALPHA, DRAW_ONE_MINUS_SRC_ALPHA);
DrawBegin(DRAW_TRIANGLES);

// 2. CVisibilityMap::BltFogOWar3d - Per-tile fog rendering (SENSITIVE FUNCTION)
//    - Processes 4 corner fog values (tileNW, tileNE, tileSW, tileSE)
//    - Calls DrawVisible() for each fog triangle section

// 3. DrawVisible - ACTUAL FOG QUAD RENDERER (NEW TARGET)
void DrawVisible(CRect* rect, uint32_t fogColor, uint32_t clearColor, uint8_t alpha);
```

**Critical Discovery - DrawVisible Function**:
```cpp
// DrawVisible renders individual fog quad sections using immediate mode
// switch(alpha & 0xf) determines fog pattern (16 different fog transition patterns)
// For each pattern, calls:
DrawColor(fogColor or clearColor);  // Set fog/clear color
DrawVertex(x, y);                   // Draw triangle vertices
```

**Why DrawVisible is PERFECT for Enhancement**:
✅ **Simple function** - Only calls `DrawColor` and `DrawVertex` (safe Draw* functions)
✅ **No stack protection** - No security canaries or complex state management
✅ **Pure rendering** - Just OpenGL drawing operations, no game logic
✅ **Same pattern as successful enhancements** - Like `drawLetter` for fonts

**Fog Enhancement Strategy - Hook DrawVisible**:
```cpp
using Fn_DrawVisible = void (*)(CRect*, uint32_t, uint32_t, uint8_t);
static core::Hook<Fn_DrawVisible> H_DrawVisible;

static void Detour_DrawVisible(CRect* rect, uint32_t fogColor, uint32_t clearColor, uint8_t alpha) {
    auto& ctx = *g_ctx;

    // Enable anti-aliasing for smooth fog edges
    if (ctx.draw.DrawEnable) {
        ctx.draw.DrawEnable(DRAW_POLYGON_SMOOTH);  // Smooth quad edges
    }

    // Enhanced blending for better fog transitions (optional)
    // Could modify blending modes here for different fog effects

    // Call original fog quad rendering
    H_DrawVisible.original()(rect, fogColor, clearColor, alpha);

    // Restore state
    if (ctx.draw.DrawDisable) {
        ctx.draw.DrawDisable(DRAW_POLYGON_SMOOTH);
    }
}
```

**Fog Enhancement Features Possible**:

**✅ 1. Anti-aliased Fog Edges** (`GL_POLYGON_SMOOTH`):
- **Current**: Sharp, jagged fog boundaries between tiles
- **Enhanced**: Smooth, anti-aliased fog quad edges
- **Impact**: Significantly reduces chunky fog appearance

**✅ 2. Enhanced Fog Blending**:
- **Current**: Standard alpha blending
- **Enhanced**: Custom blending modes for different fog effects
- **Impact**: Smoother fog transitions, better visual quality

**✅ 3. Fog Color/Contrast Enhancement**:
- **Current**: Fixed fog colors (FOGOWARSHADE, EXPLORESHADE, CLEARSHADE)
- **Enhanced**: Modify fog opacity, contrast, or add subtle color tinting
- **Impact**: Better fog visibility and atmospheric effects

**✅ 4. Advanced Fog Rendering (Future)**:
- **Gradient fog quads** - Replace solid color quads with gradients
- **Sub-pixel fog** - Render fog at higher resolution than 64x64 tiles
- **Distance-based fog falloff** - Smooth fog based on distance from edges

**Implementation Complexity**:
- **Basic enhancements**: ⭐ **Very Easy** - Just enable GL_POLYGON_SMOOTH
- **Advanced rendering**: ⭐⭐⭐ **Medium** - Custom fog quad generation

**Risk vs Reward Assessment (Updated)**:
- 🔥 **Highest possible visual impact** - Anti-aliased fog would dramatically improve appearance
- ✅ **Low technical risk** - Safe function to hook, same pattern as successful enhancements
- ⚖️ **Verdict**: **PROCEED WITH IMPLEMENTATION** - Start with basic anti-aliasing, expand from there

**Implementation Status** ✅ **COMPLETE**:

**✅ Completed:**
- Hook infrastructure added to `hooks.cpp`
- Configuration options added to `config.h` (`enableFogAntialiasing = true`)
- DrawVisible function signature and detour implemented
- Graceful fallback when DrawEnable is unavailable
- Pattern signature defined: `"83 E0 0F 41 FF E1 E8"`
- DrawVisible pattern scanning implemented in `game_addrs.cpp`
- Hook installation added to `install_all()` function
- DrawEnable offset resolution added to `renderer.cpp`

**Current Behavior:**
- Fog enhancement is **ACTIVE** when `enableFogAntialiasing = true` in config
- Enables GL_POLYGON_SMOOTH for anti-aliased fog edges
- Gracefully degrades if DrawEnable is unavailable (logs warning once)
- Ready for in-game testing

### 3. Text Rendering Enhancement ✅ **NEW TARGET - HIGH PRIORITY**
**Goal**: Crisp, anti-aliased text with better readability

**BGEE Text Architecture** 📝 **FULLY RESEARCHED**:
```cpp
// CVidFont structure (40 bytes):
struct CVidFont {
    CResHelper<CResFont,1034> baseclass_0;  // 0x0  (16 bytes)
    CVidCellFont* vidCellFont;               // 0x10 (8 bytes)
    unsigned int foreground;                 // 0x18 (4 bytes)
    unsigned int tintcolor;                  // 0x1C (4 bytes)
    int pointSize;                           // 0x20 (4 bytes)
    int zoom;                                // 0x24 (4 bytes)
};

// CResFont structure (96 bytes):
struct CResFont {
    CRes baseclass_0;                        // 0x0  (88 bytes)
    void* font;                              // 0x58 (8 bytes) -> font_t*
};

// Main text rendering function:
int CVidFont::RenderTextWrap(char* text, int x, int y, int w, int h, CRect* clipRect,
                            int param7, int param8, bool shadow, bool background);

// Low-level rendering pipeline:
int fontDraw(char* text, SDL_Rect* rect, SDL_Rect* clipRect, int flags1, int flags2,
            font_t* font, int param7, int param8, int param9, int param10, bool shadow,
            int fontSize, bool background, int param14, bool param15, float param16,
            float param17, bool param18);
```

**Text Rendering Pipeline Analysis** ✅ **COMPLETE**:
1. `CVidFont::RenderTextWrap` - Sets up blending (`DrawBlendFunc`) and colors
2. `fontDraw` - Handles text layout, wrapping, shadow/background effects
3. `drawLetters` - Iterates through characters, handles color commands, calls `drawLetter`
4. **`drawLetter` - TEXTURE BINDING FUNCTION** - renders individual glyphs using font textures

**Critical Discovery**:
```cpp
// drawLetter is the equivalent of RenderTexture for fonts!
void drawLetter(font_t* font, glyphmap_t* glyphMap, letter_t* letter,
               int x, int y, SDL_Rect* clipRect, float param7, float param8);

// Font texture atlas management:
glyphmap_t* fontGetGlyphMap(font_t* font, int fontSize);  // Gets texture atlas
void fontAddGlyph(font_t* font, int glyphIndex, int fontSize);  // Adds glyphs to atlas
```

**Font Texture Architecture**:
- **`glyphmap_t*`** - Font texture atlas containing all glyph bitmaps
- **`font_t*`** - Font data structure with metrics and glyph information
- **`drawLetter()`** calls `bindtexture()` - this is where font textures get bound!

**Enhancement Strategy - Hook drawLetter (Optimal)**:
✅ **Direct texture access** - `drawLetter` binds font textures like `RenderTexture` binds tiles
✅ **Simple function signature** - No complex stack manipulation
✅ **Same enhancement pattern** - Apply `game::ensure_texture_params()` to font textures

**Implementation Approach**:
```cpp
// Hook drawLetter - the font texture rendering function
using Fn_DrawLetter = void (*)(font_t*, glyphmap_t*, letter_t*, int, int, SDL_Rect*, float, float);
static core::Hook<Fn_DrawLetter> H_DrawLetter;

static void Detour_DrawLetter(font_t* font, glyphmap_t* glyphMap, letter_t* letter,
                             int x, int y, SDL_Rect* clipRect, float param7, float param8) {
    auto& ctx = *g_ctx;

    // Get font texture ID from glyphmap_t structure
    int fontTexId = glyphMap->textureId; // Need glyphmap_t structure from Ghidra

    // Enhance font texture parameters (identical to tile enhancement)
    if (game::ensure_texture_params(ctx.cfg, ctx.draw, fontTexId)) {
        LOG_DEBUG("Enhanced font texture {}", fontTexId);
    }

    H_DrawLetter.original()(font, glyphMap, letter, x, y, clipRect, param7, param8);
}
```

**Font Texture Enhancement Features Available**:

Following the exact same pattern as successful tile enhancement, we can apply these texture improvements to font glyphs:

**✅ 1. Linear Filtering** (`GL_TEXTURE_MIN_FILTER`, `GL_TEXTURE_MAG_FILTER`):
- **Current**: Pixelated, blocky text at different zoom levels
- **Enhanced**: Smooth, crisp text scaling without jagged edges
- **Impact**: Dramatically improves text readability, especially for UI scaling

**✅ 2. Anisotropic Filtering** (`GL_TEXTURE_MAX_ANISOTROPY_EXT`):
- **Current**: Blurry text when viewed at angles (dialog boxes, angled UI)
- **Enhanced**: Sharp text maintained at all viewing angles
- **Impact**: Better text clarity across different UI orientations

**✅ 3. LOD Bias Adjustment** (`GL_TEXTURE_LOD_BIAS`):
- **Current**: Standard texture sharpness
- **Enhanced**: Fine-tuned sharpness for optimal text contrast
- **Impact**: Crisper, more defined character edges

**✅ 4. Enhanced Texture Wrapping** (`GL_TEXTURE_WRAP_S`, `GL_TEXTURE_WRAP_T`):
- **Current**: Default texture clamping
- **Enhanced**: Optimized wrapping for glyph atlas rendering
- **Impact**: Better glyph boundary handling, reduced artifacts

**Font Enhancement vs. Tile Enhancement Comparison**:
```cpp
// IDENTICAL enhancement pattern:
// Tiles: game::ensure_texture_params(ctx.cfg, ctx.draw, tileTexId)
// Fonts: game::ensure_texture_params(ctx.cfg, ctx.draw, fontTexId)
```

**Visual Impact Examples**:
- **Dialogue text**: Smooth, crisp character speech
- **UI buttons**: Sharp, readable button labels
- **Combat log**: Clear, professional-looking combat text
- **Tooltips**: Enhanced tooltip readability
- **Menu text**: Smooth main menu and settings text

**Configuration Options** (via INI file):
- `enableFontAnisotropicFiltering = true/false`
- `maxFontAnisotropy = 8.0` (same as tiles)
- `fontLodBias = -0.25` (same as tiles, or custom value)

**Font Enhancement Final Assessment** ❌ **LOW PRIORITY**:

**Why Font Enhancement is Less Valuable**:
- ❌ **Users already have font replacement** - Can use modern TTF fonts via BGEE.lua override
- ❌ **Modern fonts > texture filtering** - High-quality TTF fonts provide better results than enhancing bitmap fonts
- ❌ **Easy user alternative exists** - Font replacement is simpler than code modification
- ❌ **Minimal unique value** - Our enhancement would only provide basic texture filtering

**What Font Hook Could Uniquely Add**:
- ✅ **Dynamic effects** - Real-time drop shadows, outlines, glow effects (complex implementation)
- ✅ **Context-aware rendering** - Special effects for spell text, dialogue, etc. (medium complexity)
- ✅ **Advanced rendering** - Subpixel rendering, adaptive anti-aliasing (high complexity)

**Conclusion**: Font enhancement provides minimal benefit since users can already replace fonts with higher-quality alternatives. The unique enhancements we could provide require significant development effort for questionable visual improvement.

### 4. Spell Effect Enhancement 🎯 HIGH VISUAL IMPACT
**Goal**: More dramatic and visually appealing spell effects

**BGEE Spell Effect Architecture** ✨:
```cpp
// Spell effect rendering functions:
?Render@CGameAnimationTypeEffect@@UEAAXPEAVCInfinity@@PEAVCVidMode@@...
?Render@CProjectileBAM@@UEAAXPEAVCGameArea@@PEAVCVidMode@@@Z
?Render@CProjectileChain@@UEAAXPEAVCGameArea@@PEAVCVidMode@@@Z
?Render@CProjectileScorcher@@UEAAXPEAVCGameArea@@PEAVCVidMode@@@Z
?Render@CProjectileSkyStrike@@UEAAXPEAVCGameArea@@PEAVCVidMode@@@Z
?Render@CVisualEffect@@UEAAXPEAVCGameArea@@PEAVCVidMode@@@Z
?RenderLightning@CInfinity@@QEAAHAEAVCRect@@HHHHKKK@Z
```

**Enhancement Opportunities**:
1. **Enhanced blending modes** based on effect type:
   - **Fire spells**: Additive blending (GL_SRC_ALPHA, GL_ONE)
   - **Ice spells**: Screen blending for crystalline effects
   - **Lightning**: Additive with bloom-like effects
   - **Shadow**: Multiplicative darkening
2. **Anti-aliased spell particles** with GL_POLYGON_SMOOTH
3. **Enhanced glow effects** for magical auras

**Implementation Priority**:
- Start with **CVisualEffect::Render** (general spell effects)
- Then **RenderLightning** (dramatic lightning enhancement)
- Finally projectile-specific enhancements

### 5. Weather Effect Enhancement 🌧️ **MEDIUM IMPACT**
**Goal**: Enhanced atmospheric weather effects

**BGEE Weather Architecture**:
```cpp
?Render@CRainStorm@@QEAAXPEAVCVidMode@@AEBVCRect@@K@Z
?Render@CSnowStorm@@QEAAXPEAVCVidMode@@AEBVCRect@@K@Z
```

**Enhancement Opportunities**:
1. **Particle smoothing** for rain/snow
2. **Enhanced alpha blending** for atmospheric effects
3. **Better weather opacity** and layering

### 4. Post-Processing Effects 🎯 LOW-MEDIUM IMPACT
**Goal**: Screen-space enhancements for overall visual quality

**Potential Effects**:
- Color correction/gamma adjustment
- Subtle depth-of-field blur for background elements
- Sharpening filter for crisp details
- Screen-space ambient occlusion (very advanced)

**Technical Approach**:
- Render to framebuffer object (FBO)
- Apply shader-based post-processing
- Present final result to screen

**OpenGL Functions Needed**:
```cpp
PFN_glGenFramebuffers glGenFramebuffers;
PFN_glBindFramebuffer glBindFramebuffer;
PFN_glFramebufferTexture2D glFramebufferTexture2D;
// Fragment shader support
```

**Research Needed**:
- BGEE's final render target
- Performance impact of FBO usage
- Shader compilation and management

## Architecture Limitations

### What We **Cannot** Enhance
- **Lighting system** - Pre-baked into tiles, no real-time lighting
- **Water reflections** - Just animated tiles, no geometry for true reflections
- **3D shadows** - Characters are sprites, not 3D models
- **Dynamic weather** - Weather effects are pre-rendered animations

### Why Infinity Engine is Limiting
- **2D sprite-based** - Everything is pre-rendered, no 3D geometry
- **Tile-based rendering** - Environments are static image tiles
- **Fixed pipeline** - Limited hooks into rendering without major engine changes

## New High-Priority Enhancement Opportunities 🎯

Based on systematic analysis of BGEE's rendering pipeline, the following enhancements provide tangible visual improvements with proven safe hooking techniques:

### 6. Character/Sprite Rendering Enhancement ⭐⭐⭐⭐⭐ **HIGHEST PRIORITY**
**Goal**: Enhanced texture filtering for character sprites, creatures, and animations

**Why This Matters**:
- Characters/creatures are the primary visual focus during gameplay (90% of player attention)
- Currently rendered with basic texture filtering, causing pixelation during camera zoom
- Uses texture-based rendering (safe to hook, same pattern as successful tile enhancement)

**BGEE Character Rendering Architecture**:
```cpp
// Main sprite rendering:
?Render@CGameSprite@@UEAAXPEAVCGameArea@@PEAVCVidMode@@@Z

// Character-specific animation rendering:
?Render@CGameAnimationTypeCharacter@@UEAAXPEAVCInfinity@@PEAVCVidMode@@...
?Render@CGameAnimationTypeCharacterOld@@UEAAXPEAVCInfinity@@PEAVCVidMode@@...

// Monster/creature animation rendering:
?Render@CGameAnimationTypeMonster@@UEAAXPEAVCInfinity@@PEAVCVidMode@@...
?Render@CGameAnimationTypeMonsterLarge@@UEAAXPEAVCInfinity@@PEAVCVidMode@@...
?Render@CGameAnimationTypeMonsterLayered@@UEAAXPEAVCInfinity@@PEAVCVidMode@@...
?Render@CGameAnimationTypeMonsterMulti@@UEAAXPEAVCInfinity@@PEAVCVidMode@@...
// ... (12+ monster animation variants)

// Related rendering functions:
?RenderSpriteCover@CGameSprite@@QEAAX...  // Character shadows
?RenderSpriteEffect@CGameSprite@@QEAAX... // Visual effects on sprites
?RenderMirrorImage@CGameSprite@@QEAAX...  // Mirror image spell effects
```

**Enhancement Strategy - Hook CGameSprite::Render**:
```cpp
using Fn_RenderSprite = void (*)(void* thisPtr, void* gameArea, void* vidMode);
static core::Hook<Fn_RenderSprite> H_RenderSprite;

static void Detour_RenderSprite(void* thisPtr, void* gameArea, void* vidMode) {
    auto& ctx = *g_ctx;

    // Note: Character sprites are rendered via CVidCell::RenderTexture internally
    // Our existing RenderTexture hook already handles texture binding
    // This hook would allow sprite-specific enhancements (selection glow, outlines, etc.)

    H_RenderSprite.original()(thisPtr, gameArea, vidMode);
}
```

**Tangible Enhancements Available**:

**✅ 1. Linear/Anisotropic Filtering** (via existing RenderTexture hook):
- **Current**: Pixelated character sprites, especially when zoomed
- **Enhanced**: Smooth, crisp character rendering at all zoom levels
- **Impact**: Dramatically improves character visual quality
- **Implementation**: Character sprites already benefit from tile enhancement if they use RenderTexture

**✅ 2. Enhanced Character Shadows**:
- **Current**: Basic shadow rendering via `RenderSpriteCover`
- **Enhanced**: Softer shadow edges with better alpha blending
- **Impact**: More professional-looking character shadows

**✅ 3. Selection Glow Enhancement**:
- **Current**: Basic selection circle
- **Enhanced**: Subtle glow effect on selected characters
- **Impact**: Better visual feedback for character selection

**✅ 4. Mirror Image Enhancement**:
- **Current**: Standard mirror image rendering
- **Enhanced**: Ethereal/ghostly effect with custom blending
- **Impact**: More impressive visual spell effects

**OpenGL Compatibility Note**:
All proposed enhancements use OpenGL 1.x functions available in BGEE:
- `GL_LINEAR`, `GL_LINEAR_MIPMAP_LINEAR` (texture filtering)
- `GL_TEXTURE_MAX_ANISOTROPY_EXT` (anisotropic filtering)
- `GL_SRC_ALPHA`, `GL_ONE` (additive blending)
- `GL_POLYGON_SMOOTH`, `GL_LINE_SMOOTH` (anti-aliasing)

**Implementation Complexity**: ⭐ **Very Low**
- Character sprites likely already use `CVidCell::RenderTexture` (already hooked)
- No additional hooks needed for basic filtering enhancement
- Advanced effects (glow, outlines) require moderate work

**Risk Assessment**: ✅ **Very Low**
- Texture-based rendering (same as tiles)
- Proven safe hooking pattern
- Graceful degradation if hooks fail

**Visual Impact**: ⭐⭐⭐⭐⭐ **Maximum**
- Characters are the primary visual focus
- Improvement visible in every scene
- Most noticeable enhancement after tiles

---

### 7. UI/Icon Rendering Enhancement ⭐⭐⭐⭐ **HIGH PRIORITY - QUICK WIN**
**Goal**: Crisp, sharp rendering for item icons, spell icons, and UI elements

**Why This Matters**:
- UI elements are constantly visible during gameplay
- Item/spell icons are low-resolution and pixelated
- Simple, isolated function (very safe to hook)

**BGEE UI Rendering Architecture**:
```cpp
// Icon rendering (items, spells, abilities):
?RenderIcon@CIcon@@QEAAXAEBVCPoint@@AEBVCSize@@AEBVCRect@@AEBVCResRef@@KGHGH2H@Z

// Bitmap rendering (UI backgrounds, buttons):
?Render@CVidBitmap@@QEAAXHHAEBVCRect@@K@Z
?RenderScaled@CVidBitmap@@QEAAXAEBVCRect@@0K@Z

// Tooltip rendering:
?Render@CInfToolTip@@UEAAHHHAEBVCRect@@PEAVCVidPoly@@HKH@Z

// Portrait rendering:
?RenderPortrait@CGameSprite@@QEAAXAEBVCPoint@@AEBVCSize@@HHHAEBVCRect@@H@Z
?RenderPortrait@CInfGame@@QEAAXKAEBVCPoint@@AEBVCSize@@HHHAEBVCRect@@@Z
```

**Enhancement Strategy - Hook RenderIcon**:
```cpp
using Fn_RenderIcon = void (*)(void* thisPtr, void* point, void* size, void* rect,
                                void* resRef, uint32_t param1, uint16_t param2,
                                uint16_t param3, uint16_t param4, uint16_t param5,
                                uint32_t param6, int param7);
static core::Hook<Fn_RenderIcon> H_RenderIcon;

static void Detour_RenderIcon(void* thisPtr, void* point, void* size, void* rect,
                              void* resRef, uint32_t param1, uint16_t param2,
                              uint16_t param3, uint16_t param4, uint16_t param5,
                              uint32_t param6, int param7) {
    auto& ctx = *g_ctx;

    // Icons are rendered via texture binding (CVidCell::RenderTexture)
    // Our existing hook already handles texture filtering
    // This hook allows icon-specific enhancements (glow for magical items, etc.)

    H_RenderIcon.original()(thisPtr, point, size, rect, resRef, param1,
                           param2, param3, param4, param5, param6, param7);
}
```

**Tangible Enhancements Available**:

**✅ 1. Icon Texture Filtering**:
- **Current**: Pixelated, blocky item/spell icons
- **Enhanced**: Sharp, crisp icons with smooth edges
- **Impact**: Professional UI appearance
- **Implementation**: Already works if icons use RenderTexture

**✅ 2. Enhanced Tooltip Backgrounds**:
- **Current**: Standard tooltip rendering
- **Enhanced**: Smoother tooltip backgrounds with better alpha blending
- **Impact**: More polished UI presentation

**✅ 3. Portrait Enhancement**:
- **Current**: Pixelated character portraits
- **Enhanced**: Smooth, high-quality portrait rendering
- **Impact**: Better character portrait presentation

**Implementation Complexity**: ⭐ **Very Low**
- Likely already enhanced via RenderTexture hook
- Minimal additional work required

**Risk Assessment**: ✅ **Very Low**
- Isolated, texture-based rendering
- Same proven pattern as tiles

**Visual Impact**: ⭐⭐⭐⭐ **High**
- Constantly visible during gameplay
- Affects inventory, spellbook, character sheets
- Professional polish improvement

---

### 8. Selection Circle/Marker Enhancement ⭐⭐⭐ **MEDIUM PRIORITY - EASY WIN**
**Goal**: Smooth, anti-aliased selection circles and target markers

**Why This Matters**:
- Selection circles are drawn constantly during gameplay
- Currently rendered with jagged, aliased edges
- Simple geometry-based rendering (safe for GL_LINE_SMOOTH)

**BGEE Marker Rendering Architecture**:
```cpp
// Selection circle rendering:
?Render@CMarker@@QEAAXPEAVCVidMode@@PEAVCGameSprite@@@Z
?Render@CMarker@@QEAAXPEAVCVidMode@@PEAVCInfinity@@AEBVCPoint@@JJ@Z

// Object highlight markers:
?Render@CObjectMarker@@UEAAXPEAVCGameArea@@PEAVCVidMode@@@Z

// Search/highlight rendering:
?RenderHighlight@@YAXUSDL_Rect@@0@Z
```

**Enhancement Strategy - Hook CMarker::Render**:
```cpp
using Fn_RenderMarker = void (*)(void* thisPtr, void* vidMode, void* sprite);
static core::Hook<Fn_RenderMarker> H_RenderMarker;

static void Detour_RenderMarker(void* thisPtr, void* vidMode, void* sprite) {
    auto& ctx = *g_ctx;

    // Enable line smoothing for anti-aliased selection circles
    if (ctx.draw.DrawEnable) {
        ctx.draw.DrawEnable(DRAW_LINE_SMOOTH);
    }

    H_RenderMarker.original()(thisPtr, vidMode, sprite);

    // Restore state
    if (ctx.draw.DrawDisable) {
        ctx.draw.DrawDisable(DRAW_LINE_SMOOTH);
    }
}
```

**Tangible Enhancements Available**:

**✅ 1. Anti-aliased Selection Circles** (`GL_LINE_SMOOTH`):
- **Current**: Jagged, pixelated selection circle edges
- **Enhanced**: Smooth, professional-looking selection circles
- **Impact**: Noticeable quality improvement during gameplay

**✅ 2. Enhanced Highlight Effects**:
- **Current**: Basic object highlighting
- **Enhanced**: Smooth, anti-aliased highlight rendering
- **Impact**: Better visual feedback for interactive objects

**Implementation Complexity**: ⭐ **Very Low**
- Simple GL state change (like fog enhancement)
- Proven safe pattern

**Risk Assessment**: ✅ **Very Low**
- Geometry-based rendering (no complex state)
- Same pattern as DrawVisible fog enhancement

**Visual Impact**: ⭐⭐⭐ **Medium-High**
- Visible whenever characters are selected
- Subtle but noticeable quality improvement

---

## Engine-Side Enhancements (ReShade Cannot Do) 🎯

**Philosophy**: Focus on state-aware rendering that requires engine context. Anything global (LUT, sharpening, vignette) can be done with ReShade and should be left to user preference.

---

### **Priority 1: Soft Fog of War** ⭐⭐⭐⭐⭐ **HIGHEST IMPACT**
**Why ReShade can't**: No access to engine visibility/exploration masks

**What we add**:
- Feathered visible edge (blue-noise dither to kill banding)
- Dimmed "explored" areas
- Fully occluded "unexplored" areas
- Outdoor/interior presets

**Hook point**: `DrawVisible` (fog quad renderer) + late composite before UI

**Implementation**:
1. Render fog to R8 mask FBO (per-tile visibility state)
2. Apply 2-pass separable blur (soft edges)
3. Composite: `unexplored=black, explored=dim, visible=clear`
4. Blue-noise dither to eliminate banding

**Cost/Impact**: Medium effort / ★★★★★ Visual Impact
**Status**: ✅ Hook location identified, ready to implement

---

### **Priority 2: Sprite-Only Refinement** ⭐⭐⭐⭐⭐ **HUGE WIN**
**Why ReShade can't**: Cannot isolate sprite draws from background/UI

**What we add**:
- Better sampling (Catmull-Rom/EASU-lite) when sprites are scaled
- Premultiplied-alpha fix (no halos on outlines)
- Optional tiny edge AA on BAM silhouettes

**Hook point**: Sprite draw routine (BAM rendering path)

**Implementation**:
1. Detect sprite magnification scale
2. Apply Catmull-Rom or EASU upsampling kernel
3. Convert to premultiplied alpha math for proper blending
4. Optional edge detection + 2-tap AA on silhouettes

**Cost/Impact**: Medium effort / ★★★★★ Visual Impact (characters look instantly cleaner)
**Status**: 🔜 Hook point: `CGameAnimationType*::Render` or `RenderPVR`

---

### **Priority 3: VFX-Only Polish** ⭐⭐⭐⭐ **SPELLS POP**
**Why ReShade can't**: No notion of "this quad is a fireball/glow"

**What we add**:
- Thresholded bloom only on spell quads
- Mild heat-haze for fire/steam (noise-warped UVs)
- Soft additive blend respecting premultiplied alpha

**Hook point**: VFX draw path (`CVisualEffect::Render`, `RenderLightning`)

**Implementation**:
1. Detect VFX rendering (via shader tone or draw flags)
2. Apply localized bloom shader (threshold + Gaussian blur)
3. Add heat distortion (UV offset based on noise texture)
4. Use proper additive blending (GL_SRC_ALPHA, GL_ONE)

**Cost/Impact**: Medium effort / ★★★★☆ Visual Impact
**Status**: 🔜 Hook points identified in function list

---

### **Priority 4: Night Emissives** ⭐⭐⭐ **ATMOSPHERE**
**Why ReShade can't**: Can't target "window pixels" in tile textures

**What we add**:
- Boost warm high-luma texels (windows/lamps) in night areas
- Subtle halo (Gaussian or signed-distance falloff)
- Optional flicker noise

**Hook point**: Tile path (existing RenderTexture) with area mode detection

**Implementation**:
1. Detect night areas (area resref ends with 'N')
2. In fragment shader: detect high-luma warm pixels
3. Add glow kernel around emissive pixels
4. Optional: modulate with noise texture for flicker

**Cost/Impact**: Low effort / ★★★☆☆ Visual Impact (big atmosphere boost)
**Status**: 🔜 Can integrate into existing tile shader

---

### **Priority 5: Water Polish** ⭐⭐⭐ **RIVERS LIVE**
**Why ReShade can't**: Needs to detect water quads only

**What we add**:
- Lightweight normal-mapped sparkle + flow
- Specular + Fresnel tint
- Optional reflection tint (no true reflections)

**Hook point**: Tile draw (detect water via tileset/texture ID)

**Implementation**:
1. Tag water tiles (via texture ID range or tileset metadata)
2. Apply scrolling normal map
3. Calculate simple Fresnel effect
4. Add specular highlights

**Cost/Impact**: Medium effort / ★★★☆☆ Visual Impact
**Status**: 🔜 Requires water tile detection logic

---

### **Priority 6: Corner Ambient Occlusion** ⭐⭐ **INTERIOR DEPTH**
**Why ReShade can't**: Needs tile-space derivatives

**What we add**:
- Cheap "corner darken" using 4-8 neighbor samples in atlas space
- Masks baseboard/light-leak look

**Hook point**: Tile shader (existing RenderTexture)

**Implementation**:
1. Sample neighboring tiles in atlas space
2. Detect corners (texture derivative discontinuities)
3. Darken based on corner detection
4. Area-specific intensity presets

**Cost/Impact**: Low effort / ★★☆☆☆ Visual Impact (subtle depth)
**Status**: 🔜 Can integrate into tile shader

---

### **Priority 7: Shadow Blobs for Actors** ⭐⭐ **GROUNDING**
**Why ReShade can't**: Needs actor positions and layer order

**What we add**:
- Soft blob under sprite feet with penumbra
- Clamp by walkable mask to avoid walls

**Hook point**: Before sprite draw

**Implementation**:
1. Get sprite world position
2. Render soft circular shadow quad under feet
3. Sample walkable map to clip shadows at walls
4. Fade based on lighting/time of day

**Cost/Impact**: Low effort / ★★☆☆☆ Visual Impact
**Status**: 🔜 Requires sprite position access

---

### **Priority 8: Area-Aware Color Grade** ⭐⭐ **AUTO AMBIANCE**
**Why ReShade can't**: Can't auto-switch per area or avoid UI

**What we add**:
- Small param grade (exposure/contrast/sat/temp) or 16× LUT
- Before UI rendering, driven by area resref (…N=night, …XX=interior)

**Hook point**: Tile shader uniform binding (LoadArea hook)

**Implementation**:
1. Detect area type from resref
2. Load appropriate color grading params
3. Apply before UI rendering
4. User-configurable via INI

**Cost/Impact**: Low effort / ★★☆☆☆ Visual Impact
**Status**: 🔜 Easy addition to existing area detection

---

### **Priority 9: Gamma-Correct Blending** ⭐⭐ **CLEANER EVERYWHERE**
**Why ReShade can't**: Happens after engine's blends already occurred

**What we add**:
- For fog/sprites/VFX: linearize → blend → re-encode sRGB
- Fixes washed edges, improves stacking of glows/fog

**Hook point**: All custom composite passes

**Implementation**:
```glsl
vec3 linear = pow(color.rgb, vec3(2.2));  // Linearize
// ... blend operations ...
vec3 srgb = pow(linear, vec3(1.0/2.2));   // Re-encode
```

**Cost/Impact**: Minimal effort / ★★☆☆☆ Visual Impact (subtle but correct)
**Status**: 🔜 Apply to all shader paths

---

### **Priority 10: Selection Circle AA** ⭐⭐ **POLISH**
**Why ReShade can't**: Can't target selection quads

**What we add**:
- Crisp AA on circles
- Subtle glow on hover/selection without touching UI text

**Hook point**: `CMarker::Render`

**Implementation**:
```cpp
glEnable(GL_LINE_SMOOTH);
glLineWidth(2.0f);
// Draw selection circle
glDisable(GL_LINE_SMOOTH);
```

**Cost/Impact**: Minimal effort / ★★☆☆☆ Visual Impact
**Status**: ✅ Already documented, trivial to implement

---

## Implementation Roadmap (Prioritized by Impact/Effort)

### **Phase 1: Foundation** (Week 1-2)
1. ✅ OpenGL function loading infrastructure
2. ✅ Shader compilation system
3. ✅ FBO management system
4. 🔜 State guard (save/restore GL state)

### **Phase 2: High-Impact Wins** (Week 3-4)
1. **Soft Fog of War** (#1) - Immediate "modern" feel
2. **Sprite Refinement** (#2) - Characters look clean
3. **VFX Polish** (#3) - Spells pop

### **Phase 3: Atmosphere** (Week 5-6)
4. **Night Emissives** (#4) - Windows glow
5. **Water Polish** (#5) - Rivers sparkle
6. **Area-Aware Grade** (#8) - Auto ambiance

### **Phase 4: Polish** (Week 7+)
7. **Corner AO** (#6) - Interior depth
8. **Shadow Blobs** (#7) - Sprite grounding
9. **Gamma-Correct Blending** (#9) - Mathematical correctness
10. **Selection Circle AA** (#10) - Final touches

---

## What This Achieves Over Stock BGEE

✨ **Edges become soft & light-aware** (fog, windows, water) instead of hard rectangles
✨ **Characters de-halo** (premultiplied alpha + proper sampling) and pop (local bloom)
✨ **Night scenes breathe** (emissives + AO) without changing art direction
✨ **Zero UI collateral** - All changes happen on correct layers
✨ **Unmistakable quality jump** - Things ReShade simply cannot do

---

## ReShade vs Engine-Side Division

**Leave to ReShade** (user preference):
- Global sharpening/blur
- Full-screen LUT/tone mapping
- Vignette, film grain
- Chromatic aberration

**Keep in Enhancer** (requires engine state):
- Fog visibility masks
- Sprite/VFX isolation
- Area-aware effects
- Layer-specific processing
- Correct blending math

This division ensures maximum user flexibility while delivering unique, engine-aware quality that no external tool can match.

## Research Tasks

### Animation System Analysis
- [ ] Identify animation texture sequences
- [ ] Understand frame timing mechanism
- [ ] Determine texture storage requirements for interpolation

### ~~Fog Rendering Analysis~~ ❌ **COMPLETED - BLOCKED**
- [x] **Locate fog rendering code paths** - Found and analyzed
- [x] **Identify fog texture characteristics** - Pure geometry, no textures
- [x] **Test blending mode improvements** - Crashes on any modification
- **VERDICT**: Fog enhancement not feasible with current approach

### Text Rendering Analysis ✅ **PRIORITY TARGET**
- [ ] Identify CVidFont::RenderTextWrap pattern signature
- [ ] Test basic GL_LINE_SMOOTH enhancement on text
- [ ] Measure visual improvement and performance impact
- [ ] Verify compatibility with different font sizes

### Spell Effect Analysis
- [ ] Catalog CVisualEffect::Render pattern signature
- [ ] Identify spell effect rendering contexts
- [ ] Test different blending modes on existing effects
- [ ] Map spell types to optimal blending modes

### Weather Effect Analysis
- [ ] Locate CRainStorm/CSnowStorm pattern signatures
- [ ] Test particle smoothing enhancements
- [ ] Measure atmospheric effect improvements

## Notes
- All enhancements must work within BGEE's existing architecture
- Performance is critical - enhancements should not impact frame rate
- Compatibility with other mods (especially graphics mods) is essential
- Consider making enhancements configurable via INI settings

# Render function

?Render3d@CVidCell@@QEAAHHHAEBVCRect@@0PEAVCVidPoly@@HKH@Z
?Render3d@CVidCell@@QEAAHHHAEBVCRect@@K@Z
?Render3d@CVidCell@@QEAAHHHAEBVCRect@@PEAVCVidPoly@@HKH@Z
?Render@CBlood@@QEAAXPEAVCVidMode@@@Z
?Render@CFog@@QEAAXPEAVCVidMode@@AEBVCPoint@@AEBVCRect@@K@Z
?Render@CGameAnimationTypeAmbient@@UEAAXPEAVCInfinity@@PEAVCVidMode@@AEBVCRect@@AEBVCPoint@@3KK2HHJEPEAVCGameSprite@@@Z
?Render@CGameAnimationTypeAmbientStatic@@UEAAXPEAVCInfinity@@PEAVCVidMode@@AEBVCRect@@AEBVCPoint@@3KK2HHJEPEAVCGameSprite@@@Z
?Render@CGameAnimationTypeCharacter@@UEAAXPEAVCInfinity@@PEAVCVidMode@@AEBVCRect@@AEBVCPoint@@3KK2HHJEPEAVCGameSprite@@@Z
?Render@CGameAnimationTypeCharacterOld@@UEAAXPEAVCInfinity@@PEAVCVidMode@@AEBVCRect@@AEBVCPoint@@3KK2HHJEPEAVCGameSprite@@@Z
?Render@CGameAnimationTypeEffect@@UEAAXPEAVCInfinity@@PEAVCVidMode@@AEBVCRect@@AEBVCPoint@@3KK2HHJEPEAVCGameSprite@@@Z
?Render@CGameAnimationTypeFlying@@UEAAXPEAVCInfinity@@PEAVCVidMode@@AEBVCRect@@AEBVCPoint@@3KK2HHJEPEAVCGameSprite@@@Z
?Render@CGameAnimationTypeMonster@@UEAAXPEAVCInfinity@@PEAVCVidMode@@AEBVCRect@@AEBVCPoint@@3KK2HHJEPEAVCGameSprite@@@Z
?Render@CGameAnimationTypeMonsterAnkheg@@UEAAXPEAVCInfinity@@PEAVCVidMode@@AEBVCRect@@AEBVCPoint@@3KK2HHJEPEAVCGameSprite@@@Z
?Render@CGameAnimationTypeMonsterIcewind@@UEAAXPEAVCInfinity@@PEAVCVidMode@@AEBVCRect@@AEBVCPoint@@3KK2HHJEPEAVCGameSprite@@@Z
?Render@CGameAnimationTypeMonsterLarge16@@UEAAXPEAVCInfinity@@PEAVCVidMode@@AEBVCRect@@AEBVCPoint@@3KK2HHJEPEAVCGameSprite@@@Z
?Render@CGameAnimationTypeMonsterLarge@@UEAAXPEAVCInfinity@@PEAVCVidMode@@AEBVCRect@@AEBVCPoint@@3KK2HHJEPEAVCGameSprite@@@Z
?Render@CGameAnimationTypeMonsterLayered@@UEAAXPEAVCInfinity@@PEAVCVidMode@@AEBVCRect@@AEBVCPoint@@3KK2HHJEPEAVCGameSprite@@@Z
?Render@CGameAnimationTypeMonsterLayeredSpell@@UEAAXPEAVCInfinity@@PEAVCVidMode@@AEBVCRect@@AEBVCPoint@@3KK2HHJEPEAVCGameSprite@@@Z
?Render@CGameAnimationTypeMonsterMulti@@UEAAXPEAVCInfinity@@PEAVCVidMode@@AEBVCRect@@AEBVCPoint@@3KK2HHJEPEAVCGameSprite@@@Z
?Render@CGameAnimationTypeMonsterMultiNew@@UEAAXPEAVCInfinity@@PEAVCVidMode@@AEBVCRect@@AEBVCPoint@@3KK2HHJEPEAVCGameSprite@@@Z
?Render@CGameAnimationTypeMonsterOld@@UEAAXPEAVCInfinity@@PEAVCVidMode@@AEBVCRect@@AEBVCPoint@@3KK2HHJEPEAVCGameSprite@@@Z
?Render@CGameAnimationTypeMonsterQuadrant@@UEAAXPEAVCInfinity@@PEAVCVidMode@@AEBVCRect@@AEBVCPoint@@3KK2HHJEPEAVCGameSprite@@@Z
?Render@CGameAnimationTypeTownStatic@@UEAAXPEAVCInfinity@@PEAVCVidMode@@AEBVCRect@@AEBVCPoint@@3KK2HHJEPEAVCGameSprite@@@Z
?Render@CGameArea@@QEAAXPEAVCVidMode@@@Z
?Render@CGameChunk@@UEAAXPEAVCGameArea@@PEAVCVidMode@@@Z
?Render@CGameContainer@@UEAAXPEAVCGameArea@@PEAVCVidMode@@@Z
?Render@CGameDoor@@UEAAXPEAVCGameArea@@PEAVCVidMode@@@Z
?Render@CGameFireball3d@@UEAAXPEAVCGameArea@@PEAVCVidMode@@@Z
?Render@CGameObject@@UEAAXPEAVCGameArea@@PEAVCVidMode@@@Z
?Render@CGameSpawning@@UEAAXPEAVCGameArea@@PEAVCVidMode@@@Z
?Render@CGameSprite@@UEAAXPEAVCGameArea@@PEAVCVidMode@@@Z
?Render@CGameStatic@@UEAAXPEAVCGameArea@@PEAVCVidMode@@@Z
?Render@CGameTemporal@@UEAAXPEAVCGameArea@@PEAVCVidMode@@@Z
?Render@CGameText@@UEAAXPEAVCGameArea@@PEAVCVidMode@@@Z
?Render@CGameTrigger@@UEAAXPEAVCGameArea@@PEAVCVidMode@@@Z
?Render@CInfinity@@QEAAKPEAVCVidMode@@H@Z
?Render@CInfTileSet@@QEAAHHHAEBVCRect@@HHAEBUTILE_CODE@@KEE@Z
?Render@CInfToolTip@@UEAAHHHAEBVCRect@@PEAVCVidPoly@@HKH@Z
?Render@CMarker@@QEAAXPEAVCVidMode@@PEAVCGameSprite@@@Z
?Render@CMarker@@QEAAXPEAVCVidMode@@PEAVCInfinity@@AEBVCPoint@@JJ@Z
?Render@CObjectMarker@@UEAAXPEAVCGameArea@@PEAVCVidMode@@@Z
?Render@CParticle@@QEAAXAEBVCPoint@@AEBVCRect@@GG@Z
?Render@CProjectileBAM@@UEAAXPEAVCGameArea@@PEAVCVidMode@@@Z
?Render@CProjectileChain@@UEAAXPEAVCGameArea@@PEAVCVidMode@@@Z
?Render@CProjectileNewScorcher@@UEAAXPEAVCGameArea@@PEAVCVidMode@@@Z
?Render@CProjectileScorcher@@UEAAXPEAVCGameArea@@PEAVCVidMode@@@Z
?Render@CProjectileSkyStrike@@UEAAXPEAVCGameArea@@PEAVCVidMode@@@Z
?Render@CProjectileSkyStrikeBAM@@UEAAXPEAVCGameArea@@PEAVCVidMode@@@Z
?Render@CProjectileSpellHit@@UEAAXPEAVCGameArea@@PEAVCVidMode@@@Z
?Render@CProjectileTravelDoor@@UEAAXPEAVCGameArea@@PEAVCVidMode@@@Z
?Render@CRainStorm@@QEAAXPEAVCVidMode@@AEBVCRect@@K@Z
?Render@CResWebm@@QEAAXXZ
?Render@CSnowStorm@@QEAAXPEAVCVidMode@@AEBVCRect@@K@Z
?Render@CSparkleCluster@@UEAAXPEAVCGameArea@@PEAVCVidMode@@@Z
?Render@CVEFVidCell@@UEAAXPEAVCGameArea@@PEAVCVidMode@@@Z
?Render@CVidBitmap@@QEAAXHHAEBVCRect@@K@Z
?Render@CVidCell@@UEAAHHHAEBVCRect@@PEAVCVidPoly@@HKH@Z
?Render@CVidCell@@UEAAHPEAKJHHAEBVCRect@@KAEBVCPoint@@@Z
?Render@CVidDrawable@@QEAAHHHAEBVCRect@@0K@Z
?Render@CVidMosaic@@QEAAHHHAEBVCRect@@0K@Z
?Render@CVisualEffect@@UEAAXPEAVCGameArea@@PEAVCVidMode@@@Z
?RenderActions@CGameSprite@@QEAAXXZ
?RenderAOE@CInfinity@@QEAAXPEAVCVidMode@@@Z
?RenderAppearance@CInfGame@@QEAAXVCPoint@@H@Z
?RenderAreaMap@CScreenMap@@QEAAHVCRect@@@Z
?RenderBam@CGameStatic@@IEAAXPEAVCGameArea@@PEAVCVidMode@@@Z
?RenderBlack@CVidMode@@QEAAHXZ
?RenderBlackFade3d@CVidMode@@SAXVCRect@@@Z
?RenderColorDisplay@CInfGame@@QEAAXVCRect@@HH@Z
?RenderDamageArrow@CGameSprite@@QEAAXPEAVCGameArea@@PEAVCVidMode@@@Z
?RenderDynamic@CSearchBitmap@@QEAAXXZ
?RenderEdgeFade@CInfinity@@QEAAXPEAVCVidMode@@PEAVCVisibilityMap@@@Z
?RenderEncumbrance@CScreenInventory@@QEAAXVCPoint@@VCRect@@@Z
?RenderFlash@CInfinity@@QEAAXPEAVCVidMode@@KE@Z
?RenderFog@CInfinity@@QEAAXPEAVCVidMode@@PEAVCVisibilityMap@@@Z
?RenderFrameTimes@CScreenWorld@@QEAAXXZ
?RenderHealthBar@CGameSprite@@QEAAXPEAVCVidMode@@@Z
?RenderHighlight@@YAXUSDL_Rect@@0@Z
?RenderHWPointerImage@CVidMode@@IEAAHPEAVCVidCell@@HHHVCRect@@@Z
?RenderIcon@CIcon@@QEAAXAEBVCPoint@@AEBVCSize@@AEBVCRect@@AEBVCResRef@@KGHGH2H@Z
?RenderLightning@CInfinity@@QEAAHAEAVCRect@@HHHHKKK@Z
?RenderListCallback@@YAXPEAX@Z
?RenderMarkers@CGameSprite@@QEAAXPEAVCVidMode@@@Z
?RenderMirrorImage@CGameSprite@@QEAAXHAEAVCRect@@00AEAVCPoint@@PEAVCSearchBitmap@@PEAVCVisibilityMap@@PEAVCVidMode@@AEAKAEAE65@Z
?RenderPalette@CInfGame@@QEAAXVCRect@@H@Z
?RenderPointer@CVidMode@@QEAAHXZ
?RenderPortrait@CGameSprite@@QEAAXAEBVCPoint@@AEBVCSize@@HHHAEBVCRect@@H@Z
?RenderPortrait@CInfGame@@QEAAXKAEBVCPoint@@AEBVCSize@@HHHAEBVCRect@@@Z
?RenderPVR@CVidCell@@QEAAHHHAEBVCRect@@0KH@Z
?RenderScaled@CVidBitmap@@QEAAXAEBVCRect@@0K@Z
?RenderSearchMap@CGameArea@@QEAAXPEAVCVidMode@@AEBVCRect@@@Z
?RenderSpriteCover@CGameSprite@@QEAAXPEAVCVidMode@@PEAVCVidCell@@KEEHH@Z
?RenderSpriteEffect@CGameSprite@@QEAAXPEAVCVidMode@@HH@Z
?RenderStatic@CSearchBitmap@@QEAAXXZ
?RenderSWPointerImage@CVidMode@@IEAAHPEAVCVidCell@@HHHVCRect@@@Z
?RenderTexture@CVidCell@@SAXHHAEBVCRect@@VCSize@@00K@Z
?RenderTexture@CVidCell@@SAXHHAEBVCRect@@VCSize@@0K@Z
?RenderTexture@CVidTile@@QEAAXHAEBVCRect@@HHK@Z
?RenderTextWrap@CVidFont@@QEAAHPEBDHHHHAEAVCRect@@HH_N2@Z
?RenderToMapScreen@CGameSprite@@QEAAXAEBVCRect@@AEBVCPoint@@@Z
?RenderTrackingArrow@CGameSprite@@QEAAXPEAVCGameArea@@PEAVCVidMode@@@Z
?RenderTransitions@CInfinity@@QEAAXPEAVCVidMode@@PEAVCSearchBitmap@@@Z
?RenderUI@CBaldurEngine@@UEAAXXZ
?RenderUI@CWarp@@UEAAXXZ
?RenderZoomed@CGameArea@@QEAAXXZ