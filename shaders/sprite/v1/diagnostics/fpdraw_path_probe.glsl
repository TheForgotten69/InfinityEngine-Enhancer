#if !defined(GL_ES)
#define highp
#define mediump
#define lowp
#else
precision highp float;
#endif

// IEE_DIAG_FPDRAW_PATH_PROBE
uniform lowp sampler2D uTex;
uniform mediump vec2 uTcScale;
uniform highp vec4 uColorTone;
varying mediump vec2 vTc;
varying lowp vec4 vColor;

lowp float ieeDiagFpdrawPathProbeMarker()
{
    return 0.0;
}

bool ieeHasTcScale()
{
    return uTcScale.x > 0.0 && uTcScale.y > 0.0;
}

mediump vec2 ieeProbeTcScale()
{
    if (ieeHasTcScale())
    {
        return uTcScale;
    }
    return vec2(0.0005, 0.0005);
}

lowp vec4 ieeSampleClamped(in mediump vec2 uv)
{
    mediump vec2 tcScale = ieeProbeTcScale();
    mediump vec2 halfTexel = tcScale * 0.5;
    return texture2D(uTex, clamp(uv, halfTexel, vec2(1.0) - halfTexel));
}

mediump float ieeAlphaDelta(in mediump vec2 uv, in mediump vec2 offset)
{
    return abs(ieeSampleClamped(uv).a - ieeSampleClamped(uv + offset).a);
}

mediump float ieeSpriteCandidate(in mediump vec2 uv)
{
    mediump vec2 tcScale = ieeProbeTcScale();
    mediump float candidate = 0.0;

    candidate = max(candidate, ieeAlphaDelta(uv, vec2( 6.0 * tcScale.x, 0.0)));
    candidate = max(candidate, ieeAlphaDelta(uv, vec2(-6.0 * tcScale.x, 0.0)));
    candidate = max(candidate, ieeAlphaDelta(uv, vec2(0.0,  6.0 * tcScale.y)));
    candidate = max(candidate, ieeAlphaDelta(uv, vec2(0.0, -6.0 * tcScale.y)));
    candidate = max(candidate, ieeAlphaDelta(uv, vec2( 4.0 * tcScale.x,  4.0 * tcScale.y)));
    candidate = max(candidate, ieeAlphaDelta(uv, vec2(-4.0 * tcScale.x,  4.0 * tcScale.y)));
    candidate = max(candidate, ieeAlphaDelta(uv, vec2( 4.0 * tcScale.x, -4.0 * tcScale.y)));
    candidate = max(candidate, ieeAlphaDelta(uv, vec2(-4.0 * tcScale.x, -4.0 * tcScale.y)));

    return step(0.15, candidate);
}

void main()
{
    lowp vec4 texColor = texture2D(uTex, vTc) * vColor;
    mediump float grey = dot(texColor.rgb, vec3(0.299, 0.587, 0.114));
    lowp vec3 tone = grey * uColorTone.rgb;
    lowp vec3 baseColor = mix(texColor.rgb, tone, uColorTone.a);

    mediump float stripe = step(0.5, fract((vTc.x + vTc.y) * 96.0));
    lowp vec3 genericTint = mix(vec3(0.02, 0.05, 0.12), vec3(0.10, 0.35, 1.00), stripe);
    lowp vec3 spriteTint = mix(vec3(1.00, 0.00, 0.75), vec3(0.00, 1.00, 0.95), stripe);

    mediump float spriteCandidate = ieeSpriteCandidate(vTc);

    lowp vec3 debugColor = mix(mix(baseColor, genericTint, 0.75), spriteTint, spriteCandidate);
    debugColor += vec3(ieeDiagFpdrawPathProbeMarker());

    gl_FragColor = vec4(debugColor, texColor.a);
}
