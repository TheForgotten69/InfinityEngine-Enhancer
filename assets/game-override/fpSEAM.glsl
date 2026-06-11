#if !defined(GL_ES)
#define highp
#define mediump
#define lowp
#else
precision highp float;
#endif

// fpSEAM.glsl
// IEE_V1_FPSEAM_LIQUID

uniform lowp sampler2D uTex;
uniform highp vec2 uTcScale;
uniform highp vec4 uColorTone;
uniform highp float uIeeTileLiquidMode;
uniform highp float uIeeLiquidTime;

varying highp vec2 vTc;
varying highp vec2 vRef;
varying lowp vec4 vColor;

bool isBorderColor(vec4 testColor)
{
    return (testColor.x == 0.0) && (testColor.y == 0.0) && (testColor.z == 0.0);
}

vec4 ieeSampleSeamAware(vec2 tc)
{
    vec2 texCoordUnbiased = tc / uTcScale;
    vec2 refCoordUnbiased = vRef / uTcScale;

    float fu = fract(texCoordUnbiased.x);
    float fv = fract(texCoordUnbiased.y);

    vec2 texCoordTileLoc = mod(texCoordUnbiased - refCoordUnbiased, 64.0);
    int texCoordTileLocIntx = int(floor(texCoordTileLoc.x));
    int texCoordTileLocInty = int(floor(texCoordTileLoc.y));

    vec4 texColor0 = texture2D(uTex, tc);

    vec4 texColor1 = texture2D(uTex, tc + vec2(uTcScale.x, 0.0));
    vec4 texColor2 = texture2D(uTex, tc + vec2(0.0, uTcScale.y));
    vec4 texColor3 = texture2D(uTex, tc + vec2(uTcScale.x, uTcScale.y));

    bool border0 = isBorderColor(texColor0);
    bool border1 = isBorderColor(texColor1);
    bool border2 = isBorderColor(texColor2);
    bool border3 = isBorderColor(texColor3);

    if (((texCoordTileLocIntx == 0) || (texCoordTileLocIntx >= 63) ||
         (texCoordTileLocInty == 0) || (texCoordTileLocInty >= 63)) &&
        (border0 || border1 || border2 || border3))
    {
        return texColor0;
    }

    if (border1) texColor1 = texColor0;
    if (border2) texColor2 = texColor0;
    if (border3) texColor3 = texColor0;

    vec4 texColorTop = mix(texColor0, texColor1, fu);
    vec4 texColorBottom = mix(texColor2, texColor3, fu);
    return mix(texColorTop, texColorBottom, fv);
}

float ieeHash(vec2 p)
{
    return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453123);
}

float ieeNoise(vec2 p)
{
    vec2 i = floor(p);
    vec2 f = fract(p);
    vec2 u = f * f * (3.0 - 2.0 * f);

    return mix(
        mix(ieeHash(i + vec2(0.0, 0.0)), ieeHash(i + vec2(1.0, 0.0)), u.x),
        mix(ieeHash(i + vec2(0.0, 1.0)), ieeHash(i + vec2(1.0, 1.0)), u.x),
        u.y);
}

float ieeFbm(vec2 p)
{
    float value = 0.0;
    float amplitude = 0.5;
    mat2 octave = mat2(vec2(1.6, 1.2), vec2(-1.2, 1.6));

    for (int i = 0; i < 4; ++i) {
        value += amplitude * ieeNoise(p);
        p = octave * p * 1.35;
        amplitude *= 0.5;
    }

    return value;
}

vec2 ieeHash22(vec2 p)
{
    return vec2(
        ieeHash(p),
        ieeHash(p + vec2(19.19, 47.23)));
}

float ieeVoronoi(vec2 uv)
{
    vec2 cell = floor(uv);
    vec2 local = fract(uv);
    float minDist = 8.0;

    for (int j = -1; j <= 1; ++j) {
        for (int i = -1; i <= 1; ++i) {
            vec2 offset = vec2(float(i), float(j));
            vec2 point = 0.5 + 0.5 * sin((6.2831853 * ieeHash22(cell + offset)) + (uIeeLiquidTime * 0.35));
            vec2 diff = offset + point - local;
            minDist = min(minDist, dot(diff, diff));
        }
    }

    return sqrt(minDist);
}

