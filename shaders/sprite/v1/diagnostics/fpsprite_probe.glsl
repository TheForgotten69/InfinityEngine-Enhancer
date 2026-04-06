// IEE_DIAG_FPSPRITE_PROBE
uniform lowp sampler2D uTex;
uniform lowp float uSpriteBlurAmount;
uniform mediump vec2 uTcScale;
varying mediump vec2 vTc;
varying lowp vec4 vColor;

lowp float ieeProbeFpspriteMarker()
{
    return 0.0;
}

bool uhHasTcScale()
{
    return uTcScale.x > 0.0 && uTcScale.y > 0.0;
}

void main()
{
    lowp vec4 texColor = texture2D(uTex, vTc);
    if (texColor.a <= 0.01)
    {
        gl_FragColor = texColor;
        return;
    }

    lowp vec3 tint = uhHasTcScale() ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
    gl_FragColor = vec4(mix(texColor.rgb, tint, 0.7) + vec3(ieeProbeFpspriteMarker()), texColor.a);
}
