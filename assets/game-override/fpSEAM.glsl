#if !defined(GL_ES)
#define highp
#define mediump
#define lowp
#else
precision highp float;
#endif

// fpSEAM.glsl
// IEE_V2_FPSEAM_WATER — world-space, WED-mask-gated water.
// uIeeEnabled < 0.5 reproduces the vanilla fpSEAM output exactly.

uniform lowp	sampler2D	uTex;
uniform highp 	vec2 		uTcScale;
uniform highp	vec4		uColorTone;

// --- IEE feed (set by the DLL at program bind + frame tick) ---
uniform highp	float		uIeeEnabled;       // 0/1 master gate (F10)
uniform highp	float		uIeeTime;          // seconds
uniform highp	vec2		uIeeScroll;        // world px of viewport origin
uniform highp	float		uIeeZoom;          // world->screen scale
uniform highp	vec2		uIeeViewport;      // physical px (w, h)
uniform highp	vec2		uIeeWorldSizeInv;  // 1 / world px
uniform lowp	sampler2D	uIeeAreaMask;      // unit 2: liquid mode per WED cell

varying highp	vec2		vTc;
varying highp	vec2		vRef;
varying lowp	vec4		vColor;

bool isBorderColor(vec4 testColor)
{
	if ((testColor.x == 0.0) && (testColor.y == 0.0) && (testColor.z == 0.0))
	{
		return true;
	}
	return false;
}

// Vanilla seam-aware sampling, parameterized on the sample coordinate so the
// water path can ride the same border handling with distorted coords.
vec4 seamSample(vec2 tc)
{
	vec2 texCoordUnbiased = tc / uTcScale;
	vec2 refCoordUnbiased = vRef / uTcScale;

	float fu = fract(texCoordUnbiased.x);
	float fv = fract(texCoordUnbiased.y);

	vec2 texCoordTileLoc = mod(texCoordUnbiased - refCoordUnbiased, 64.0);
	int texCoordTileLocIntx = int(floor(texCoordTileLoc.x));
	int texCoordTileLocInty = int(floor(texCoordTileLoc.y));

	vec4 texColor0 = texture2D(uTex, tc);

	vec4 texColor1 = texture2D(uTex, tc + vec2(uTcScale.x, 0.0));
	vec4 texColor2 = texture2D(uTex, tc + vec2(0.0, uTcScale.y));
	vec4 texColor3 = texture2D(uTex, tc + vec2(uTcScale.x, uTcScale.y));

	bool border0 = isBorderColor(texColor0);
	bool border1 = isBorderColor(texColor1);
	bool border2 = isBorderColor(texColor2);
	bool border3 = isBorderColor(texColor3);

	vec4 texColor = vec4(0.0, 0.0, 0.0, 1.0);

	if (((texCoordTileLocIntx == 0) || (texCoordTileLocIntx >= 63) ||
		 (texCoordTileLocInty == 0) || (texCoordTileLocInty >= 63)) &&
		 (border0 || border1 || border2 || border3) )
	{
		texColor = texColor0;
	}
	else
	{
		if (border1) { texColor1 = texColor0; }
		if (border2) { texColor2 = texColor0; }
		if (border3) { texColor3 = texColor0; }

		vec4 texColorTop = mix(texColor0, texColor1, fu);
		vec4 texColorBottom = mix(texColor2, texColor3, fu);
		texColor = mix(texColorTop, texColorBottom, fv);
	}

	return texColor;
}

float hash21(vec2 p)
{
	p = fract(p * vec2(123.34, 456.21));
	p += dot(p, p + 45.32);
	return fract(p.x * p.y);
}

void main()
{
	// World position of this fragment: gl_FragCoord is physical pixels with a
	// bottom-left origin; the engine's world is top-left, scaled by zoom.
	vec2 screenPx = vec2(gl_FragCoord.x, uIeeViewport.y - gl_FragCoord.y);
	vec2 worldPos = uIeeScroll + screenPx / max(uIeeZoom, 0.0001);

	// Liquid mode of the WED cell under this fragment (R8: 0..5 -> 0..5/255).
	float cellMode = 0.0;
	if (uIeeEnabled > 0.5)
	{
		cellMode = floor(texture2D(uIeeAreaMask, worldPos * uIeeWorldSizeInv).r * 255.0 + 0.5);
	}

	vec2 sampleTc = vTc;
	if (cellMode > 0.5)
	{
		// World-space wave field: continuous across tile and cell boundaries.
		float t = uIeeTime;
		vec2 w = worldPos * 0.085;
		vec2 distort;
		distort.x = sin(w.y * 1.7 + t * 1.1) + 0.5 * sin(w.x * 2.3 - t * 0.7);
		distort.y = cos(w.x * 1.9 - t * 0.9) + 0.5 * cos(w.y * 2.9 + t * 1.3);
		// Damp the distortion to zero near tile edges: |distort| reaches 1.5,
		// times the 1.5 texel factor = 2.25 texels, which would cross into a
		// neighboring (non-adjacent-in-world) atlas tile. The envelope is
		// symmetric at every boundary, so the wave field stays continuous.
		vec2 unb     = vTc / uTcScale;
		vec2 tileLoc = mod(unb - vRef / uTcScale, 64.0);
		vec2 edge    = min(tileLoc, 64.0 - tileLoc);
		float damp   = smoothstep(0.0, 3.5, min(edge.x, edge.y));
		sampleTc = vTc + distort * uTcScale * 1.5 * damp;
	}

	vec4 texColor = seamSample(sampleTc);

	if (cellMode > 0.5)
	{
		float t = uIeeTime;
		vec2 w = worldPos * 0.085;

		// Per-mode tint (TileLiquidMode: 1 water, 2 lava, 3 goo, 4 sewage, 5 swamp).
		vec3 deepTint = vec3(0.10, 0.22, 0.38);
		if (cellMode > 4.5)      { deepTint = vec3(0.16, 0.26, 0.10); } // swamp
		else if (cellMode > 3.5) { deepTint = vec3(0.30, 0.28, 0.08); } // sewage
		else if (cellMode > 2.5) { deepTint = vec3(0.12, 0.28, 0.08); } // goo
		else if (cellMode > 1.5) { deepTint = vec3(0.40, 0.12, 0.02); } // lava

		float swell = 0.5 + 0.5 * sin(w.x * 1.3 + w.y * 1.1 + t * 0.8);
		texColor.rgb = mix(texColor.rgb, texColor.rgb * (0.85 + 0.3 * swell) + deepTint * 0.12, 0.55);

		// Sparse moving glints.
		float glint = hash21(floor(worldPos * 0.25) + floor(t * 2.0));
		if (glint > 0.985)
		{
			texColor.rgb += vec3(0.25);
		}
	}

	texColor = texColor * vColor;

	float grey = dot(texColor.rgb, vec3(0.299, 0.587, 0.114));
	vec3 tone = grey * uColorTone.rgb;

	gl_FragColor = vec4(mix(texColor.rgb, tone, uColorTone.a), texColor.a);
}
