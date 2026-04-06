// IEE_DIAG_FPSELECT_EASU_DELTA
uniform lowp sampler2D uTex;
uniform lowp float uSpriteBlurAmount;
uniform mediump vec2 uTcScale;
varying mediump vec2 vTc;
varying lowp vec4 vColor;

const float fSolidThreshold = 0.1;

lowp float ieeDiagFpselectEasuDeltaMarker()
{
    return 0.0;
}

bool uhHasTcScale()
{
    return uTcScale.x > 0.0 && uTcScale.y > 0.0;
}

mediump vec2 uhSpriteTcScale()
{
    if (uhHasTcScale())
    {
        return uTcScale;
    }
    return vec2(0.0005, 0.0005);
}

lowp vec4 uhSampleSprite(in mediump vec2 uv)
{
    mediump vec2 tcScale = uhSpriteTcScale();
    mediump vec2 halfTexel = tcScale * 0.5;
    return texture2D(uTex, clamp(uv, halfTexel, vec2(1.0) - halfTexel));
}

lowp vec3 uhSampleTexelRGB(in mediump vec2 basePos, in mediump vec2 offset)
{
    return uhSampleSprite((basePos + offset + vec2(0.5, 0.5)) * uhSpriteTcScale()).rgb;
}

mediump float uhLuma(in lowp vec3 c)
{
    return c.g + 0.5 * (c.r + c.b);
}

void uhEasuSet(
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

void uhEasuTap(
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

lowp vec3 uhSampleEasu(in mediump vec2 uv)
{
    mediump vec2 tcScale = uhSpriteTcScale();
    mediump vec2 sourcePos = uv / tcScale - vec2(0.5, 0.5);
    mediump vec2 basePos = floor(sourcePos);
    mediump vec2 pp = sourcePos - basePos;

    lowp vec3 bC = uhSampleTexelRGB(basePos, vec2( 0.0, -1.0)); mediump float bL = uhLuma(bC);
    lowp vec3 cC = uhSampleTexelRGB(basePos, vec2( 1.0, -1.0)); mediump float cL = uhLuma(cC);
    lowp vec3 eC = uhSampleTexelRGB(basePos, vec2(-1.0,  0.0)); mediump float eL = uhLuma(eC);
    lowp vec3 fC = uhSampleTexelRGB(basePos, vec2( 0.0,  0.0)); mediump float fL = uhLuma(fC);
    lowp vec3 gC = uhSampleTexelRGB(basePos, vec2( 1.0,  0.0)); mediump float gL = uhLuma(gC);
    lowp vec3 hC = uhSampleTexelRGB(basePos, vec2( 2.0,  0.0)); mediump float hL = uhLuma(hC);
    lowp vec3 iC = uhSampleTexelRGB(basePos, vec2(-1.0,  1.0)); mediump float iL = uhLuma(iC);
    lowp vec3 jC = uhSampleTexelRGB(basePos, vec2( 0.0,  1.0)); mediump float jL = uhLuma(jC);
    lowp vec3 kC = uhSampleTexelRGB(basePos, vec2( 1.0,  1.0)); mediump float kL = uhLuma(kC);
    lowp vec3 lC = uhSampleTexelRGB(basePos, vec2( 2.0,  1.0)); mediump float lL = uhLuma(lC);
    lowp vec3 nC = uhSampleTexelRGB(basePos, vec2( 0.0,  2.0)); mediump float nL = uhLuma(nC);
    lowp vec3 oC = uhSampleTexelRGB(basePos, vec2( 1.0,  2.0)); mediump float oL = uhLuma(oC);

    mediump vec2 dir = vec2(0.0, 0.0);
    mediump float len = 0.0;
    uhEasuSet(dir, len, (1.0 - pp.x) * (1.0 - pp.y), bL, eL, fL, gL, jL);
    uhEasuSet(dir, len, pp.x * (1.0 - pp.y), cL, fL, gL, hL, kL);
    uhEasuSet(dir, len, (1.0 - pp.x) * pp.y, fL, iL, jL, kL, nL);
    uhEasuSet(dir, len, pp.x * pp.y, gL, jL, kL, lL, oL);

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
    uhEasuTap(aC, aW, vec2( 0.0, -1.0) - pp, dir, len2, lob, clp, bC);
    uhEasuTap(aC, aW, vec2( 1.0, -1.0) - pp, dir, len2, lob, clp, cC);
    uhEasuTap(aC, aW, vec2(-1.0,  1.0) - pp, dir, len2, lob, clp, iC);
    uhEasuTap(aC, aW, vec2( 0.0,  1.0) - pp, dir, len2, lob, clp, jC);
    uhEasuTap(aC, aW, vec2( 0.0,  0.0) - pp, dir, len2, lob, clp, fC);
    uhEasuTap(aC, aW, vec2(-1.0,  0.0) - pp, dir, len2, lob, clp, eC);
    uhEasuTap(aC, aW, vec2( 1.0,  1.0) - pp, dir, len2, lob, clp, kC);
    uhEasuTap(aC, aW, vec2( 2.0,  1.0) - pp, dir, len2, lob, clp, lC);
    uhEasuTap(aC, aW, vec2( 2.0,  0.0) - pp, dir, len2, lob, clp, hC);
    uhEasuTap(aC, aW, vec2( 1.0,  0.0) - pp, dir, len2, lob, clp, gC);
    uhEasuTap(aC, aW, vec2( 1.0,  2.0) - pp, dir, len2, lob, clp, oC);
    uhEasuTap(aC, aW, vec2( 0.0,  2.0) - pp, dir, len2, lob, clp, nC);

    return min(max4, max(min4, aC / max(aW, 1.0e-5)));
}

lowp vec3 uhDeltaHeat(in lowp vec3 baseRgb, in lowp vec3 easuRgb)
{
    mediump vec3 delta = abs(vec3(easuRgb) - vec3(baseRgb));
    mediump float magnitude = max(delta.r, max(delta.g, delta.b));

    mediump vec3 heat = vec3(0.0);
    heat += mix(vec3(0.0), vec3(0.0, 0.25, 1.0), smoothstep(0.001, 0.01, magnitude));
    heat += mix(vec3(0.0), vec3(1.0, 0.45, 0.0), smoothstep(0.01, 0.04, magnitude));
    heat += mix(vec3(0.0), vec3(1.0, 1.0, 1.0), smoothstep(0.04, 0.12, magnitude));
    heat = min(heat, vec3(1.0));

    return heat;
}

void main()
{
    lowp vec4 texColor = texture2D(uTex, vTc);
    if (texColor.a > fSolidThreshold)
    {
        lowp vec3 easuRgb = uhSampleEasu(vTc);
        lowp vec3 heat = uhDeltaHeat(texColor.rgb, easuRgb) + vec3(ieeDiagFpselectEasuDeltaMarker());
        gl_FragColor = vec4(heat, texColor.a);
        return;
    }

    gl_FragColor = vColor;
}
