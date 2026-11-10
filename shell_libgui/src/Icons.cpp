#include "Icons.h"

#define NANOSVG_IMPLEMENTATION
#define NANOSVGRAST_IMPLEMENTATION
#include <nanosvg.h>
#include <nanosvgrast.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>

namespace cad::shell_libgui {

IconAtlas IconAtlas::build(std::uint32_t cell) {
    IconAtlas atlas;
    const auto columns = std::uint32_t(std::ceil(std::sqrt(double(kIconSourceCount))));
    const std::uint32_t rows = columns == 0 ? 0 : (std::uint32_t(kIconSourceCount) + columns - 1) / columns;
    atlas.width = std::max(1u, columns * cell);
    atlas.height = std::max(1u, rows * cell);
    atlas.rgba.assign(std::size_t(atlas.width) * atlas.height * 4, 0);

    std::unique_ptr<NSVGrasterizer, void (*)(NSVGrasterizer*)> rast(nsvgCreateRasterizer(), nsvgDeleteRasterizer);
    for (std::size_t i = 0; i < kIconSourceCount; ++i) {
        // nsvgParse edits its input in place, so it gets a copy.
        std::string text = kIconSources[i].svg;
        NSVGimage* image = nsvgParse(text.data(), "px", 96.0f);
        if (image == nullptr || image->width <= 0) {
            std::fprintf(stderr, "icon %s: not a readable SVG\n", kIconSources[i].name);
            if (image) nsvgDelete(image);
            continue;
        }
        const std::uint32_t cx = std::uint32_t(i) % columns, cy = std::uint32_t(i) / columns;
        const float scale = float(cell) / std::max(image->width, image->height);
        unsigned char* dst = atlas.rgba.data() + (std::size_t(cy) * cell * atlas.width + std::size_t(cx) * cell) * 4;
        nsvgRasterize(rast.get(), image, 0, 0, scale, dst, int(cell), int(cell), int(atlas.width * 4));
        nsvgDelete(image);
        // nanosvg writes straight alpha. Premultiplied on the CPU, once, because the GPU filters in
        // hardware and straight alpha filtered there gives every edge a dark halo.
        for (std::uint32_t y = 0; y < cell; ++y) {
            unsigned char* px = dst + std::size_t(y) * atlas.width * 4;
            for (std::uint32_t x = 0; x < cell; ++x, px += 4)
                for (int c = 0; c < 3; ++c) px[c] = static_cast<unsigned char>((px[c] * px[3] + 127) / 255);
        }
        // Half a texel in, so linear filtering never reaches a neighbour.
        const float hu = 0.5f / float(atlas.width), hv = 0.5f / float(atlas.height);
        atlas.uv_[kIconSources[i].name] = IconUv{float(cx * cell) / float(atlas.width) + hu,
                                                 float(cy * cell) / float(atlas.height) + hv,
                                                 float((cx + 1) * cell) / float(atlas.width) - hu,
                                                 float((cy + 1) * cell) / float(atlas.height) - hv};
    }
    return atlas;
}

const IconUv* IconAtlas::find(const std::string& name) const {
    const auto it = uv_.find(name);
    return it == uv_.end() ? nullptr : &it->second;
}

}  // namespace cad::shell_libgui
