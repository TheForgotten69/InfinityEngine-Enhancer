#if !defined(GL_ES)
#define highp
#define mediump
#define lowp
#else
precision highp float;
#endif

// fpSEAM.glsl
// World-space, WED-mask-gated liquid replacement.
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
uniform highp	vec3		uIeeWaterTint;     // authored water color (avg of the area's water overlay tile)
uniform highp	float		uIeePointCount;    // 0..32 classified ambient-animation points
// Two vec4 slots per point i: [2i] = (baseX, baseY, kind, heightPx) where the
// kind fraction is a palette id (.0 warm body / .1 blue body / .2 glow only);
// [2i+1] = (halfWidthPx, reserved...).
uniform highp	vec4		uIeePoints[64];
uniform lowp	sampler2D	uIeeAreaMask;      // unit 2: one liquid mode per 64px WED cell
uniform lowp	sampler2D	uIeeNormalMap;     // unit 3: tiling water normal map
uniform lowp	sampler2D	uIeeDudvMap;       // unit 4: tiling DuDv distortion map
uniform lowp	sampler2D	uIeeFoamMap;       // unit 5: tiling foam mask
uniform lowp	sampler2D	uIeeNoiseMap;      // unit 6: tiling FBM octaves (R/G smooth, B high-freq, A blobs)

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

// Palette-derived tint arrives in linear light. Engine tile textures are
// legacy encoded RGB, so water grading converts explicitly and re-encodes only
// the affected contour; the vanilla/off path remains byte-for-byte unchanged.
vec3 ieeSrgbToLinear(vec3 color)
{
	vec3 low = color / 12.92;
	vec3 high = pow((color + vec3(0.055)) / 1.055, vec3(2.4));
	return mix(low, high, step(vec3(0.04045), color));
}

