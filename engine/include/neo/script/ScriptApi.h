// ===========================================================================
//  ScriptApi.h — the stable C ABI between the C++ engine and managed (.NET)
//  gameplay code.
//
//  Design constraints, in order:
//   1. No C++ types cross the boundary. Only PODs, opaque handles and enums
//      with explicit widths. That is what lets the C# side use
//      [UnmanagedCallersOnly] + raw function pointers with no marshalling.
//   2. The struct is append-only and versioned. Never reorder fields: that is
//      an ABI break that silently corrupts every existing script assembly.
//   3. Every function pointer is nullable-checked on the managed side, so a
//      newer engine can run an older script assembly.
//
//  Direction of calls:
//      engine  --NeoHostApi-->  managed script
//      managed --3 exports--->  engine
// ===========================================================================
#pragma once

#include <stdint.h>

#if defined(__cplusplus)
extern "C" {
#endif

#define NEO_SCRIPT_ABI_VERSION 3u

typedef struct NeoWorld NeoWorld; // opaque, engine-owned

// --- Host services offered to managed code ---------------------------------
// All pointers are valid for the lifetime of the process.
typedef struct NeoHostApi {
  uint32_t abi_version; // must equal NEO_SCRIPT_ABI_VERSION

  // --- Diagnostics -------------------------------------------------------
  // level: 0=trace 1=debug 2=info 3=warn 4=error
  void (*log)(int32_t level, const char* message);

  // --- Time --------------------------------------------------------------
  double (*time_seconds)(void);
  float (*delta_seconds)(void);
  uint64_t (*frame_index)(void);

  // --- Entities ----------------------------------------------------------
  // Returns 0 when the id is not alive.
  uint32_t (*entity_is_alive)(NeoWorld* world, uint32_t entity);
  uint32_t (*entity_find)(NeoWorld* world, const char* name);

  // Transform access as an out-array of 7 floats:
  //   [px, py, pz, qx, qy, qz, qw]   (scale is separate)
  void (*transform_get)(NeoWorld* world, uint32_t entity, float* out7);
  void (*transform_set)(NeoWorld* world, uint32_t entity, const float* in7);
  void (*scale_get)(NeoWorld* world, uint32_t entity, float* out3);
  void (*scale_set)(NeoWorld* world, uint32_t entity, const float* in3);

  // --- Input -------------------------------------------------------------
  uint32_t (*key_down)(int32_t key);
  void (*pointer_position)(float* out2);
  uint32_t (*pointer_down)(void);

  // --- Scene / renderer knobs -------------------------------------------
  void (*set_sun_elevation)(NeoWorld* world, float degrees);
  void (*set_cloud_coverage)(NeoWorld* world, float coverage);
  void (*request_ibl_rebake)(NeoWorld* world);

  // --- Reserved for future ABI versions ---------------------------------
  void* reserved[8];
} NeoHostApi;

// --- Managed entry points the engine resolves ------------------------------
// Implemented with [UnmanagedCallersOnly(EntryPoint = "...")] in C#.
typedef int32_t (*NeoScriptInitFn)(NeoWorld* world, const NeoHostApi* host);
typedef void (*NeoScriptUpdateFn)(NeoWorld* world, float delta_seconds);
typedef void (*NeoScriptShutdownFn)(NeoWorld* world);

#if defined(__cplusplus)
} // extern "C"
#endif
