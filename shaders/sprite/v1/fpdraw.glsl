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

bool ieeHasTcScale()
{
    return uTcScale.x > 0.0 && uTcScale.y > 0.0;
}

mediump vec2 ieeSafeTcScale()
{
    if (ieeHasTcScale())
    {
        return uTcScale;
    }

    return vec2(1.0 / 1024.0, 1.0 / 1024.0);
}

lowp vec4 ieeSampleClamped(in mediump vec2 uv)
{
    mediump vec2 halfTexel = ieeSafeTcScale() * 0.5;
    return texture2D(uTex, clamp(uv, halfTexel, vec2(1.0) - halfTexel));
}

mediump float ieeLuma(in lowp vec3 c)
{
    return c.g + 0.5 * (c.r + c.b);
}

lowp vec3 ieeSampleTexelRGB(in mediump vec2 basePos, in mediump vec2 offset)
{
    return ieeSampleClamped((basePos + offset + vec2(0.5, 0.5)) * ieeSafeTcScale()).rgb;
}

void ieeEasuSet(
    inout mediump vec2 dir,
    inout mediump float len,
    mediump float w,
    mediump float lA,
    mediump float lB,
    mediump float lC,
    mediump float lD,
    mediump float lE)
{
    mediump float lenX = max(abs(lD - lC), abs(lC - lB));
    mediump float dirX = lD - lB;
    dir.x += dirX * w;
    lenX = clamp(abs(dirX) / max(lenX, 1.0e-5), 0.0, 1.0);
    lenX *= lenX;
    len += lenX * w;

    mediump float lenY = max(abs(lE - lC), abs(lC - lA));
    mediump float dirY = lE - lA;
    dir.y += dirY * w;
    lenY = clamp(abs(dirY) / max(lenY, 1.0e-5), 0.0, 1.0);
    lenY *= lenY;
    len += lenY * w;
}

void ieeEasuTap(
    inout mediump vec3 aC,
    inout mediump float aW,
    mediump vec2 off,
    mediump vec2 dir,
    mediump vec2 len,
    mediump float lob,
    mediump float clp,
    lowp vec3 c)
{
    mediump vec2 v = vec2(dot(off, dir), dot(off, vec2(-dir.y, dir.x)));
    v *= len;
    mediump float d2 = min(dot(v, v), clp);
    mediump float wB = 0.4 * d2 - 1.0;
    mediump float wA = lob * d2 - 1.0;
    wB *= wB;
    wA *= wA;
    wB = 1.5625 * wB - 0.5625;
    mediump float w = wB * wA;
    aC += c * w;
    aW += w;
}

