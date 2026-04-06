#if __VERSION__ >= 130
#define COMPAT_VARYING in
#define COMPAT_TEXTURE texture
out vec4 FragColor;
#else
#define COMPAT_VARYING varying
#define COMPAT_TEXTURE texture2D
#define FragColor gl_FragColor
#endif

#ifdef GL_ES
#ifdef GL_FRAGMENT_PRECISION_HIGH
precision highp float;
#else
precision mediump float;
#endif
#endif

uniform int FrameCount;
uniform vec2 OutputSize;
uniform vec2 TextureSize;
uniform vec2 InputSize;
uniform sampler2D Texture;
COMPAT_VARYING vec4 TEX0;

#define Source Texture
#define vTexCoord TEX0.xy
#define SourceSize vec4(TextureSize, 1.0 / TextureSize)

vec4 SampleSource(vec2 p) {
    return COMPAT_TEXTURE(Source, p);
}

void FsrEasuCon(
    out vec4 con0,
    out vec4 con1,
    out vec4 con2,
    out vec4 con3,
    vec2 inputViewportInPixels,
    vec2 inputSizeInPixels,
    vec2 outputSizeInPixels)
{
    con0 = vec4(
        inputViewportInPixels.x / outputSizeInPixels.x,
        inputViewportInPixels.y / outputSizeInPixels.y,
        0.5 * inputViewportInPixels.x / outputSizeInPixels.x - 0.5,
        0.5 * inputViewportInPixels.y / outputSizeInPixels.y - 0.5
    );
    con1 = vec4(1.0, 1.0, 1.0, -1.0) / inputSizeInPixels.xyxy;
    con2 = vec4(-1.0, 2.0, 1.0, 2.0) / inputSizeInPixels.xyxy;
    con3 = vec4(0.0, 4.0, 0.0, 0.0) / inputSizeInPixels.xyxy;
}

void FsrEasuTapF(
    inout vec3 aC,
    inout float aW,
    vec2 off,
    vec2 dir,
    vec2 len,
    float lob,
    float clp,
    vec3 c)
{
    vec2 v = vec2(dot(off, dir), dot(off, vec2(-dir.y, dir.x)));
    v *= len;
    float d2 = min(dot(v, v), clp);
    float wB = 0.4 * d2 - 1.0;
    float wA = lob * d2 - 1.0;
    wB *= wB;
    wA *= wA;
    wB = 1.5625 * wB - 0.5625;
    float w = wB * wA;
    aC += c * w;
    aW += w;
}

void FsrEasuSetF(
    inout vec2 dir,
    inout float len,
    float w,
    float lA, float lB, float lC, float lD, float lE)
{
    float lenX = max(abs(lD - lC), abs(lC - lB));
    float dirX = lD - lB;
    dir.x += dirX * w;
    lenX = clamp(abs(dirX) / max(lenX, 1e-5), 0.0, 1.0);
    lenX *= lenX;
    len += lenX * w;

    float lenY = max(abs(lE - lC), abs(lC - lA));
    float dirY = lE - lA;
    dir.y += dirY * w;
    lenY = clamp(abs(dirY) / max(lenY, 1e-5), 0.0, 1.0);
    lenY *= lenY;
    len += lenY * w;
}

