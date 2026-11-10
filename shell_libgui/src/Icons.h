#pragma once
// The ribbon's icons as one RGBA atlas: SVG sources compiled in (icons/*.svg), rasterised once.
//
// Toolkit-free: this produces pixels and texture coordinates. Uploading them is the UiPass's job
// and drawing them is a panel's, so neither libgui nor bgfx appears here.

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace cad::shell_libgui {

struct IconSource {
    const char* name;  ///< RibbonItem::iconName
    const char* svg;
};
extern const IconSource kIconSources[];
extern const std::size_t kIconSourceCount;

/// Where one icon sits in the atlas, in texture coordinates.
struct IconUv {
    float u0 = 0, v0 = 0, u1 = 0, v1 = 0;
};

class IconAtlas {
public:
    /// Rasterises every source at `cellPx` square, RGBA8 with PREMULTIPLIED alpha. A source nanosvg
    /// cannot parse is reported on stderr and left out, so a bad file costs one icon, not the ribbon.
    static IconAtlas build(std::uint32_t cellPx);

    [[nodiscard]] const IconUv* find(const std::string& name) const;

    std::vector<std::uint8_t> rgba;
    std::uint32_t width = 0, height = 0;

private:
    std::unordered_map<std::string, IconUv> uv_;
};

}  // namespace cad::shell_libgui