lowp vec3 ieeSampleSpriteBody(in mediump vec2 uv)
{
    if (!ieeHasTcScale())
    {
        return texture2D(uTex, uv).rgb;
    }

    mediump vec2 tcScale = ieeSafeTcScale();
    mediump vec2 sourcePos = uv / tcScale - vec2(0.5, 0.5);
    mediump vec2 basePos = floor(sourcePos);
    mediump vec2 pp = sourcePos - basePos;

    lowp vec3 bC = ieeSampleTexelRGB(basePos, vec2( 0.0, -1.0)); mediump float bL = ieeLuma(bC);
    lowp vec3 cC = ieeSampleTexelRGB(basePos, vec2( 1.0, -1.0)); mediump float cL = ieeLuma(cC);
    lowp vec3 eC = ieeSampleTexelRGB(basePos, vec2(-1.0,  0.0)); mediump float eL = ieeLuma(eC);
    lowp vec3 fC = ieeSampleTexelRGB(basePos, vec2( 0.0,  0.0)); mediump float fL = ieeLuma(fC);
    lowp vec3 gC = ieeSampleTexelRGB(basePos, vec2( 1.0,  0.0)); mediump float gL = ieeLuma(gC);
    lowp vec3 hC = ieeSampleTexelRGB(basePos, vec2( 2.0,  0.0)); mediump float hL = ieeLuma(hC);
    lowp vec3 iC = ieeSampleTexelRGB(basePos, vec2(-1.0,  1.0)); mediump float iL = ieeLuma(iC);
    lowp vec3 jC = ieeSampleTexelRGB(basePos, vec2( 0.0,  1.0)); mediump float jL = ieeLuma(jC);
    lowp vec3 kC = ieeSampleTexelRGB(basePos, vec2( 1.0,  1.0)); mediump float kL = ieeLuma(kC);
    lowp vec3 lC = ieeSampleTexelRGB(basePos, vec2( 2.0,  1.0)); mediump float lL = ieeLuma(lC);
    lowp vec3 nC = ieeSampleTexelRGB(basePos, vec2( 0.0,  2.0)); mediump float nL = ieeLuma(nC);
    lowp vec3 oC = ieeSampleTexelRGB(basePos, vec2( 1.0,  2.0)); mediump float oL = ieeLuma(oC);

    mediump vec2 dir = vec2(0.0, 0.0);
    mediump float len = 0.0;
    ieeEasuSet(dir, len, (1.0 - pp.x) * (1.0 - pp.y), bL, eL, fL, gL, jL);
    ieeEasuSet(dir, len, pp.x * (1.0 - pp.y), cL, fL, gL, hL, kL);
    ieeEasuSet(dir, len, (1.0 - pp.x) * pp.y, fL, iL, jL, kL, nL);
    ieeEasuSet(dir, len, pp.x * pp.y, gL, jL, kL, lL, oL);

    mediump float dirR = dot(dir, dir);
    bool zro = dirR < (1.0 / 32768.0);
    dirR = inversesqrt(max(dirR, 1.0 / 32768.0));
    dir = zro ? vec2(1.0, 0.0) : dir * dirR;

    len = 0.5 * len;
    len *= len;
    mediump float stretch = dot(dir, dir) / max(max(abs(dir.x), abs(dir.y)), 1.0e-5);
    mediump vec2 len2 = vec2(1.0 + (stretch - 1.0) * len, 1.0 - 0.5 * len);
    mediump float lob = 0.5 - 0.29 * len;
    mediump float clp = 1.0 / lob;

    lowp vec3 min4 = min(min(fC, gC), min(jC, kC));
    lowp vec3 max4 = max(max(fC, gC), max(jC, kC));
    mediump vec3 aC = vec3(0.0, 0.0, 0.0);
    mediump float aW = 0.0;
    ieeEasuTap(aC, aW, vec2( 0.0, -1.0) - pp, dir, len2, lob, clp, bC);
    ieeEasuTap(aC, aW, vec2( 1.0, -1.0) - pp, dir, len2, lob, clp, cC);
    ieeEasuTap(aC, aW, vec2(-1.0,  1.0) - pp, dir, len2, lob, clp, iC);
    ieeEasuTap(aC, aW, vec2( 0.0,  1.0) - pp, dir, len2, lob, clp, jC);
    ieeEasuTap(aC, aW, vec2( 0.0,  0.0) - pp, dir, len2, lob, clp, fC);
    ieeEasuTap(aC, aW, vec2(-1.0,  0.0) - pp, dir, len2, lob, clp, eC);
    ieeEasuTap(aC, aW, vec2( 1.0,  1.0) - pp, dir, len2, lob, clp, kC);
    ieeEasuTap(aC, aW, vec2( 2.0,  1.0) - pp, dir, len2, lob, clp, lC);
    ieeEasuTap(aC, aW, vec2( 2.0,  0.0) - pp, dir, len2, lob, clp, hC);
    ieeEasuTap(aC, aW, vec2( 1.0,  0.0) - pp, dir, len2, lob, clp, gC);
    ieeEasuTap(aC, aW, vec2( 1.0,  2.0) - pp, dir, len2, lob, clp, oC);
    ieeEasuTap(aC, aW, vec2( 0.0,  2.0) - pp, dir, len2, lob, clp, nC);

    return min(max4, max(min4, aC / max(aW, 1.0e-5)));
}

lowp vec3 ieeSharpenSpriteBody(in mediump vec2 uv, in lowp vec3 filteredRgb, in lowp float filteredAlpha)
{
    mediump vec2 tcScale = ieeSafeTcScale();
    lowp vec4 left = ieeSampleClamped(uv - vec2(tcScale.x, 0.0));
    lowp vec4 right = ieeSampleClamped(uv + vec2(tcScale.x, 0.0));
    lowp vec4 up = ieeSampleClamped(uv - vec2(0.0, tcScale.y));
    lowp vec4 down = ieeSampleClamped(uv + vec2(0.0, tcScale.y));

    lowp vec3 neighborhood = (left.rgb + right.rgb + up.rgb + down.rgb) * 0.25;
    lowp vec3 sharpened = filteredRgb + (filteredRgb - neighborhood) * 0.35;
    mediump float alphaMask = smoothstep(0.15, 0.80, filteredAlpha);

    return clamp(mix(filteredRgb, sharpened, alphaMask), 0.0, 1.0);
}

void main()
{
    lowp vec4 texColor = texture2D(uTex, vTc);

    if (uIeeSpriteBodyMode > 0.5)
    {
        lowp vec3 bodyRgb = ieeSampleSpriteBody(vTc);
        bodyRgb = ieeSharpenSpriteBody(vTc, bodyRgb, texColor.a);
        texColor = vec4(bodyRgb, texColor.a);
    }

    texColor *= vColor;

    mediump float grey = dot(texColor.rgb, vec3(0.299, 0.587, 0.114));
    lowp vec3 tone = grey * uColorTone.rgb;
    gl_FragColor = vec4(mix(texColor.rgb, tone, uColorTone.a), texColor.a);
}