vec3 ieeLinearToSrgb(vec3 color)
{
	color = max(color, vec3(0.0));
	vec3 low = color * 12.92;
	vec3 high = 1.055 * pow(color, vec3(1.0 / 2.4)) - vec3(0.055);
	return mix(low, high, step(vec3(0.0031308), color));
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

// Liquid coverage (0 = land, 1 = liquid) at a world position. The WED cell
// mask is an outer bound; the base tile's alpha supplies the authored contour.
float ieeLiquidAt(vec2 worldPos)
{
	float mode = texture2D(uIeeAreaMask, worldPos * uIeeWorldSizeInv).r * 255.0;
	return mode > 0.5 ? 1.0 : 0.0;
}

// Sample 8px around the fragment so authored holes can cross a cell boundary
// without exposing the engine's generic liquid overlay.
float ieeCoverage(vec2 worldPos)
{
	float cover = ieeLiquidAt(worldPos);
	cover += ieeLiquidAt(worldPos + vec2( 8.0, 0.0));
	cover += ieeLiquidAt(worldPos + vec2(-8.0, 0.0));
	cover += ieeLiquidAt(worldPos + vec2(0.0,  8.0));
	cover += ieeLiquidAt(worldPos + vec2(0.0, -8.0));
	return cover / 5.0;
}

// The caller already sampled the current cell to obtain its liquid mode.
// Reuse that result instead of fetching the same mask texel again.
float ieeCoverageWithCenter(vec2 worldPos, float centerLiquid)
{
	// When an unflagged fragment is more than 8px from its 64px cell edge,
	// every neighbor tap necessarily addresses the same unflagged cell. Avoid
	// four texture fetches without changing the coverage result.
	vec2 inCell = mod(worldPos, 64.0);
	vec2 edgeDistance = min(inCell, vec2(64.0) - inCell);
	if (centerLiquid < 0.5 && min(edgeDistance.x, edgeDistance.y) > 8.0)
	{
		return 0.0;
	}

	float cover = centerLiquid;
	cover += ieeLiquidAt(worldPos + vec2( 8.0, 0.0));
	cover += ieeLiquidAt(worldPos + vec2(-8.0, 0.0));
	cover += ieeLiquidAt(worldPos + vec2(0.0,  8.0));
	cover += ieeLiquidAt(worldPos + vec2(0.0, -8.0));
	return cover / 5.0;
}

// A liquid cell surrounded by liquid cells cannot reach land through any of
// the shore kernel's +/-52px taps. Prove that case from the eight neighboring
// 64px WED cells and skip the more expensive 20-tap shoreline filter. The
// result is identical for confirmed interior water; edge cells retain the
// validated shoreline path below.
bool ieeIsInteriorWater(vec2 worldPos, float centerCoverage)
{
	if (centerCoverage < 0.999)
	{
		return false;
	}

	const float cell = 64.0;
	if (ieeLiquidAt(worldPos + vec2( cell, 0.0)) < 0.5) { return false; }
	if (ieeLiquidAt(worldPos + vec2(-cell, 0.0)) < 0.5) { return false; }
	if (ieeLiquidAt(worldPos + vec2(0.0,  cell)) < 0.5) { return false; }
	if (ieeLiquidAt(worldPos + vec2(0.0, -cell)) < 0.5) { return false; }
	if (ieeLiquidAt(worldPos + vec2( cell,  cell)) < 0.5) { return false; }
	if (ieeLiquidAt(worldPos + vec2(-cell,  cell)) < 0.5) { return false; }
	if (ieeLiquidAt(worldPos + vec2( cell, -cell)) < 0.5) { return false; }
	if (ieeLiquidAt(worldPos + vec2(-cell, -cell)) < 0.5) { return false; }
	return true;
}

// Smoothed shore proximity: 0 deep inside water, ~1 at the land boundary.
float ieeShoreFactor(vec2 worldPos, float centerCoverage)
{
	if (ieeIsInteriorWater(worldPos, centerCoverage))
	{
		return 0.0;
	}

	// centerCoverage was already computed for the contour gate. Reusing it
	// removes another five mask fetches without changing the result.
	float cover = centerCoverage * 2.0;
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

	// Contour diagnostic (uIeeEnabled = 2.0). The base tile's texture alpha is
	// the painted water contour inside flagged cells:
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

		// Point-placement markers: a filled dot plus a 40px ring at every fed
		// effect point — orange = fire, grey = smoke, yellow = light. Absent
		// rings in ALIGN mode mean the point feed (not the effect math) is
		// broken.
		if (uIeePointCount > 0.5 && vColor.a > 0.9)
		{
			float ring = 0.0;
			vec3 ringColor = vec3(0.0);
			for (int i = 0; i < 32; ++i)
			{
				if (float(i) >= uIeePointCount) { break; }
				vec4 p = uIeePoints[2 * i];
				float d = length(worldPos - p.xy);
				float m = smoothstep(4.0, 1.5, abs(d - 40.0)) + smoothstep(6.0, 2.0, d);
				if (m > ring)
				{
					ring = m;
					if (p.z < 1.5)      { ringColor = vec3(1.0, 0.45, 0.05); }
					else if (p.z < 2.5) { ringColor = vec3(0.75, 0.75, 0.85); }
					else                { ringColor = vec3(1.0, 0.95, 0.20); }
				}
			}
			if (ring > 0.02)
			{
				gl_FragColor = vec4(mix(gl_FragColor.rgb, ringColor, clamp(ring, 0.0, 1.0)),
				                    gl_FragColor.a);
			}
		}
		return;
	}

	// Ambient point effects: computed before sampling so heat shimmer can
	// ride the one seamSample call as a texture-coordinate offset. Base pass
	// only; the WATER_ALPHA secondary pass and fades stay vanilla. Kept as
	// flat locals (no struct/function) for maximum GLSL-frontend
	// compatibility.
	vec3 fxGlow = vec3(0.0);
	vec3 fxFlame = vec3(0.0);
	float fxFlameA = 0.0;
	float fxShimmer = 0.0;
	float fxHaze = 0.0;
	if (uIeeEnabled > 0.5 && vColor.a > 0.9 && uIeePointCount > 0.5)
	{
		float fxT = uIeeTime;
		for (int i = 0; i < 32; ++i)
		{
			if (float(i) >= uIeePointCount) { break; }
			vec4 p = uIeePoints[2 * i];
			vec4 pb = uIeePoints[2 * i + 1];
			vec2 offs = worldPos - p.xy;
			float kind = p.z;
			float strength = clamp(p.w / 76.0, 0.05, 1.4);

			if (kind < 1.5) // fire: textured flame body + cast light + shimmer
			{
				// Authored geometry from the BAM frame table: p.w = height,
				// pb.x = half-width, position already at the flame's
				// bottom-center.
				float fh = max(p.w, 5.0);
				float fw = max(pb.x, 1.5);
				float palette = fract(kind) * 10.0;   // 0 warm / 1 blue / 2 glow-only
				float blue = (palette > 0.5 && palette < 1.5) ? 1.0 : 0.0;
				bool bodyless = palette > 1.5;

				// --- flame body (replaces the engine's BAM flame) ---
				if (!bodyless && offs.y < 6.0 && offs.y > -fh * 1.3 && abs(offs.x) < fw * 2.6)
				{
					float v = clamp(-offs.y / fh, 0.0, 1.2);   // 0 base -> 1 tip
					float widthAt = fw * (1.05 - 0.60 * min(v, 1.0));
					float xr = offs.x / max(widthAt, 1.0);
					float radial = 1.0 - clamp(xr * xr, 0.0, 1.0);
					float base = smoothstep(8.0, -1.0, offs.y);
					float column = radial * base * smoothstep(1.20, 0.50, v);
					// Rising noise erodes the column: calm base, ragged tip.
					// Sample scale follows the flame size so small flames keep
					// visible structure.
					float ns = max(fh, 14.0);
					float seed = p.x * 0.61 + float(i) * 19.0;
					float nA = texture2D(uIeeNoiseMap,
					              vec2((seed + offs.x) / (ns * 0.62),
					                   (offs.y + fxT * ns * 1.35) / (ns * 0.9))).r;
					float nB = texture2D(uIeeNoiseMap,
					              vec2((seed * 0.37 - offs.x) / (ns * 0.36),
					                   (offs.y + fxT * ns * 2.1) / (ns * 0.52))).b;
					float intensity = column * (1.15 - v * 0.5)
					                - (nA * 0.78 + nB * 0.60) * (0.36 + 0.95 * v);
					intensity = clamp(intensity * 1.75, 0.0, 1.0);
					if (intensity > 0.02)
					{
						// Ramp: deep -> mid -> bright -> hot core (kept small).
						vec3 c0 = mix(vec3(0.42, 0.02, 0.0), vec3(0.01, 0.06, 0.42), blue);
						vec3 c1 = mix(vec3(1.0, 0.28, 0.02), vec3(0.08, 0.38, 0.95), blue);
						vec3 c2 = mix(vec3(1.0, 0.70, 0.16), vec3(0.45, 0.75, 1.0), blue);
						vec3 c3 = mix(vec3(1.02, 0.95, 0.72), vec3(0.85, 0.95, 1.05), blue);
						vec3 flame = mix(c0, c1, smoothstep(0.02, 0.38, intensity));
						flame = mix(flame, c2, smoothstep(0.38, 0.74, intensity));
						flame = mix(flame, c3, smoothstep(0.80, 0.97, intensity));
						float a = smoothstep(0.03, 0.30, intensity);
						if (a > fxFlameA)
						{
							fxFlameA = a;
							fxFlame = flame;
						}
					}
					fxShimmer += column * 0.5 * strength;
				}

				// --- cast light on the surroundings ---
				vec2 g = offs;
				g.y += fh * 0.4;  // center the light on the flame body
				float radius = 42.0 + 85.0 * strength;
				float d = length(g);
				if (d < radius)
				{
					float fall = 1.0 - d / radius;
					fall *= fall;
					float fi = float(i) * 7.31;
					float flick = 0.80 + 0.14 * sin(fxT * 9.7 + fi)
					                   + 0.06 * sin(fxT * 23.0 + fi * 1.7);
					vec3 glowColor = mix(vec3(1.0, 0.45, 0.15), vec3(0.30, 0.55, 1.0),
					                     step(0.5, blue));
					fxGlow += glowColor * (fall * (0.30 + 0.55 * strength) * flick);
				}
			}
			else if (kind < 2.5) // smoke: textured plume replacing the BAM puffs
			{
				float height = max(p.w, 20.0);
				float rise = -offs.y; // px above the source
				if (rise > -10.0 && rise < height)
				{
					float prog = rise / height;
					float seed = p.x * 0.37 + float(i) * 11.0;
					// Slow lateral wander that grows with altitude.
					float sway = (texture2D(uIeeNoiseMap,
					                 vec2(seed / 64.0 + fxT * 0.045, prog * 1.7)).a - 0.5)
					             * (8.0 + 34.0 * prog);
					float widthPx = max(pb.x, 6.0) + 26.0 * prog;
					float lateral = (offs.x - sway) / widthPx;
					float across = exp(-lateral * lateral * 1.9);
					// Two scrolling octaves shape the billows.
					float dA = texture2D(uIeeNoiseMap,
					              vec2((offs.x + seed) / 96.0, (offs.y + fxT * 42.0) / 96.0)).g;
					float dB = texture2D(uIeeNoiseMap,
					              vec2((offs.x - seed) / 48.0, (offs.y + fxT * 66.0) / 48.0)).r;
					float density = clamp(dA * 0.80 + dB * 0.55 - 0.38, 0.0, 1.0);
					float along = smoothstep(-10.0, 12.0, rise) * (1.0 - prog);
					fxHaze += across * along * density * 0.62;
				}
			}
			else if (kind > 3.5 && kind < 4.5) // light: steady soft glow
			{
				float radius = max(p.w, 10.0);
				float d = length(offs);
				if (d < radius)
				{
					float fall = 1.0 - d / radius;
					fall *= fall;
					float breathe = 0.92 + 0.08 * sin(fxT * 2.1 + float(i) * 3.3);
					fxGlow += vec3(1.0, 0.74, 0.40) * (fall * 0.40 * breathe);
				}
			}
		}
		fxShimmer = clamp(fxShimmer, 0.0, 1.0);
		fxHaze = clamp(fxHaze, 0.0, 0.62);
	}

	vec2 sampleTc = vTc;
	if (fxShimmer > 0.01)
	{
		// Small wavering refraction above flames; uTcScale converts the
		// world-pixel amplitude into atlas units, so the offset stays within
		// the seam handling's tolerance.
		vec2 wobble = vec2(sin(uIeeTime * 13.0 + worldPos.y * 0.35),
		                   cos(uIeeTime * 11.0 + worldPos.x * 0.30));
		sampleTc += wobble * fxShimmer * 1.4 * uTcScale;
	}

	vec4 texColor = seamSample(sampleTc);

	// Inside flagged cells the engine draws the
	// base tile with TRANSPARENT pixels exactly where water composites
	// through — the tile's own alpha is the painted contour. Restrict to the
	// opaque base pass: the WATER_ALPHA secondary pass and fades carry
	// vColor.a < 1 and must stay vanilla.
	float waterMask = 0.0;
	float waterCoverage = 0.0;
	if (uIeeEnabled > 0.5 && vColor.a > 0.9)
	{
		// Wide cell gate: hole pixels up to one mask texel OUTSIDE a flagged
		// cell still count (coverage 0.2 from one neighbor tap) — authored
		// water spills slightly past its cell, and unstyled spill showed the
		// engine's raw overlay tile at water-body edges.
		waterCoverage = ieeCoverageWithCenter(worldPos, cellMode > 0.5 ? 1.0 : 0.0);
		float cellSoft = smoothstep(0.01, 0.08, waterCoverage);
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
		// so grade from the AUTHORED water tint — the average color of the
		// area's own water overlay tile, fed by the DLL (sea teal, river
		// brown, ...; neutral grey when the overlay tile could not decode).
		// Partially opaque contour pixels still contribute their real art.
		vec3 artLinear = ieeSrgbToLinear(texColor.rgb);
		vec3 artColor = mix(uIeeWaterTint, artLinear, texColor.a);
		float artLuma = dot(artColor, vec3(0.2126, 0.7152, 0.0722));
		// Normalize the art tone so dark night pixels still yield a usable hue;
		// soften extremes so bright rim pixels can't smear the palette.
		vec3 artTone = artColor / max(artLuma, 0.06);
		artTone = mix(vec3(1.0), artTone, 0.75);
		vec3 deepColor    = artTone * 0.10;
		vec3 shallowColor = artTone * 0.32;
		vec3 foamColor    = mix(ieeSrgbToLinear(vec3(0.88)), artTone * 0.9, 0.10);
		float emissive    = 0.0;
		if (cellMode > 1.5 && cellMode < 2.5)
		{
			// Lava keeps an authored-orange emissive identity.
			deepColor = ieeSrgbToLinear(vec3(0.32, 0.04, 0.00));
			shallowColor = ieeSrgbToLinear(vec3(0.85, 0.28, 0.02));
			foamColor = ieeSrgbToLinear(vec3(1.00, 0.78, 0.25));
			emissive = 0.5;
		}

		// Shore proximity: brightens shallows and drives the foam band.
		float shore = ieeShoreFactor(worldPos, waterCoverage);

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

		// Sun glitter + lava glow (kept subtle: strong white spec on the
		// neutral grade can otherwise read as monochrome noise).
		water += spec * mix(0.15, 0.45, waveH);
		if (emissive > 0.0)
		{
			float pulse = 0.5 + 0.5 * sin(t * 1.1 + waveH * 6.0);
			water += deepColor * emissive * (0.6 + 0.8 * pulse);
		}

		// Replace by the painted contour: hole pixels become our water (their
		// art rgb is black anyway); the seamSample bilinear already softens
		// the alpha edge. Raise the output alpha so our water covers the
		// engine's generic animated tile drawn underneath the cell.
		texColor.rgb = ieeLinearToSrgb(mix(artLinear, water, waterMask));
		texColor.a = max(texColor.a, waterMask);
	}

	// Firelight, candle glow, and smoke haze over the (possibly water-graded)
	// background art, in linear light so the warm lift does not band.
	if (fxHaze > 0.004 || fxFlameA > 0.004 || fxGlow.r + fxGlow.g + fxGlow.b > 0.004)
	{
		vec3 lin = ieeSrgbToLinear(texColor.rgb);
		float sceneLuma = dot(lin, vec3(0.2126, 0.7152, 0.0722));
		// True black means void/unexplored: no art exists there, so cast
		// light must not paint it (the additive term is luma-gated; the
		// multiplicative term is naturally zero on black).
		float hasArt = smoothstep(0.0015, 0.012, sceneLuma);
		// Night-adaptive cast light: dark scenes get real light, bright day
		// scenes only a whisper.
		float castLight = (0.05 + 0.45 * (1.0 - clamp(sceneLuma * 4.0, 0.0, 1.0))) * hasArt;
		lin = lin * (vec3(1.0) + fxGlow * 1.5) + fxGlow * castLight;
		// Smoke veil: moonlit smoke over dark roofs, shadowy over bright
		// ground. Not art-gated — a per-texel luma gate mottles dark stone,
		// and smoke drifting over the night sky reads naturally.
		vec3 hazeTarget = vec3(0.40, 0.40, 0.45) * (0.40 + 0.55 * sceneLuma) + vec3(0.02);
		lin = mix(lin, hazeTarget, fxHaze);
		// Flame body composites over everything with its own self-glow;
		// flames are visible against the void (a fire at a cave mouth), so it
		// is deliberately not art-gated.
		lin = mix(lin, fxFlame, fxFlameA);
		lin += fxFlame * fxFlameA * 0.35;
		texColor.rgb = ieeLinearToSrgb(lin);
		texColor.a = max(texColor.a, fxFlameA);
	}

	texColor = texColor * vColor;

	float grey = dot(texColor.rgb, vec3(0.299, 0.587, 0.114));
	vec3 tone = grey * uColorTone.rgb;

	// The engine blends the painted-art secondary tile at WATER_ALPHA over
	// flagged cells AFTER the base pass; at native strength it buries our
	// water under static art, and cells
	// WITHOUT a secondary tile render our water alone — the two populations
	// look different. Suppress the pass entirely while ON
	// so every water cell shares one source of truth; OFF and ALIGN keep it.
	float alphaScale = 1.0;
	if (uIeeEnabled > 0.5 && uIeeEnabled < 1.5 && cellMode > 0.5 &&
	    vColor.a > 0.15 && vColor.a < 0.9)
	{
		alphaScale = 0.0;
	}

	gl_FragColor = vec4(mix(texColor.rgb, tone, uColorTone.a), texColor.a * alphaScale);
}
