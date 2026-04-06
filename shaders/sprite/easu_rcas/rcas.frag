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
uniform float RcasSharpness;
uniform sampler2D Texture;
COMPAT_VARYING vec4 TEX0;

#define Source Texture
#define vTexCoord TEX0.xy
#define FSR_RCAS_LIMIT (0.25 - (1.0 / 16.0))

vec4 FsrRcasLoadF(vec2 p) {
    return COMPAT_TEXTURE(Source, p / OutputSize.xy);
}

void FsrRcasCon(out float con, float sharpness) {
    con = exp2(-sharpness);
}

vec3 FsrRcasF(vec2 ip, float con)
{
    vec3 b = FsrRcasLoadF(ip + vec2( 0.0, -1.0)).rgb;
    vec3 d = FsrRcasLoadF(ip + vec2(-1.0,  0.0)).rgb;
    vec3 e = FsrRcasLoadF(ip).rgb;
    vec3 f = FsrRcasLoadF(ip + vec2( 1.0,  0.0)).rgb;
    vec3 h = FsrRcasLoadF(ip + vec2( 0.0,  1.0)).rgb;

    vec3 mn4 = min(b, min(f, h));
    vec3 mx4 = max(b, max(f, h));
    vec3 hitMin = mn4 / max(4.0 * mx4, vec3(1e-5));
    vec3 hitMax = (vec3(1.0) - mx4) / max(4.0 * mn4 - 4.0, vec3(-3.9999));
    vec3 lobeRGB = max(-hitMin, hitMax);
    float lobe = max(-FSR_RCAS_LIMIT, min(max(lobeRGB.r, max(lobeRGB.g, lobeRGB.b)), 0.0)) * con;
    return (lobe * (b + d + h + f) + e) / (4.0 * lobe + 1.0);
}

void main()
{
    vec2 fragCoord = vTexCoord * OutputSize.xy;
    float con;
    FsrRcasCon(con, RcasSharpness);
    vec3 rgb = FsrRcasF(fragCoord, con);
    float alpha = COMPAT_TEXTURE(Source, vTexCoord).a;
    FragColor = vec4(rgb, alpha);
}
