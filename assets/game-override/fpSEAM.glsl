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
uniform highp	vec2		uIeeZoom;          // physical px per world px, per axis
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

// --- IEE water helpers (world-space, GLSL 110) ---

float ieeHash(vec2 p)
{
	p = fract(p * vec2(123.34, 345.45));
	p += dot(p, p + 34.345);
	return fract(p.x * p.y);
}

// Smooth value noise over world position.
float ieeNoise(vec2 p)
{
	vec2 i = floor(p);
	vec2 f = fract(p);
	vec2 u = f * f * (3.0 - 2.0 * f);
	float a = ieeHash(i);
	float b = ieeHash(i + vec2(1.0, 0.0));
	float c = ieeHash(i + vec2(0.0, 1.0));
	float d = ieeHash(i + vec2(1.0, 1.0));
	return mix(mix(a, b, u.x), mix(c, d, u.x), u.y);
}

// Animated 3-octave wave height field. Two drifting layers kill repetition.
float ieeWaveHeight(vec2 worldPos, float t)
{
	vec2 p = worldPos * 0.035;
	float h = 0.0;
	h += 0.55 * ieeNoise(p + vec2(t * 0.06, t * 0.045));
	h += 0.30 * ieeNoise(p * 2.3 - vec2(t * 0.085, t * 0.03));
	h += 0.15 * ieeNoise(p * 5.1 + vec2(t * 0.12, -t * 0.09));
	return h;
}

// Liquid coverage (0 = land, 1 = liquid) at a world position.
float ieeLiquidAt(vec2 worldPos)
{
	float mode = texture2D(uIeeAreaMask, worldPos * uIeeWorldSizeInv).r * 255.0;
	return mode > 0.5 ? 1.0 : 0.0;
}

// Continuous liquid coverage: manual bilinear over the 64px cell grid.
// 1 deep inside water, 0 on land, smooth curve through shoreline cells —
// the "one surface" field that kills the per-cell squares.
float ieeCoverage(vec2 worldPos)
{
	vec2 cell = worldPos / 64.0 - 0.5;
	vec2 base = (floor(cell) + 0.5) * 64.0;
	vec2 f = fract(cell);
	float c00 = ieeLiquidAt(base);
	float c10 = ieeLiquidAt(base + vec2(64.0, 0.0));
	float c01 = ieeLiquidAt(base + vec2(0.0, 64.0));
	float c11 = ieeLiquidAt(base + vec2(64.0, 64.0));
	return mix(mix(c00, c10, f.x), mix(c01, c11, f.x), f.y);
}

// Smoothed shore proximity: 0 deep inside water, ~1 at the land boundary.
float ieeShoreFactor(vec2 worldPos)
{
	float cover = ieeCoverage(worldPos) * 2.0;
	cover += ieeCoverage(worldPos + vec2( 44.0, 0.0));
	cover += ieeCoverage(worldPos + vec2(-44.0, 0.0));
	cover += ieeCoverage(worldPos + vec2(0.0,  44.0));
	cover += ieeCoverage(worldPos + vec2(0.0, -44.0));
	return 1.0 - clamp((cover - 1.0) / 5.0, 0.0, 1.0);
}

