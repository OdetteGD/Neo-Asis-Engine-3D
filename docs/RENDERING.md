# Rendering

The interesting part. This document covers what each pass does, the maths
involved, and the mobile-specific trade-offs behind the choices.

## Pipeline overview

```
  ┌───────────────┐
  │ ShadowCascades│  depth-only, 3-4 cascades in a tiled atlas
  └───────┬───────┘
          ▼
  ┌───────────────┐
  │   G-Buffer    │  albedo+AO | octNormal+rough+metal | emissive
  └───────┬───────┘
          │                    ┌──────────────────┐
          │                    │ VolumetricClouds │ compute, 0.25-0.5x res
          │                    └────────┬─────────┘
          ▼                             ▼
  ┌────────────────────────────────────────────────┐
  │           Deferred Lighting (HDR)              │
  │  PBR direct + IBL + shadows + atmosphere       │
  │  + cloud composite + aerial perspective        │
  └───────────────────────┬────────────────────────┘
                          ▼
  ┌────────────────────────────────────────────────┐
  │  Bloom (dual filter) → ACES → FXAA → sRGB      │
  │  → dither → surface pre-rotation               │
  └────────────────────────────────────────────────┘
```

## Shadow cascades

Three cascades by default. The split distances are logarithmic, matching the
way perspective depth distributes precision:

```
split_i = near * (far/near)^(i/N)
```

The atlas is a single 2D depth image with cascades laid out horizontally, which
means one binding, one layout transition and one cache-hot texture for all
cascades. Selection in the shader is a simple loop over the four matrices with
an NDC bounds test — branching is cheap here because whole warps usually agree.

Bias is handled two ways, because one is never enough:
- constant depth bias during vertex transform
- world-space normal offset before the lookup

## G-buffer packing

| Target | Format | Contents | Bytes/px |
|---|---|---|---|
| 0 | `R8G8B8A8_UNORM` | albedo.rgb, AO | 4 |
| 1 | `R16G16B16A16_SFLOAT` | octahedral normal.xy, roughness, metallic | 8 |
| 2 | `B10G11R11_UFLOAT` | emissive (HDR, no exponent issues) | 4 |
| depth | `D32_SFLOAT` | reverse-Z depth | 4 |

20 bytes per pixel versus 32+ for the naive RGBA16F layout. On a 1080p phone
that is ~42 MB of traffic *per read-write cycle* saved.

Normals use octahedral encoding rather than a full octahedron of 32 bits because
the extra precision buys nothing visible once you clamp roughness for specular
anti-aliasing.

## PBR

Standard metallic/roughness Cook-Torrance:

```
f = (1-F) * albedo/π * (1-metallic)  +  D·V·F
```

- `D` — GGX / Trowbridge-Reitz
- `V` — Smith height-correlated visibility (Heitz 2014)
- `F` — Schlick

Two deliberate additions:

**Specular anti-aliasing.** Roughness is clamped to the normal variance across
the pixel footprint:

```glsl
float variance = 0.25 * (dot(dFdx(N), dFdx(N)) + dot(dFdy(N), dFdy(N)));
roughness = max(roughness, sqrt(clamp(variance, 0.0, 1.0)));
```

Without this, high-frequency normal maps shimmer badly on a 6-inch screen.

**Multiple-scattering compensation.** Single-scattering GGX loses energy at high
roughness, turning rough metals black. The analytic compensation term restores
it, with the IBL path getting an extra `1 + 0.35·roughness` factor.

## Image-based lighting

Split-sum approximation:

```
specular ≈ prefiltered(roughness) × (F0 · A + B)
diffuse  ≈ irradiance × albedo × (1-F)
```

`A` and `B` come from `ibl/brdf_lut.comp` (1024-sample Hammersley integration).
Because that LUT is a 256×256 texture fetch per pixel, the lighting pass also
ships a compact analytic fit:

