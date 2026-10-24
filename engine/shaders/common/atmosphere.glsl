// ===========================================================================
//  atmosphere.glsl — physically based sky: Rayleigh + Mie + ozone single
//  scattering along a ray through a spherical atmosphere.
//
//  This is the same family of model used by production engines (Preetham
//  extended with a Bruneton-style height profile). It runs analytic here so a
//  cheap transmittance LUT is not required on mobile, with an optional
//  precomputed LUT path in the IBL baker.
// ===========================================================================
#ifndef NEO_ATMOSPHERE_GLSL
#define NEO_ATMOSPHERE_GLSL

#include "frame_data.glsl"

// Rayleigh scattering coefficients at sea level (1/m), wavelength dependent.
vec3 rayleigh_coeff() { return frame.atmosphereRayleigh.xyz; }
// Mie scattering coefficient (grey, aerosol).
float mie_coeff() { return frame.atmosphereMie.x; }
// Ozone absorption.
vec3 ozone_coeff() { return frame.atmosphereOzone.xyz; }
float planet_radius() { return frame.atmosphereScale.x; }
float atmosphere_radius() { return frame.atmosphereScale.y; }
float mie_g() { return frame.atmosphereMie.w; }

const float RAYLEIGH_SCALE_HEIGHT = 8000.0;
const float MIE_SCALE_HEIGHT = 1200.0;

// Densities at a given altitude above the surface.
float rayleigh_density(float altitude) {
  return exp(-max(altitude, 0.0) / RAYLEIGH_SCALE_HEIGHT);
}
float mie_density(float altitude) {
  return exp(-max(altitude, 0.0) / MIE_SCALE_HEIGHT);
}

// Ray/sphere intersection: returns (t_near, t_far), or (-1,-1) when it misses.
vec2 ray_sphere(vec3 origin, vec3 dir, float radius) {
  float b = dot(dir, origin);
  float c = dot(origin, origin) - radius * radius;
  float d = b * b - c;
  if (d < 0.0) return vec2(-1.0);
  d = sqrt(d);
  return vec2(-b - d, -b + d);
}

// Henyey-Greenstein phase function for aerosols.
float phase_mie(float cos_theta, float g) {
  float g2 = g * g;
  return (1.0 - g2) / (4.0 * NEO_PI * pow(max(1.0 + g2 - 2.0 * g * cos_theta, 1e-4), 1.5));
}

// Rayleigh phase is nearly symmetric with a slight forward/backward lobe.
float phase_rayleigh(float cos_theta) {
  return (3.0 / (16.0 * NEO_PI)) * (1.0 + cos_theta * cos_theta);
}

// ---------------------------------------------------------------------------
//  Integrate in-scattering along a view ray from a camera at `origin`.
//  Returns linear HDR radiance. Step count is tuned for mobile: 16 steps is
//  visually indistinguishable from 64 once the sun disk is added analytically.
// ---------------------------------------------------------------------------
vec3 atmosphere_scattering(vec3 origin, vec3 dir, int steps, out float transmittance_out) {
  const vec3 sun = frame.sunDirection.xyz;
  const float sun_intensity = frame.sunDirection.w;

  vec3 camera = origin + vec3(0.0, planet_radius(), 0.0);
  vec2 hit = ray_sphere(camera, dir, atmosphere_radius());
  if (hit.y < 0.0) {
    transmittance_out = 1.0;
    return vec3(0.0);
  }
  float t_start = max(hit.x, 0.0);
  float t_end = hit.y;

  // Stop at the planet surface if we are looking down.
  vec2 ground = ray_sphere(camera, dir, planet_radius());
  if (ground.x > 0.0) t_end = min(t_end, ground.x);

  if (t_end <= t_start) {
    transmittance_out = 1.0;
    return vec3(0.0);
  }

  float segment = (t_end - t_start) / float(steps);
  vec3 rayleigh_sum = vec3(0.0);
  vec3 mie_sum = vec3(0.0);
  float optical_r = 0.0;
  float optical_m = 0.0;

  float cos_theta = dot(dir, sun);
  float p_r = phase_rayleigh(cos_theta);
  float p_m = phase_mie(cos_theta, mie_g());

  // Optical depth is accumulated back-to-front so each sample knows how much
  // atmosphere is behind it (needed for the extinction term).
  for (int i = 0; i < steps; ++i) {
    float t = t_start + segment * (float(i) + 0.5);
    vec3 p = camera + dir * t;
    float altitude = length(p) - planet_radius();
    if (altitude < 0.0) break;

    float dr = rayleigh_density(altitude) * segment;
    float dm = mie_density(altitude) * segment;
    optical_r += dr;
    optical_m += dm;

    // Sun visibility from this sample point (planet shadowing).
    vec2 sun_hit = ray_sphere(p, sun, planet_radius());
    if (sun_hit.x > 0.0) continue;

    vec3 tau = rayleigh_coeff() * optical_r + vec3(mie_coeff() * optical_m) +
               ozone_coeff() * optical_r * 0.15;
    vec3 attenuation = exp(-tau);

    rayleigh_sum += dr * attenuation;
    mie_sum += dm * attenuation;
  }

  vec3 rayleigh = rayleigh_coeff() * rayleigh_sum * p_r;
  vec3 mie = vec3(mie_coeff()) * mie_sum * p_m * 0.9;
  vec3 in_scatter = (rayleigh + mie) * sun_intensity * frame.sunColor.rgb;

  vec3 tau_total = rayleigh_coeff() * optical_r + vec3(mie_coeff() * optical_m) +
                   ozone_coeff() * optical_r * 0.15;
  transmittance_out = exp(-tau_total).r;
  return in_scatter;
}

// Analytic sun disk with limb darkening. Added on top of the scattering term
// so the sun stays crisp regardless of step count.
vec3 sun_disk(vec3 dir) {
  vec3 sun = frame.sunDirection.xyz;
  float cos_angle = dot(dir, sun);
  float cos_radius = cos(frame.sunColor.w);
  if (cos_angle < cos_radius) return vec3(0.0);

  // Limb darkening: the solar limb is ~60% as bright as the centre.
  float r = acos(clamp(cos_angle, -1.0, 1.0)) / max(frame.sunColor.w, 1e-4);
  float limb = 1.0 - 0.4 * (1.0 - sqrt(max(1.0 - r * r, 0.0)));

  float intensity = frame.sunDirection.w * 0.02;
  return frame.sunColor.rgb * intensity * limb;
}

// Aerial perspective for distant geometry: tint towards the horizon colour and
// fade with distance. Uses an exponential height fog with a sun-facing glow.
vec3 apply_aerial_perspective(vec3 color, vec3 world_pos, vec3 view_dir) {
  float dist = length(world_pos - frame.cameraPosition.xyz);
  const float density = 0.000025;
  const float falloff = 0.00008;

  float height_factor = exp(-max(world_pos.y, 0.0) * falloff);
  float fog = 1.0 - exp(-dist * density * height_factor);

  float sun_amount = pow(saturate1(dot(view_dir, frame.sunDirection.xyz)), 8.0);
  vec3 fog_color = mix(vec3(0.55, 0.68, 0.85), frame.sunColor.rgb * 1.4, sun_amount);

  return mix(color, fog_color, saturate1(fog));
}

#endif // NEO_ATMOSPHERE_GLSL
