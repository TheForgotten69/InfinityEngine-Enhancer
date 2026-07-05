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
uniform lowp	sampler2D	uIeeAreaMask;      // unit 2: fine liquid mask, 8px/texel
uniform lowp	sampler2D	uIeeNormalMap;     // unit 3: tiling water normal map
uniform lowp	sampler2D	uIeeDudvMap;       // unit 4: tiling DuDv distortion map
uniform lowp	sampler2D	uIeeFoamMap;       // unit 5: tiling foam mask

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

// Water surface normal from two drifting layers of the tiling normal map.
// Different scales + opposite drift kill visible repetition.
vec3 ieeWaterNormal(vec2 worldPos, float t)
{
	vec2 drift1 = vec2(t * 3.1, t * 2.2);
	vec2 drift2 = vec2(-t * 4.4, t * 1.6);
	// DuDv jitter breaks up the linear scroll so the layers feel fluid.
	vec2 dudv = texture2D(uIeeDudvMap, worldPos / 384.0 + vec2(t * 0.015, -t * 0.01)).rg * 2.0 - 1.0;
	vec3 n1 = texture2D(uIeeNormalMap, (worldPos + drift1 + dudv * 9.0) / 256.0).rgb * 2.0 - 1.0;
	vec3 n2 = texture2D(uIeeNormalMap, (worldPos + drift2 - dudv * 6.0) / 117.0).rgb * 2.0 - 1.0;
	vec3 n = vec3(n1.xy + n2.xy * 0.6, n1.z * n2.z);
	return normalize(vec3(n.x, n.y, max(n.z, 0.3) * 2.2));
}

// Liquid coverage (0 = land, 1 = liquid) at a world position. The mask is the
// painted overlay-tile silhouette at 8px/texel, so this IS the water contour.
float ieeLiquidAt(vec2 worldPos)
{
	float mode = texture2D(uIeeAreaMask, worldPos * uIeeWorldSizeInv).r * 255.0;
	return mode > 0.5 ? 1.0 : 0.0;
}

// Soft coverage: center tap plus 4 taps one mask texel (8px) away. 1 inside
// the painted water, 0 on land, a one-texel soft rim along the contour.
float ieeCoverage(vec2 worldPos)
{
	float cover = ieeLiquidAt(worldPos);
	cover += ieeLiquidAt(worldPos + vec2( 8.0, 0.0));
	cover += ieeLiquidAt(worldPos + vec2(-8.0, 0.0));
	cover += ieeLiquidAt(worldPos + vec2(0.0,  8.0));
	cover += ieeLiquidAt(worldPos + vec2(0.0, -8.0));
	return cover / 5.0;
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

	// v16 alpha-contour probe (uIeeEnabled = 2.0, was the alignment gradient —
	// alignment passed v7). Tests the theory that the base tile's texture
	// alpha IS the painted water contour inside flagged cells:
	//   magenta = transparent texel (water hole -> real water)
	//   dark blue = opaque texel (painted land art)
	//   untinted = unflagged cell
	// Expected when correct: magenta traces the exact painted silhouette
	// inside the blue-zone cells. The WATER_ALPHA secondary pass (vColor.a
	// well below 1) is discarded and flagged output is forced opaque so the
	// base-tile draw's reading stays visible over the underneath water tile.
	if (uIeeEnabled > 1.5)
	{
		vec4 art = seamSample(vTc);
		if (cellMode > 0.5)
		{
			if (vColor.a < 0.9)
			{
				discard;
			}
			float hole = 1.0 - art.a;
			vec3 dbg = mix(vec3(0.05, 0.10, 0.55), vec3(1.0, 0.0, 0.6), hole);
			gl_FragColor = vec4(mix(art.rgb * vColor.rgb, dbg, 0.8), 1.0);
		}
		else
		{
			vec4 base = art * vColor;
			gl_FragColor = vec4(base.rgb, base.a);
		}
		return;
	}

	vec4 texColor = seamSample(vTc);

	// Water mask (v16-verified): inside flagged cells the engine draws the
	// base tile with TRANSPARENT pixels exactly where water composites
	// through — the tile's own alpha is the painted contour. Restrict to the
	// opaque base pass: the WATER_ALPHA secondary pass and fades carry
	// vColor.a < 1 and must stay vanilla.
	float waterMask = 0.0;
	if (uIeeEnabled > 0.5 && vColor.a > 0.9)
	{
		float cellSoft = smoothstep(0.02, 0.30, ieeCoverage(worldPos));
		waterMask = (1.0 - texColor.a) * cellSoft;
	}

	if (waterMask > 0.02)
	{
		float t = uIeeTime;
		vec3 normal = ieeWaterNormal(worldPos, t);
		// Wave "height" proxy from the normal's tilt: drives depth + caps.
		float waveH = clamp(0.5 + (normal.x + normal.y) * 1.4, 0.0, 1.0);

		// IE shadows fall toward the lower-left: light comes from the top-right.
		vec3 lightDir = normalize(vec3(0.45, -0.6, 0.66));
		float diffuse = clamp(dot(normal, lightDir), 0.0, 1.0);
		// Specular: tight highlight for sun glitter on wave crests.
		vec3 viewDir = vec3(0.0, 0.0, 1.0);
		vec3 halfVec = normalize(lightDir + viewDir);
		float spec = pow(clamp(dot(normal, halfVec), 0.0, 1.0), 64.0);

		// Palette: water-hole texels carry no art color (transparent = black),
		// so grade from a neutral tone there — the engine's own secondary
		// WATER_ALPHA pass then blends the AUTHORED painted water on top and
		// restores each area's palette (sea teal, river brown, ...). Partially
		// opaque contour pixels still contribute their real art color.
		vec3 artColor = mix(vec3(0.5), texColor.rgb, texColor.a);
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

		// Animated shoreline foam from the foam texture, breathing on the band.
		float foamBand = smoothstep(0.60, 0.95, shore + 0.12 * sin(t * 1.3 + waveH * 6.0));
		float foamTex = texture2D(uIeeFoamMap, (worldPos + vec2(t * 6.0, -t * 4.0)) / 220.0).r;
		float foam = foamBand * smoothstep(0.30, 0.85, foamTex);
		// Crest foam: texture-shaped caps on the most tilted waves.
		foam += smoothstep(0.80, 0.95, waveH) * foamTex * 0.7;
		water = mix(water, foamColor, clamp(foam, 0.0, 1.0) * 0.75);

		// Sun glitter + lava glow.
		water += spec * mix(0.35, 0.9, waveH);
		if (emissive > 0.0)
		{
			float pulse = 0.5 + 0.5 * sin(t * 1.1 + waveH * 6.0);
			water += deepColor * emissive * (0.6 + 0.8 * pulse);
		}

		// Replace by the painted contour: hole pixels become our water (their
		// art rgb is black anyway); the seamSample bilinear already softens
		// the alpha edge. Raise the output alpha so our water covers the
		// engine's generic animated tile drawn underneath the cell.
		texColor.rgb = mix(texColor.rgb, water, waterMask);
		texColor.a = max(texColor.a, waterMask);
	}

	texColor = texColor * vColor;

	float grey = dot(texColor.rgb, vec3(0.299, 0.587, 0.114));
	vec3 tone = grey * uColorTone.rgb;

	gl_FragColor = vec4(mix(texColor.rgb, tone, uColorTone.a), texColor.a);
}