```glsl
float NoV4 = pow(NoV, 4.0);
float brdf_x = mix(1.0, 1.0 - a * 0.72, NoV4);
float brdf_y = a * 0.28 * (1.0 - NoV4);
```

On a tile-based GPU, avoiding that fetch and its cache pressure is worth more
than the accuracy difference.

## Atmosphere

Rayleigh + Mie + ozone, integrated analytically along the view ray in
`common/atmosphere.glsl`. Why not a full LUT chain (transmittance → scattering →
irradiance) like a desktop engine? Because on mobile the LUT generation cost and
its memory traffic show up as a fixed frame cost, while the analytic version
scales down cleanly: 14 steps at half-res quality still looks right because the
sun disk is added analytically rather than resolved by the march.

Height profiles:

```
ρ_R(h) = exp(-h / 8000)
ρ_M(h) = exp(-h / 1200)
```

Optical depth accumulates back-to-front in a single loop, so each sample knows
how much atmosphere is behind it for extinction.

Aerial perspective is applied to surfaces after lighting, tinted towards the
horizon colour with an extra sun-facing glow term.

## Volumetric clouds

`passes/clouds.comp`, dispatched at 0.25–0.5× resolution.

**Shape.** Perlin–Worley, the standard construction for cumulus:

```
shape = perlin(p) − (1 − worley(p·3)) · 0.55
```

A vertical profile flattens the base and rounds the top, and a weather map
modulates coverage so clouds form banks instead of uniform mush.

**Lighting.** A 6-tap march towards the sun gives optical depth, then:

```
beer   = exp(-optical · 0.12)
powder = 1 - exp(-density · 8)
light  = beer · (0.55 + 0.45 · powder)
```

The powder term is what makes cloud edges dark and thin wisps translucent — the
single biggest contributor to "that looks like a cloud" versus "that looks like
fog".

**Temporal stability.** Per-pixel jitter from interleaved gradient noise offset
by the frame index turns banding into high-frequency noise, which a temporal
filter resolves between frames. Cost is 1–3 ms at quarter res on a mid-range
Mali-G78, which is why Medium runs clouds at 0.35×.

## Post chain

**Bloom** is dual-filtered: a 13-tap downsample chain (firefly-resistant, unlike
a box filter) and a 3×3 tent upsample accumulated additively. The first mip
applies a soft-knee threshold.

**Tonemapping** is ACES (Narkowicz fit) — cheap, good highlight rolloff, and
well understood by artists.

**Anti-aliasing** defaults to FXAA because MSAA on a deferred renderer means
either fat G-buffers or an extra resolve, both of which cost more than they are
worth at 1080p on a phone. High/Ultra can enable TAA instead.

**Dithering.** One LSB of ordered noise before output. Sky gradients band badly
on 8-bit mobile panels; this makes the banding invisible for effectively zero
cost.

## Surface pre-rotation

Phones report a display transform (90°/270° in portrait). Rather than rendering
into a rotated target and letting the compositor rotate — which costs a full
bandwidth pass — the tonemap pass applies a 2×2 rotation to the UV and the
renderer declares the transform in `VkSwapchainCreateInfoKHR`. See
`Swapchain::rotation_matrix_element()`.

## Where to look when something is wrong

| Symptom | First place to check |
|---|---|
| Black screen on Android | `AndroidMain.cpp` lifecycle ordering; surface created before renderer |
| Everything too dark / too bright | `FrameConstants::cameraPosition.w` (exposure) and `sunDirection.w` (lux) |
| Sky looks flat | `atmosphere_scale.z` (turbidity) and `rayleigh_coeff()` |
| Shadows missing | `shadow_params.z` bias, or the atlas never transitioned to a sampled layout |
| Shimmering highlights | Specular AA clamp in `gbuffer.frag` |
| Bands in the sky | Dither in `tonemap.frag` |
| Banding in clouds | Ray jitter (`ign`) and the frame-index offset |
