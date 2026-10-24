#version 450
// ===========================================================================
//  tonemap.frag — final pass: exposure, bloom composite, ACES tonemap, FXAA,
//  sRGB encode, vignette, and the surface pre-rotation transform.
//
//  Writes directly to the swapchain image, so this is the second-most
//  bandwidth-sensitive pass on mobile after the G-buffer.
// ===========================================================================

#extension GL_GOOGLE_include_directive : require
#include "common/frame_data.glsl"

layout(set = 1, binding = 0) uniform sampler2D uHDR;
layout(set = 1, binding = 1) uniform sampler2D uBloom;
layout(set = 1, binding = 2) uniform sampler2D uDepth;

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;

layout(push_constant) uniform TonemapPC {
  vec4 exposure;       // x = exposure, y = bloom intensity, z = fxaa, w = vignette
  vec4 preRotationX;   // row 0 of the display rotation matrix
  vec4 preRotationY;   // row 1
} pc;

// --- FXAA (compact, edge-only variant tuned for 1x MSAA) -------------------
vec3 fxaa(vec2 uv, vec2 texel) {
  vec3 rgb_m = texture(uHDR, uv).rgb;
  float luma_m = luma(rgb_m);

  vec3 rgb_nw = texture(uHDR, uv + vec2(-1.0, -1.0) * texel).rgb;
  vec3 rgb_ne = texture(uHDR, uv + vec2( 1.0, -1.0) * texel).rgb;
  vec3 rgb_sw = texture(uHDR, uv + vec2(-1.0,  1.0) * texel).rgb;
  vec3 rgb_se = texture(uHDR, uv + vec2( 1.0,  1.0) * texel).rgb;

  float luma_nw = luma(rgb_nw);
  float luma_ne = luma(rgb_ne);
  float luma_sw = luma(rgb_sw);
  float luma_se = luma(rgb_se);

  float luma_min = min(luma_m, min(min(luma_nw, luma_ne), min(luma_sw, luma_se)));
  float luma_max = max(luma_m, max(max(luma_nw, luma_ne), max(luma_sw, luma_se)));

  // Early out on flat areas — the common case, and the reason this is cheap.
  if (luma_max - luma_min < max(0.0312, luma_max * 0.125)) return rgb_m;

  vec2 dir = vec2(-((luma_nw + luma_ne) - (luma_sw + luma_se)),
                   ((luma_nw + luma_sw) - (luma_ne + luma_se)));

  float dir_reduce = max((luma_nw + luma_ne + luma_sw + luma_se) * (0.25 * 0.0625), 1e-4);
  float rcp_min = 1.0 / (min(abs(dir.x), abs(dir.y)) + dir_reduce);
  dir = clamp(dir * rcp_min, vec2(-8.0), vec2(8.0)) * texel;

  vec3 rgb_a = 0.5 * (texture(uHDR, uv + dir * (1.0 / 3.0 - 0.5)).rgb +
                      texture(uHDR, uv + dir * (2.0 / 3.0 - 0.5)).rgb);
  vec3 rgb_b = rgb_a * 0.5 + 0.25 * (texture(uHDR, uv + dir * -0.5).rgb +
                                     texture(uHDR, uv + dir *  0.5).rgb);

  float luma_b = luma(rgb_b);
  return (luma_b < luma_min || luma_b > luma_max) ? rgb_a : rgb_b;
}

void main() {
  vec2 uv = vUV;

  // Surface pre-rotation: portrait phones report a 90/270 texture transform.
  if (abs(pc.preRotationX.x - 1.0) > 1e-4 || abs(pc.preRotationY.y - 1.0) > 1e-4) {
    vec2 centered = uv * 2.0 - 1.0;
    vec2 rotated = vec2(dot(pc.preRotationX.xy, centered), dot(pc.preRotationY.xy, centered));
    uv = rotated * 0.5 + 0.5;
  }

  vec2 texel = frame.screenSize.zw;

  vec3 hdr = (pc.exposure.z > 0.5) ? fxaa(uv, texel) : texture(uHDR, uv).rgb;

  // --- Bloom ---------------------------------------------------------------
  if (pc.exposure.y > 0.0) {
    hdr += texture(uBloom, uv).rgb * pc.exposure.y;
  }

  // --- Exposure + tonemap --------------------------------------------------
  hdr *= pc.exposure.x * frame.cameraPosition.w;
  vec3 mapped = aces_tonemap(hdr);

  // --- Vignette (subtle, helps focus on small screens) ---------------------
  if (pc.exposure.w > 0.0) {
    vec2 c = vUV - 0.5;
    float v = 1.0 - dot(c, c) * pc.exposure.w * 2.0;
    mapped *= clamp(v, 0.0, 1.0);
  }

  // --- Linear -> sRGB ------------------------------------------------------
  // The swapchain is sRGB-format on some devices and UNORM on others; the
  // renderer picks an sRGB format whenever available, so encoding here only
  // runs when the format is linear.
  vec3 srgb = mix(mapped * 12.92,
                  1.055 * pow(max(mapped, vec3(0.0)), vec3(1.0 / 2.4)) - 0.055,
                  step(vec3(0.0031308), mapped));

  // Ordered dither to kill 8-bit banding in sky gradients (mobile screens show
  // this badly). Amplitude is one LSB.
  float dither = (ign(gl_FragCoord.xy) - 0.5) / 255.0;
  srgb += dither;

  outColor = vec4(clamp(srgb, 0.0, 1.0), 1.0);
}
