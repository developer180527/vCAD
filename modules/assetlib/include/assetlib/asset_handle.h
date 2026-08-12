#pragma once
#include "uuid.h"

namespace assetlib {

// Typed wrapper around UUID — the ONLY way to reference assets at runtime.
// Never store paths. Store AssetHandle<Mesh>, AssetHandle<Texture> etc.
//
// The template parameter T is a tag only — used for type safety.
// The handle itself is just a UUID; no T* is stored here.
template<typename T>
struct AssetHandle {
    UUID uuid;

    bool valid()   const { return uuid != UUID::null(); }
    bool operator==(const AssetHandle& o) const { return uuid == o.uuid; }
    bool operator!=(const AssetHandle& o) const { return uuid != o.uuid; }

    static AssetHandle<T> invalid() { return { UUID::null() }; }
};

} // namespace assetlib

template<typename T>
struct std::hash<assetlib::AssetHandle<T>> {
    size_t operator()(const assetlib::AssetHandle<T>& h) const noexcept {
        return std::hash<assetlib::UUID>{}(h.uuid);
    }
};
