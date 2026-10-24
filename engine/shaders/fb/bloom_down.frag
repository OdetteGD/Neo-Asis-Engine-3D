#version 450
// ===========================================================================
//  bloom_down.frag — 13-tap CoD:AW style downsample.
//  Chosen over a simple 4-tap box because it removes the flickering fireflies
//  that a box filter produces on high-contrast mobile content.
// ===========================================================================

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;

layout(set = 2, binding = 0) uniform sampler2D uSource;

layout(push_constant) uniform BloomPC {
  vec4 texel;      // xy = source texel size, zw = target texel size
  vec4 params;     // x = mip index, y = threshold, z/w spare
} pc;

vec3 downsample13(vec2 uv, vec2 texel) {
  vec3 a = texture(uSource, uv + texel * vec2(-2, -2)).rgb;
  vec3 b = texture(uSource, uv + texel * vec2( 0, -2)).rgb;
  vec3 c = texture(uSource, uv + texel * vec2( 2, -2)).rgb;
  vec3 d = texture(uSource, uv + texel * vec2(-1, -1)).rgb;
  vec3 e = texture(uSource, uv + texel * vec2( 1, -1)).rgb;
  vec3 f = texture(uSource, uv + texel * vec2(-2,  0)).rgb;
  vec3 g = texture(uSource, uv).rgb;
  vec3 h = texture(uSource, uv + texel * vec2( 2,  0)).rgb;
  vec3 i = texture(uSource, uv + texel * vec2(-1,  1)).rgb;
  vec3 j = texture(uSource, uv + texel * vec2( 1,  1)).rgb;
  vec3 k = texture(uSource, uv + texel * vec2(-2,  2)).rgb;
  vec3 l = texture(uSource, uv + texel * vec2( 0,  2)).rgb;
  vec3 m = texture(uSource, uv + texel * vec2( 2,  2)).rgb;

  // Weighted so that the centre 2x2 group dominates (matches the reference
  // kernel: 0.125 corners, 0.5 edges, 2.0 centre).
  return (d + e + i + j) * 0.125 + (a + c + k + m) * 0.03125 +
         (b + f + h + l) * 0.0625 + g * 0.125;
}

void main() {
  vec3 color = downsample13(vUV, pc.texel.xy);

  // First mip applies the soft-knee threshold; subsequent mips just blur.
  if (pc.params.x < 0.5) {
    float threshold = pc.params.y;
    float knee = threshold * 0.6;
    float brightness = max(color.r, max(color.g, color.b));
    float soft = clamp(brightness - threshold + knee, 0.0, 2.0 * knee);
    soft = soft * soft / (4.0 * knee + 1e-5);
    float contribution = max(soft, brightness - threshold) / max(brightness, 1e-5);
    color *= contribution;
  }

  outColor = vec4(max(color, vec3(0.0)), 1.0);
}