vec4 ieeApplyLiquidStyle(vec4 baseColor)
{
    if (uIeeTileLiquidMode < 0.5 || uTcScale.x <= 0.0 || uTcScale.y <= 0.0) {
        return baseColor;
    }

    vec2 texCoordUnbiased = vTc / uTcScale;
    vec2 refCoordUnbiased = vRef / uTcScale;
    vec2 tileUv = mod(texCoordUnbiased - refCoordUnbiased, 64.0) / 64.0;
    vec2 overlayUv = texCoordUnbiased / 64.0;
    float time = uIeeLiquidTime;
    float luma = dot(baseColor.rgb, vec3(0.299, 0.587, 0.114));

    float mode = floor(uIeeTileLiquidMode + 0.5);
    float flowSpeed = 0.75;
    float warpStrength = 0.90;
    float fluidScale = 2.5;
    float foamScale = 6.5;
    float foamCutoff = 0.78;
    float foamStrength = 0.85;
    float replaceAmount = 0.86;
    float sourceMix = 0.18;
    float pulse = 0.0;
    vec3 shallowColor = vec3(0.18, 0.70, 0.76);
    vec3 deepColor = vec3(0.02, 0.20, 0.32);
    vec3 foamColor = vec3(0.90, 0.98, 1.00);
    vec3 glowColor = vec3(0.05, 0.12, 0.20);
    vec3 sourceTint = vec3(0.55, 0.80, 0.95);

    if (mode < 1.5) {
        flowSpeed = 0.80;
        warpStrength = 1.00;
        fluidScale = 2.6;
        foamScale = 6.2;
        foamCutoff = 0.76;
        replaceAmount = 0.88;
        sourceMix = 0.20;
        shallowColor = vec3(0.22, 0.76, 0.80);
        deepColor = vec3(0.03, 0.24, 0.38);
        foamColor = vec3(0.92, 0.99, 1.00);
        glowColor = vec3(0.06, 0.15, 0.24);
        sourceTint = vec3(0.55, 0.85, 0.98);
    } else if (mode < 2.5) {
        flowSpeed = 0.42;
        warpStrength = 1.35;
        fluidScale = 2.2;
        foamScale = 5.2;
        foamCutoff = 0.82;
        foamStrength = 0.35;
        replaceAmount = 0.92;
        sourceMix = 0.10;
        pulse = 0.16;
        shallowColor = vec3(0.88, 0.36, 0.06);
        deepColor = vec3(0.22, 0.03, 0.00);
        foamColor = vec3(1.00, 0.84, 0.28);
        glowColor = vec3(0.48, 0.12, 0.02);
        sourceTint = vec3(1.00, 0.50, 0.10);
    } else if (mode < 3.5) {
        flowSpeed = 0.36;
        warpStrength = 0.82;
        fluidScale = 2.9;
        foamScale = 7.2;
        foamCutoff = 0.84;
        foamStrength = 0.42;
        replaceAmount = 0.90;
        sourceMix = 0.12;
        shallowColor = vec3(0.32, 0.58, 0.16);
        deepColor = vec3(0.05, 0.18, 0.05);
        foamColor = vec3(0.80, 0.94, 0.62);
        glowColor = vec3(0.10, 0.18, 0.05);
        sourceTint = vec3(0.60, 0.86, 0.50);
    } else if (mode < 4.5) {
        flowSpeed = 0.30;
        warpStrength = 0.65;
        fluidScale = 2.4;
        foamScale = 6.0;
        foamCutoff = 0.86;
        foamStrength = 0.28;
        replaceAmount = 0.84;
        sourceMix = 0.14;
        shallowColor = vec3(0.28, 0.36, 0.18);
        deepColor = vec3(0.08, 0.12, 0.05);
        foamColor = vec3(0.72, 0.78, 0.58);
        glowColor = vec3(0.08, 0.10, 0.03);
        sourceTint = vec3(0.58, 0.62, 0.40);
    } else {
        flowSpeed = 0.28;
        warpStrength = 0.72;
        fluidScale = 2.2;
        foamScale = 5.8;
        foamCutoff = 0.87;
        foamStrength = 0.24;
        replaceAmount = 0.82;
        sourceMix = 0.16;
        shallowColor = vec3(0.26, 0.32, 0.14);
        deepColor = vec3(0.07, 0.10, 0.03);
        foamColor = vec3(0.66, 0.72, 0.48);
        glowColor = vec3(0.08, 0.10, 0.02);
        sourceTint = vec3(0.52, 0.58, 0.28);
    }

    vec2 flowUv = overlayUv * fluidScale;
    float flowA = ieeFbm(flowUv + vec2(time * flowSpeed, -time * flowSpeed * 0.45));
    float flowB = ieeFbm((flowUv * 1.9) + vec2(-time * flowSpeed * 0.70, time * flowSpeed * 0.80));
    float wave = 0.5 + 0.5 * sin((tileUv.x * 12.0) + (tileUv.y * 7.0) + (flowA * 3.2) - (flowB * 2.1) + (time * flowSpeed * 4.0));
    float ridge = 0.5 + 0.5 * sin((overlayUv.x * 8.0) - (overlayUv.y * 5.5) + (flowB * 2.6) - (time * flowSpeed * 3.0));
    float bubbles = 1.0 - clamp(ieeVoronoi((overlayUv * foamScale) + vec2(flowA, flowB) * 1.5), 0.0, 1.0);

    vec2 warp = vec2(flowA - 0.5, flowB - 0.5) * warpStrength;
    vec4 warpedColor = ieeSampleSeamAware(vTc + warp * uTcScale * 1.75);
    float pseudoDepth = clamp((0.40 * (1.0 - luma)) + (0.35 * (1.0 - wave)) + (0.25 * (1.0 - ridge)), 0.0, 1.0);
    float shallowMask = 1.0 - smoothstep(0.18, 0.78, pseudoDepth);
    float crestMask = smoothstep(0.58, 0.86, wave + ridge * 0.25 + bubbles * 0.15);
    float foamMask = smoothstep(foamCutoff, foamCutoff + 0.16, (shallowMask * 0.75) + (crestMask * 0.45) + (bubbles * 0.55));

    vec3 stylized = mix(shallowColor, deepColor, pseudoDepth);
    stylized = mix(stylized, warpedColor.rgb * sourceTint, sourceMix);
    stylized += glowColor * (0.05 + 0.10 * ridge);
    if (pulse > 0.0) {
        stylized += vec3(1.00, 0.48, 0.10) * pulse * (0.55 + 0.45 * ridge);
    }
    stylized = mix(stylized, foamColor, clamp(foamMask * foamStrength, 0.0, 1.0));

    vec4 result = baseColor;
    result.rgb = mix(baseColor.rgb, clamp(stylized, 0.0, 1.0), replaceAmount);
    return result;
}

void main()
{
    vec4 texColor = ieeSampleSeamAware(vTc);
    texColor = ieeApplyLiquidStyle(texColor);
    texColor = texColor * vColor;

    float grey = dot(texColor.rgb, vec3(0.299, 0.587, 0.114));
    vec3 tone = grey * uColorTone.rgb;

    gl_FragColor = vec4(mix(texColor.rgb, tone, uColorTone.a), texColor.a);
}
