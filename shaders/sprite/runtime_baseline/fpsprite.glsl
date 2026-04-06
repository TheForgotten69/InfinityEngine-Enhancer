uniform lowp sampler2D uTex;
uniform lowp float uSpriteBlurAmount;
varying mediump vec2 vTc;
varying lowp vec4 vColor;

mediump float normpdf(in mediump float x, in mediump float sigma)
{
    return 0.39894 * exp(-0.5 * x * x / (sigma * sigma)) / sigma;
}

void main()
{
    const int mSize = 5;
    const int kSize = (mSize - 1) / 2;
    int solidPixelCount = 0;
    mediump float kernel[mSize];
    lowp vec4 blur_colour = vec4(0.0);

    mediump float sigma = 7.0;
    mediump float Z = 0.0;
    for (int j = 0; j <= kSize; ++j)
    {
        kernel[kSize + j] = kernel[kSize - j] = normpdf(float(j), sigma);
    }

    for (int j = 0; j < mSize; ++j)
    {
        Z += kernel[j];
    }

    for (int i = -kSize; i <= kSize; ++i)
    {
        for (int j = -kSize; j <= kSize; ++j)
        {
            lowp vec4 sample = texture2D(uTex, vTc + vec2(float(i) * 0.0005, float(j) * 0.0005));
            blur_colour += kernel[kSize + j] * kernel[kSize + i] * sample;
            if (sample.a > 0.5)
            {
                solidPixelCount = solidPixelCount + 1;
            }
        }
    }

    if (solidPixelCount > 0)
    {
        blur_colour.a = blur_colour.a * uSpriteBlurAmount;
    }
    blur_colour = blur_colour / (Z * Z);

    vec4 tex_color = texture2D(uTex, vTc);
    gl_FragColor = mix(blur_colour, tex_color, tex_color.a);
}