void FsrEasuF(
    out vec3 pix,
    vec2 ip,
    vec4 con0,
    vec4 con1,
    vec4 con2,
    vec4 con3)
{
    vec2 pp = ip * con0.xy + con0.zw;
    vec2 fp = floor(pp);
    pp -= fp;

    vec2 p0 = fp * con1.xy + con1.zw;
    vec2 p1 = p0 + con2.xy;
    vec2 p2 = p0 + con2.zw;
    vec2 p3 = p0 + con3.xy;

    vec4 off = vec4(-0.5, 0.5, -0.5, 0.5) * con1.xxyy;

    vec3 bC = SampleSource(p0 + off.xw).rgb; float bL = bC.g + 0.5 * (bC.r + bC.b);
    vec3 cC = SampleSource(p0 + off.yw).rgb; float cL = cC.g + 0.5 * (cC.r + cC.b);
    vec3 iC = SampleSource(p1 + off.xw).rgb; float iL = iC.g + 0.5 * (iC.r + iC.b);
    vec3 jC = SampleSource(p1 + off.yw).rgb; float jL = jC.g + 0.5 * (jC.r + jC.b);
    vec3 fC = SampleSource(p1 + off.yz).rgb; float fL = fC.g + 0.5 * (fC.r + fC.b);
    vec3 eC = SampleSource(p1 + off.xz).rgb; float eL = eC.g + 0.5 * (eC.r + eC.b);
    vec3 kC = SampleSource(p2 + off.xw).rgb; float kL = kC.g + 0.5 * (kC.r + kC.b);
    vec3 lC = SampleSource(p2 + off.yw).rgb; float lL = lC.g + 0.5 * (lC.r + lC.b);
    vec3 hC = SampleSource(p2 + off.yz).rgb; float hL = hC.g + 0.5 * (hC.r + hC.b);
    vec3 gC = SampleSource(p2 + off.xz).rgb; float gL = gC.g + 0.5 * (gC.r + gC.b);
    vec3 oC = SampleSource(p3 + off.yz).rgb; float oL = oC.g + 0.5 * (oC.r + oC.b);
    vec3 nC = SampleSource(p3 + off.xz).rgb; float nL = nC.g + 0.5 * (nC.r + nC.b);

    vec2 dir = vec2(0.0);
    float len = 0.0;
    FsrEasuSetF(dir, len, (1.0 - pp.x) * (1.0 - pp.y), bL, eL, fL, gL, jL);
    FsrEasuSetF(dir, len, pp.x * (1.0 - pp.y), cL, fL, gL, hL, kL);
    FsrEasuSetF(dir, len, (1.0 - pp.x) * pp.y, fL, iL, jL, kL, nL);
    FsrEasuSetF(dir, len, pp.x * pp.y, gL, jL, kL, lL, oL);

    vec2 dir2 = dir * dir;
    float dirR = dir2.x + dir2.y;
    bool zro = dirR < (1.0 / 32768.0);
    dirR = inversesqrt(max(dirR, 1.0 / 32768.0));
    dir = zro ? vec2(1.0, 0.0) : dir * dirR;
    len = 0.5 * len;
    len *= len;
    float stretch = dot(dir, dir) / max(max(abs(dir.x), abs(dir.y)), 1e-5);
    vec2 len2 = vec2(1.0 + (stretch - 1.0) * len, 1.0 - 0.5 * len);
    float lob = 0.5 - 0.29 * len;
    float clp = 1.0 / lob;

    vec3 min4 = min(min(fC, gC), min(jC, kC));
    vec3 max4 = max(max(fC, gC), max(jC, kC));
    vec3 aC = vec3(0.0);
    float aW = 0.0;
    FsrEasuTapF(aC, aW, vec2( 0.0, -1.0) - pp, dir, len2, lob, clp, bC);
    FsrEasuTapF(aC, aW, vec2( 1.0, -1.0) - pp, dir, len2, lob, clp, cC);
    FsrEasuTapF(aC, aW, vec2(-1.0,  1.0) - pp, dir, len2, lob, clp, iC);
    FsrEasuTapF(aC, aW, vec2( 0.0,  1.0) - pp, dir, len2, lob, clp, jC);
    FsrEasuTapF(aC, aW, vec2( 0.0,  0.0) - pp, dir, len2, lob, clp, fC);
    FsrEasuTapF(aC, aW, vec2(-1.0,  0.0) - pp, dir, len2, lob, clp, eC);
    FsrEasuTapF(aC, aW, vec2( 1.0,  1.0) - pp, dir, len2, lob, clp, kC);
    FsrEasuTapF(aC, aW, vec2( 2.0,  1.0) - pp, dir, len2, lob, clp, lC);
    FsrEasuTapF(aC, aW, vec2( 2.0,  0.0) - pp, dir, len2, lob, clp, hC);
    FsrEasuTapF(aC, aW, vec2( 1.0,  0.0) - pp, dir, len2, lob, clp, gC);
    FsrEasuTapF(aC, aW, vec2( 1.0,  2.0) - pp, dir, len2, lob, clp, oC);
    FsrEasuTapF(aC, aW, vec2( 0.0,  2.0) - pp, dir, len2, lob, clp, nC);
    pix = min(max4, max(min4, aC / max(aW, 1e-5)));
}

void main()
{
    vec4 con0;
    vec4 con1;
    vec4 con2;
    vec4 con3;
    vec2 fragCoord = vTexCoord * OutputSize.xy;
    vec3 rgb;
    FsrEasuCon(con0, con1, con2, con3, SourceSize.xy, SourceSize.xy, OutputSize.xy);
    FsrEasuF(rgb, fragCoord, con0, con1, con2, con3);
    float alpha = SampleSource(vTexCoord).a;
    FragColor = vec4(rgb, alpha);
}
