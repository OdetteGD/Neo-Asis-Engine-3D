// ===========================================================================
//  frame_data.glsl — std430 mirror of neo::FrameConstants (see FrameData.h)
//  Shared by every pass. Set 0, binding 0.
// ===========================================================================
#ifndef NEO_FRAME_DATA_GLSL
#define NEO_FRAME_DATA_GLSL

#extension GL_GOOGLE_include_directive : require

layout(std430, binding = 0) readonly buffer FrameBlock {
  mat4  view;
  mat4  proj;
  mat4  viewProj;
  mat4  invViewProj;
  mat4  prevViewProj;

  vec4  cameraPosition;    // w = exposure
  vec4  cameraForward;
  vec4  screenSize;        // xy = pixels, zw = 1/pixels

  vec4  timing;            // x = t, y = dt, z = frame, w = near

  mat4  shadowMatrices[4];
  vec4  shadowParams;      // x = cascades, y = map size, z = bias, w = normal bias

  vec4  sunDirection;      // xyz = towards the sun, w = intensity
  vec4  sunColor;          // rgb = colour, w = angular radius

  vec4  atmosphereRayleigh;
  vec4  atmosphereMie;     // w = g
  vec4  atmosphereScale;   // x = planet radius, y = atmosphere radius, z = turbidity
  vec4  atmosphereOzone;

  vec4  cloudLayer;        // x = base, y = top
  vec4  cloudParams;       // x = coverage, y = density, z = steps, w = frame
  vec4  cloudWind;

  vec4  fogParams;

  vec4  postParams;        // x = render scale, y = exposure key, z = bloom, w = tier
} frame;

// ---- Scenery lights: set 0, binding 1 -------------------------------------
struct GpuLight {
  vec4 positionRange;      // xyz = position, w = radius
  vec4 directionAngle;     // xyz = direction, w = cos(outer)
  vec4 colorIntensity;     // rgb = linear colour, w = intensity
  vec4 params;             // x = type, y = shadow index
};

layout(std430, binding = 1) readonly buffer LightBlock {
  GpuLight lights[];
};

#define NEO_LIGHT_DIRECTIONAL 0
#define NEO_LIGHT_POINT 1
#define NEO_LIGHT_SPOT 2

// ---- Shared helpers -------------------------------------------------------
const float NEO_PI = 3.14159265359;
const float NEO_EPS = 1e-6;

float saturate1(float x) { return clamp(x, 0.0, 1.0); }
vec3 saturate3(vec3 x) { return clamp(x, vec3(0.0), vec3(1.0)); }

float luma(vec3 c) { return dot(c, vec3(0.2126, 0.7152, 0.0722)); }

// Reconstruct view-space position from device depth with the [0,1] depth range
// the engine uses everywhere (Vulkan convention, GLM_FORCE_DEPTH_ZERO_TO_ONE).
vec3 view_position_from_depth(vec2 uv, float depth) {
  vec4 clip = vec4(uv * 2.0 - 1.0, depth, 1.0);
  vec4 view = frame.invViewProj * clip;
  return (view.xyz / view.w) * vec4(1.0).xyz;
}

vec2 oct_encode(vec3 n) {
  n /= (abs(n.x) + abs(n.y) + abs(n.z));
  vec2 e = n.z >= 0.0 ? n.xy : (1.0 - abs(n.yx)) * sign(n.xy);
  return e * 0.5 + 0.5;
}

vec3 oct_decode(vec2 e) {
  e = e * 2.0 - 1.0;
  vec3 n = vec3(e, 1.0 - abs(e.x) - abs(e.y));
  if (n.z < 0.0) n.xy = (1.0 - abs(n.yx)) * sign(n.xy);
  return normalize(n);
}

// ACES filmic (Narkowicz fit) — cheap enough for mobile, good highlight rolloff.
vec3 aces_tonemap(vec3 x) {
  const float a = 2.51, b = 0.03, c = 2.43, d = 0.59, e = 0.14;
  return saturate3((x * (a * x + b)) / (x * (c * x + d) + e));
}

// Interleaved gradient noise — the standard choice for temporal dithering.
float ign(vec2 pixel) {
  return fract(52.9829189 * fract(0.06711056 * pixel.x + 0.00583715 * pixel.y));
}

#endif // NEO_FRAME_DATA_GLSL
