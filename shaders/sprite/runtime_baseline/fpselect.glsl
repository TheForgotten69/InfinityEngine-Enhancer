uniform lowp sampler2D uTex;
uniform lowp float uSpriteBlurAmount;
uniform mediump vec2 uTcScale;
varying mediump vec2 vTc;
varying lowp vec4 vColor;

mediump float normpdf(in mediump float x, in mediump float sigma)
{
    return 0.39894 * exp(-0.5 * x * x / (sigma * sigma)) / sigma;
}

const float fSolidThreshold = 0.1;

void main()
{
    vec4 texColor = texture2D(uTex, vTc);
    if (texColor.a > fSolidThreshold)
    {
        gl_FragColor = mix(vColor, texColor, texColor.a);
        return;
    }

    int x, y;
    int kSize = 3;
    float minDist = float(kSize) + 1.0;
    for (x = -kSize; x <= kSize; x++)
    {
        for (y = -kSize; y <= kSize; y++)
        {
            vec2 sampleCoord = vTc + vec2(float(x), float(y)) * uTcScale;
            sampleCoord = sampleCoord / uTcScale;
            sampleCoord = sampleCoord + vec2(0.5, 0.5);
            sampleCoord.x = float(int(sampleCoord.x));
            sampleCoord.y = float(int(sampleCoord.y));
            sampleCoord = sampleCoord + vec2(0.5, 0.5);
            sampleCoord = sampleCoord * uTcScale;

            vec2 coordDiff = abs(vTc - sampleCoord) / uTcScale;

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
