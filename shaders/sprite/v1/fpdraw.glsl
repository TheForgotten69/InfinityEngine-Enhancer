#if !defined(GL_ES)
#define highp
#define mediump
#define lowp
#else
precision highp float;
#endif

// fpDraw.glsl
// IEE_V2_FPDRAW_SPRITE_BODY
uniform lowp sampler2D uTex;
uniform highp vec2 uTcScale;
uniform highp vec4 uColorTone;
uniform highp float uIeeSpriteBodyMode;
varying mediump vec2 vTc;
varying lowp vec4 vColor;

lowp vec4 ieeSampleSource(in mediump vec2 uv)
{
    mediump vec2 texel = max(uTcScale, vec2(1.0 / 8192.0));
    mediump vec2 halfTexel = 0.5 * texel;
    return texture2D(uTex, clamp(uv, halfTexel, vec2(1.0) - halfTexel));
}

mediump float ieeLuma(in lowp vec3 rgb)
{
    return dot(rgb, vec3(0.299, 0.587, 0.114));
}

lowp vec4 ieeSampleCatmullRom(in mediump vec2 uv)
{
    mediump vec2 texel = max(uTcScale, vec2(1.0 / 8192.0));
    mediump vec2 texSize = vec2(1.0) / texel;
    mediump vec2 samplePos = uv * texSize;
    mediump vec2 texPos1 = floor(samplePos - 0.5) + 0.5;
    mediump vec2 f = samplePos - texPos1;
    mediump vec2 f2 = f * f;
    mediump vec2 f3 = f2 * f;

    mediump vec2 w0 = f2 - 0.5 * (f3 + f);
    mediump vec2 w1 = 1.5 * f3 - 2.5 * f2 + 1.0;
    mediump vec2 w3 = 0.5 * (f3 - f2);
    mediump vec2 w2 = 1.0 - w0 - w1 - w3;

    mediump vec2 w12 = w1 + w2;
    mediump vec2 offset12 = vec2(
        w12.x > 0.0 ? (w2.x / w12.x) : 0.0,
        w12.y > 0.0 ? (w2.y / w12.y) : 0.0);

    mediump vec2 texPos0 = (texPos1 - 1.0) * texel;
    mediump vec2 texPos3 = (texPos1 + 2.0) * texel;
    mediump vec2 texPos12 = (texPos1 + offset12) * texel;

    lowp vec4 result = vec4(0.0);
    result += ieeSampleSource(vec2(texPos0.x, texPos0.y)) * (w0.x * w0.y);
    result += ieeSampleSource(vec2(texPos12.x, texPos0.y)) * (w12.x * w0.y);
    result += ieeSampleSource(vec2(texPos3.x, texPos0.y)) * (w3.x * w0.y);

    result += ieeSampleSource(vec2(texPos0.x, texPos12.y)) * (w0.x * w12.y);
    result += ieeSampleSource(vec2(texPos12.x, texPos12.y)) * (w12.x * w12.y);
    result += ieeSampleSource(vec2(texPos3.x, texPos12.y)) * (w3.x * w12.y);

    result += ieeSampleSource(vec2(texPos0.x, texPos3.y)) * (w0.x * w3.y);
    result += ieeSampleSource(vec2(texPos12.x, texPos3.y)) * (w12.x * w3.y);
    result += ieeSampleSource(vec2(texPos3.x, texPos3.y)) * (w3.x * w3.y);
    return result;
}

