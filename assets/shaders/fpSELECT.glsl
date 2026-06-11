// fpSELECT.glsl  (IEE replacement: AA on selection ring edges)
//
// AUTHORING RULE: the engine-interface section below must mirror the dumped
// original (iee-shader-dumps/fpSELECT.glsl) exactly — every uniform/varying by
// name, and the uIeeEnabled=0 path must reproduce the original math 1:1.
// If the runtime log reports "missing interface identifier", add the listed
// names from the dump and trigger a shader recompile.
#version 460 compatibility

// ---- engine interface: adapt names to the dump ----
uniform sampler2D sTex;
varying vec2 vTc;

// ---- IEE additions (fed per frame by the probe) ----
uniform float uIeeTime;     // seconds; reserved
uniform float uIeeEnabled;  // 0/1 hotkey A/B

void main() {
    vec4 texel = texture2D(sTex, vTc);
    vec4 engineColor = texel * gl_Color; // mirror the dumped main() exactly

    if (uIeeEnabled > 0.5) {
        // Anti-alias the alpha edge: smooth across one screen-pixel footprint.
        float a = engineColor.a;
        float w = fwidth(a);
        engineColor.a = smoothstep(0.5 - w, 0.5 + w, a);
    }

    gl_FragColor = engineColor;
}
