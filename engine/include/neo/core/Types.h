// ===========================================================================
//  Neo-Asis Engine 3D — core scalar / handle types
// ===========================================================================
#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>

namespace neo {

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using i8 = std::int8_t;
using i16 = std::int16_t;
using i32 = std::int32_t;
using i64 = std::int64_t;
using f32 = float;
using f64 = double;
using usize = std::size_t;

// --- Generic generational handle -------------------------------------------
// A 32-bit index + 32-bit generation packed in a u64. Zero is "null".
// Handles are cheap to copy, safe against stale references, and trivially
// hashable — used for meshes, materials, textures, entities, GPU resources.
template <typename Tag> class Handle {
public:
  static constexpr u32 kIndexBits = 24;
  static constexpr u32 kIndexMask = (1u << kIndexBits) - 1u;

  constexpr Handle() = default;

  [[nodiscard]] static constexpr Handle make(u32 index, u32 generation = 1) {
    return Handle((static_cast<u64>(generation) << kIndexBits) | (index & kIndexMask));
  }

  [[nodiscard]] constexpr bool valid() const { return id_ != 0; }
  [[nodiscard]] constexpr explicit operator bool() const { return valid(); }

  [[nodiscard]] constexpr u32 index() const { return static_cast<u32>(id_ & kIndexMask); }
  [[nodiscard]] constexpr u32 generation() const { return static_cast<u32>(id_ >> kIndexBits); }
  [[nodiscard]] constexpr u64 raw() const { return id_; }

  friend constexpr bool operator==(Handle a, Handle b) { return a.id_ == b.id_; }
  friend constexpr bool operator!=(Handle a, Handle b) { return a.id_ != b.id_; }
  friend constexpr bool operator<(Handle a, Handle b) { return a.id_ < b.id_; }

private:
  explicit constexpr Handle(u64 id) : id_(id) {}
  u64 id_ = 0;
};

struct MeshTag {};
struct MaterialTag {};
struct TextureTag {};
struct EntityTag {};
struct PipelineTag {};
struct BufferTag {};

using MeshHandle = Handle<MeshTag>;
using MaterialHandle = Handle<MaterialTag>;
using TextureHandle = Handle<TextureTag>;
using EntityHandle = Handle<EntityTag>;
using PipelineHandle = Handle<PipelineTag>;
using BufferHandle = Handle<BufferTag>;

// --- Small helpers ---------------------------------------------------------
[[nodiscard]] inline constexpr u32 align_up(u32 v, u32 a) {
  return (v + a - 1u) & ~(a - 1u);
}
[[nodiscard]] inline constexpr u64 align_up64(u64 v, u64 a) {
  return (v + a - 1ull) & ~(a - 1ull);
}

template <typename T> [[nodiscard]] inline constexpr T clamp(T v, T lo, T hi) {
  return v < lo ? lo : (v > hi ? hi : v);
}

inline void hash_combine(usize& seed, usize v) {
  seed ^= v + 0x9e3779b97f4a7c15ull + (seed << 6) + (seed >> 2);
}

} // namespace neo
