#if __VERSION__ >= 130
#define COMPAT_VARYING in
#define COMPAT_TEXTURE texture
out vec4 FragColor;
#else
#define COMPAT_VARYING varying
#define COMPAT_TEXTURE texture2D
#define FragColor gl_FragColor
#endif

uniform sampler2D Texture;
COMPAT_VARYING vec4 TEX0;

void main()
{
    FragColor = COMPAT_TEXTURE(Texture, TEX0.xy);
}