lowp vec4 ieeEnhanceSpriteBody()
{
    mediump vec2 texel = max(uTcScale, vec2(1.0 / 8192.0));
    lowp vec4 center = ieeSampleSource(vTc);
    if (center.a <= 0.001) {
        return center;
    }

    lowp vec4 filtered = ieeSampleCatmullRom(vTc);
    lowp vec4 north = ieeSampleSource(vTc + vec2(0.0, -texel.y));
    lowp vec4 south = ieeSampleSource(vTc + vec2(0.0, texel.y));
    lowp vec4 west = ieeSampleSource(vTc + vec2(-texel.x, 0.0));
    lowp vec4 east = ieeSampleSource(vTc + vec2(texel.x, 0.0));
    lowp vec4 northWest = ieeSampleSource(vTc + vec2(-texel.x, -texel.y));
    lowp vec4 northEast = ieeSampleSource(vTc + vec2(texel.x, -texel.y));
    lowp vec4 southWest = ieeSampleSource(vTc + vec2(-texel.x, texel.y));
    lowp vec4 southEast = ieeSampleSource(vTc + vec2(texel.x, texel.y));

    mediump float cardinalCoverage = 0.25 * (north.a + south.a + west.a + east.a);
    mediump float diagonalCoverage = 0.25 * (northWest.a + northEast.a + southWest.a + southEast.a);
    mediump float coverage = 0.65 * cardinalCoverage + 0.35 * diagonalCoverage;
    mediump float solid = smoothstep(0.24, 0.92, center.a);
    mediump float interior = smoothstep(0.18, 0.80, coverage);
    mediump float alphaDelta = max(abs(center.a - cardinalCoverage), abs(center.a - diagonalCoverage));
    mediump float edgeStability = 1.0 - smoothstep(0.04, 0.28, alphaDelta);
    mediump float edgeGuard = solid * interior * edgeStability;

    mediump float filterMix = mix(0.38, 0.74, edgeGuard);
    lowp vec3 baseRgb = mix(center.rgb, filtered.rgb, filterMix);
    lowp vec3 neighborMean = 0.125 * (north.rgb + south.rgb + west.rgb + east.rgb +
                                      northWest.rgb + northEast.rgb + southWest.rgb + southEast.rgb);

    lowp vec3 minRgb = min(
        min(min(north.rgb, south.rgb), min(west.rgb, east.rgb)),
        min(min(northWest.rgb, northEast.rgb), min(southWest.rgb, southEast.rgb)));
    minRgb = min(minRgb, min(center.rgb, filtered.rgb));

    lowp vec3 maxRgb = max(
        max(max(north.rgb, south.rgb), max(west.rgb, east.rgb)),
        max(max(northWest.rgb, northEast.rgb), max(southWest.rgb, southEast.rgb)));
    maxRgb = max(maxRgb, max(center.rgb, filtered.rgb));

    mediump float centerLuma = ieeLuma(center.rgb);
    mediump float meanLuma = ieeLuma(neighborMean);
    mediump float lumaMin = min(
        min(min(ieeLuma(north.rgb), ieeLuma(south.rgb)), min(ieeLuma(west.rgb), ieeLuma(east.rgb))),
        min(min(ieeLuma(northWest.rgb), ieeLuma(northEast.rgb)), min(ieeLuma(southWest.rgb), ieeLuma(southEast.rgb))));
    mediump float lumaMax = max(
        max(max(ieeLuma(north.rgb), ieeLuma(south.rgb)), max(ieeLuma(west.rgb), ieeLuma(east.rgb))),
        max(max(ieeLuma(northWest.rgb), ieeLuma(northEast.rgb)), max(ieeLuma(southWest.rgb), ieeLuma(southEast.rgb))));
    mediump float localContrast = max(
        smoothstep(0.03, 0.20, lumaMax - lumaMin),
        smoothstep(0.01, 0.08, abs(centerLuma - meanLuma)));
    mediump float sharpenStrength = (0.06 + 0.10 * localContrast) * edgeGuard;

    mediump vec3 sharpened = baseRgb + (baseRgb - neighborMean) * sharpenStrength;
    lowp vec3 clampSlack = vec3(0.02 + 0.02 * localContrast);
    lowp vec3 clamped = clamp(sharpened, minRgb - clampSlack, maxRgb + clampSlack);
    lowp vec3 rgb = mix(baseRgb, clamped, edgeGuard * (0.70 + 0.30 * localContrast));

    return vec4(rgb, center.a);
}

void main()
{
    lowp vec4 texColor;
    if (uIeeSpriteBodyMode > 0.5 && uTcScale.x > 0.0 && uTcScale.y > 0.0) {
        texColor = ieeEnhanceSpriteBody() * vColor;
    } else {
        texColor = texture2D(uTex, vTc) * vColor;
    }

    mediump float grey = dot(texColor.rgb, vec3(0.299, 0.587, 0.114));
    lowp vec3 tone = grey * uColorTone.rgb;
    gl_FragColor = vec4(mix(texColor.rgb, tone, uColorTone.a), texColor.a);
}