void main()
{
	// World position of this fragment: gl_FragCoord is physical pixels with a
	// bottom-left origin; the engine's world is top-left, scaled by zoom.
	vec2 screenPx = vec2(gl_FragCoord.x, uIeeViewport.y - gl_FragCoord.y);
	vec2 worldPos = uIeeScroll + screenPx / max(uIeeZoom, vec2(0.0001, 0.0001));

	// Liquid mode of the WED cell under this fragment (R8: 0..5 -> 0..5/255).
	float cellMode = 0.0;
	if (uIeeEnabled > 0.5)
	{
		cellMode = floor(texture2D(uIeeAreaMask, worldPos * uIeeWorldSizeInv).r * 255.0 + 0.5);
	}

	// Alignment debug (uIeeEnabled = 2.0): renders the derived mask coordinate
	// as a gradient so ONE screenshot calibrates the whole world transform:
	//   red   = mask u (0 at world left -> 1 at world right)
	//   green = mask v (0 at world top  -> 1 at world bottom)
	//   blue  = 1 on liquid-flagged cells
	// Expected when correct: red ramps left->right across the WHOLE map,
	// green ramps top->bottom, blue hugs the water. A mirrored ramp = flipped
	// axis; a shifted ramp = scroll/viewport offset; a too-steep ramp = zoom.
	if (uIeeEnabled > 1.5)
	{
		vec2 maskUv = worldPos * uIeeWorldSizeInv;
		vec4 base = seamSample(vTc);
		base = base * vColor;
		vec3 dbg = vec3(fract(maskUv.x * 4.0), fract(maskUv.y * 4.0), cellMode > 0.5 ? 1.0 : 0.0);
		base.rgb = mix(base.rgb, dbg, 0.65);
		gl_FragColor = vec4(base.rgb, base.a);
		return;
	}

	vec4 texColor = seamSample(vTc);

	// Continuous coverage drives a soft replacement: the water is one surface
	// whose boundary curves through shoreline cells (no per-cell squares).
	float coverage = 0.0;
	if (uIeeEnabled > 0.5)
	{
		coverage = ieeCoverage(worldPos);
	}

	if (coverage > 0.02)
	{
		float t = uIeeTime;
		float waveH = ieeWaveHeight(worldPos, t);

		// Surface normal from the height field (finite differences).
		float hx = ieeWaveHeight(worldPos + vec2(6.0, 0.0), t) - waveH;
		float hy = ieeWaveHeight(worldPos + vec2(0.0, 6.0), t) - waveH;
		vec3 normal = normalize(vec3(-hx * 4.0, -hy * 4.0, 1.0));

		// IE shadows fall toward the lower-left: light comes from the top-right.
		vec3 lightDir = normalize(vec3(0.45, -0.6, 0.66));
		float diffuse = clamp(dot(normal, lightDir), 0.0, 1.0);
		// Specular: tight highlight for sun glitter on wave crests.
		vec3 viewDir = vec3(0.0, 0.0, 1.0);
		vec3 halfVec = normalize(lightDir + viewDir);
		float spec = pow(clamp(dot(normal, halfVec), 0.0, 1.0), 48.0);

		// Palette derived from the AUTHORED art so every body of water keeps
		// its area's painted character (sea teal, river brown, ...): the art
		// color is graded into deep and shallow tones instead of hardcoding.
		vec3 artColor = texColor.rgb;
		float artLuma = dot(artColor, vec3(0.299, 0.587, 0.114));
		// Normalize the art tone so dark night pixels still yield a usable hue;
		// soften extremes so bright rim pixels can't smear the palette.
		vec3 artTone = artColor / max(artLuma, 0.06);
		artTone = mix(vec3(1.0), artTone, 0.75);
		vec3 deepColor    = artTone * 0.10;
		vec3 shallowColor = artTone * 0.32;
		vec3 foamColor    = mix(vec3(0.88), artTone * 0.9, 0.10);
		float emissive    = 0.0;
		if (cellMode > 1.5 && cellMode < 2.5)
		{
			// Lava keeps an authored-orange emissive identity.
			deepColor = vec3(0.32, 0.04, 0.00);
			shallowColor = vec3(0.85, 0.28, 0.02);
			foamColor = vec3(1.00, 0.78, 0.25);
			emissive = 0.5;
		}

		// Shore proximity: brightens shallows and drives the foam band.
		float shore = ieeShoreFactor(worldPos);

		// Water body: deep->shallow by wave height and shore, lit by the normal.
		float depthMix = clamp(waveH * 0.65 + shore * 0.55, 0.0, 1.0);
		vec3 water = mix(deepColor, shallowColor, depthMix);
		water *= 0.75 + 0.45 * diffuse;

		// Large-scale patchiness so open water does not read flat.
		float patch = ieeNoise(worldPos * 0.011 + vec2(t * 0.012, -t * 0.008));
		water = mix(water, mix(deepColor, shallowColor, patch), 0.30);
		float patch2 = ieeNoise(worldPos * 0.004);
		water *= 0.88 + 0.24 * patch2;

		// Animated shoreline foam: a noisy band that breathes against the land.
		float foamBand = smoothstep(0.62, 0.95, shore + 0.15 * sin(t * 1.4 + waveH * 9.0));
		float foamNoise = ieeNoise(worldPos * 0.22 + vec2(t * 0.35, -t * 0.2));
		float foam = foamBand * smoothstep(0.40, 0.78, foamNoise);
		// Crest foam: thin caps on the highest waves, away from shore too.
		foam += smoothstep(0.78, 0.92, waveH) * 0.5;
		water = mix(water, foamColor, clamp(foam, 0.0, 1.0) * 0.7);

		// Sun glitter + lava glow.
		water += spec * mix(0.35, 0.9, waveH);
		if (emissive > 0.0)
		{
			float pulse = 0.5 + 0.5 * sin(t * 1.1 + waveH * 6.0);
			water += deepColor * emissive * (0.6 + 0.8 * pulse);
		}

		// Soft replacement by coverage: full water inside the body, a smooth
		// blend through the boundary cells, untouched land beyond.
		float waterAlpha = smoothstep(0.30, 0.7, coverage);

		// Art-luma gate: a live pixel-resolution mask. Painted water is dark;
		// pool rims, pavement, and flowers inside coarse mask cells are bright
		// and must never be flooded. Lava is exempt (authored bright).
		if (emissive <= 0.0)
		{
			waterAlpha *= 1.0 - smoothstep(0.40, 0.62, artLuma);
		}

		texColor.rgb = mix(texColor.rgb, water, waterAlpha);
	}

	texColor = texColor * vColor;

	float grey = dot(texColor.rgb, vec3(0.299, 0.587, 0.114));
	vec3 tone = grey * uColorTone.rgb;

	gl_FragColor = vec4(mix(texColor.rgb, tone, uColorTone.a), texColor.a);
}
