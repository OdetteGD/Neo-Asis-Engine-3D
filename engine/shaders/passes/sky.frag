#version 450
// ===========================================================================
//  sky.frag — renders the procedural sky only (used as a background pass and
//  by the IBL baker's capture cube).
// ===========================================================================

#extension GL_GOOGLE_include_directive : require
#include "common/frame_data.glsl"
#include "common/atmosphere.glsl"

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;

layout(push_constant) uniform SkyPC {
  vec4 params;      // x = steps, y = exposure, z = clouds, w = mode
} pc;

void main() {
  // Reconstruct the view ray for this pixel.
  vec4 near_clip = vec4(vUV * 2.0 - 1.0, 0.0, 1.0);
  vec4 far_clip = vec4(vUV * 2.0 - 1.0, 1.0, 1.0);
  vec4 near_world = frame.invViewProj * near_clip;
  vec4 far_world = frame.invViewProj * far_clip;
  vec3 dir = normalize(far_world.xyz / far_world.w - near_world.xyz / near_world.w);

  float transmittance = 1.0;
  int steps = int(clamp(pc.params.x, 8.0, 64.0));
  vec3 color = atmosphere_scattering(frame.cameraPosition.xyz, dir, steps, transmittance);

  // Sun disk, attenuated by the atmosphere path (it dims at the horizon).
  color += sun_disk(dir) * transmittance;

  // Ground plane / lower hemisphere: a distant terrain fade so the sky pass
  // never leaves a hard seam with the scene.
  if (dir.y < 0.0) {
    vec3 ground = vec3(0.045, 0.05, 0.048) * (1.0 - abs(dir.y));
    color = mix(color, ground, smoothstep(0.0, -0.15, dir.y));
  }

  outColor = vec4(color, 1.0);
}
