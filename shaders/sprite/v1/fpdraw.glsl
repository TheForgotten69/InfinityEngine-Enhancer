#if !defined(GL_ES)
#define highp
#define mediump
#define lowp
#else
precision highp float;
#endif

// fpDraw.glsl
uniform lowp sampler2D uTex;
uniform highp vec4 uColorTone;
varying mediump vec2 vTc;
varying lowp vec4 vColor;

void main()
{
    lowp vec4 texColor = texture2D(uTex, vTc) * vColor;
    mediump float grey = dot(texColor.rgb, vec3(0.299, 0.587, 0.114));
    lowp vec3 tone = grey * uColorTone.rgb;
    gl_FragColor = vec4(mix(texColor.rgb, tone, uColorTone.a), texColor.a);
}
