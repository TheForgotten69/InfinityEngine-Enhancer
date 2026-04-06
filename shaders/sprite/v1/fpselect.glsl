// IEE_V1_FPSELECT
uniform lowp sampler2D uTex;
uniform lowp float uSpriteBlurAmount;
uniform mediump vec2 uTcScale;
varying mediump vec2 vTc;
varying lowp vec4 vColor;

const float fSolidThreshold = 0.1;

mediump float ieeV1FpselectMarker()
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

lowp vec3 uhSharpenSprite(in mediump vec2 uv, in lowp vec3 centerRgb, in lowp float centerAlpha)
{
    if (centerAlpha <= 0.20)
    {
        return centerRgb;
    }

    mediump vec2 tcScale = uhSpriteTcScale();

    lowp vec4 north = uhSampleSprite(uv + vec2(0.0, -tcScale.y));
    lowp vec4 south = uhSampleSprite(uv + vec2(0.0,  tcScale.y));
    lowp vec4 west  = uhSampleSprite(uv + vec2(-tcScale.x, 0.0));
    lowp vec4 east  = uhSampleSprite(uv + vec2( tcScale.x, 0.0));
    lowp vec4 nw    = uhSampleSprite(uv + vec2(-tcScale.x, -tcScale.y));
    lowp vec4 ne    = uhSampleSprite(uv + vec2( tcScale.x, -tcScale.y));
    lowp vec4 sw    = uhSampleSprite(uv + vec2(-tcScale.x,  tcScale.y));
    lowp vec4 se    = uhSampleSprite(uv + vec2( tcScale.x,  tcScale.y));

    mediump vec3 blur = vec3(centerRgb) * (4.0 * centerAlpha);
    mediump float weight = 4.0 * centerAlpha;

    blur += vec3(north.rgb) * (2.0 * north.a);
    blur += vec3(south.rgb) * (2.0 * south.a);
    blur += vec3(west.rgb)  * (2.0 * west.a);
    blur += vec3(east.rgb)  * (2.0 * east.a);
    weight += 2.0 * (north.a + south.a + west.a + east.a);

    blur += vec3(nw.rgb) * nw.a;
    blur += vec3(ne.rgb) * ne.a;
    blur += vec3(sw.rgb) * sw.a;
    blur += vec3(se.rgb) * se.a;
    weight += nw.a + ne.a + sw.a + se.a;

    if (weight <= 0.001)
    {
        return centerRgb;
    }

    mediump vec3 blurRgb = blur / weight;
    mediump vec3 detail = vec3(centerRgb) - blurRgb;
    mediump float localContrast = max(abs(detail.r), max(abs(detail.g), abs(detail.b)));
    mediump float edgeGain = smoothstep(0.01, 0.10, localContrast);
    mediump float strength = mix(1.35, 2.60, edgeGain);
    mediump vec3 sharpened = vec3(centerRgb) + detail * strength;

    lowp vec3 minRgb = centerRgb;
    lowp vec3 maxRgb = centerRgb;
    if (north.a > 0.05) { minRgb = min(minRgb, north.rgb); maxRgb = max(maxRgb, north.rgb); }
    if (south.a > 0.05) { minRgb = min(minRgb, south.rgb); maxRgb = max(maxRgb, south.rgb); }
    if (west.a  > 0.05) { minRgb = min(minRgb, west.rgb);  maxRgb = max(maxRgb, west.rgb); }
    if (east.a  > 0.05) { minRgb = min(minRgb, east.rgb);  maxRgb = max(maxRgb, east.rgb); }
    if (nw.a    > 0.05) { minRgb = min(minRgb, nw.rgb);    maxRgb = max(maxRgb, nw.rgb); }
    if (ne.a    > 0.05) { minRgb = min(minRgb, ne.rgb);    maxRgb = max(maxRgb, ne.rgb); }
    if (sw.a    > 0.05) { minRgb = min(minRgb, sw.rgb);    maxRgb = max(maxRgb, sw.rgb); }
    if (se.a    > 0.05) { minRgb = min(minRgb, se.rgb);    maxRgb = max(maxRgb, se.rgb); }

    mediump float clampSlack = mix(0.02, 0.09, edgeGain);
    sharpened = clamp(sharpened, vec3(minRgb) - vec3(clampSlack), vec3(maxRgb) + vec3(clampSlack));

    mediump float sharpenMix = smoothstep(0.28, 0.95, centerAlpha);
    return mix(centerRgb, vec3(sharpened), sharpenMix);
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

void main()
{
    mediump vec2 tcScale = uhSpriteTcScale();
    vec4 texColor = texture2D(uTex, vTc);
    if (texColor.a > fSolidThreshold)
    {
        lowp vec3 reconstructionRgb = texColor.rgb;
        if (uhHasTcScale())
        {
            lowp float reconstructionMix = smoothstep(0.45, 0.95, texColor.a);
            reconstructionMix = clamp(reconstructionMix * 0.35 + ieeV1FpselectMarker(), 0.0, 1.0);
            reconstructionRgb = mix(texColor.rgb, uhSampleEasu(vTc), reconstructionMix);
        }
        lowp vec3 enhancedRgb = uhSharpenSprite(vTc, reconstructionRgb, texColor.a);
        gl_FragColor = mix(vColor, vec4(enhancedRgb, texColor.a), texColor.a);
        return;
    }

    int x;
    int y;
    int kSize = 3;
    float minDist = float(kSize) + 1.0;
    for (x = -kSize; x <= kSize; x++)
    {
        for (y = -kSize; y <= kSize; y++)
        {
            vec2 sampleCoord = vTc + vec2(float(x), float(y)) * tcScale;
            sampleCoord = sampleCoord / tcScale;
            sampleCoord = sampleCoord + vec2(0.5, 0.5);
            sampleCoord.x = float(int(sampleCoord.x));
            sampleCoord.y = float(int(sampleCoord.y));
            sampleCoord = sampleCoord + vec2(0.5, 0.5);
            sampleCoord = sampleCoord * tcScale;

            vec2 coordDiff = abs(vTc - sampleCoord) / tcScale;

            vec4 texSample = texture2D(uTex, sampleCoord);
            if (texSample.a > fSolidThreshold)
            {
                float distance = sqrt((coordDiff.x * coordDiff.x) + (coordDiff.y * coordDiff.y));
                minDist = min(minDist, distance);
            }
        }
    }

    minDist = max(0.0, minDist - 1.1);
    minDist = minDist / float(kSize);
    gl_FragColor = mix(vColor, vec4(0, 0, 0, 0), minDist);
}
