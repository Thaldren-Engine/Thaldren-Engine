#pragma once
// Imports the original game's graphics into the renderer's formats:
// palette and blend table, GAF frames, TNT terrain tiles and feature cells,
// texture GAFs and drawable 3DO models (see docs/RE_MEMORY.md, "Rendering").

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "render/image.h"
#include "render/model.h"
#include "ta/hpi.h"
#include "ta/model3do.h"

namespace ta {

// palettes\PALETTE.PAL: 256 x {r, g, b, unused}.
bool load_palette(Vfs& vfs, render::Palette& out);
// palettes\PALETTE.ALP: the 256 x 256 half-and-half mixing table the game
// builds (and caches) from the palette; used for shadows and translucent sprites.
bool load_blend_table(Vfs& vfs, render::BlendTable& out);

// Decodes every frame of GAF sequence `seq` (layered frames are flattened:
// layers drawn in order onto the union of their rectangles).
bool gaf_sequence_frames(const std::vector<std::uint8_t>& gaf, int seq, std::vector<render::Sprite8>& out);

struct TntTiles {
    int width = 0, height = 0;               // tiles (32 x 32 pixels, 2 x 2 height cells)
    std::vector<std::uint16_t> map;          // tile index per tile
    std::vector<render::Sprite8> tiles;      // no key
};
bool tnt_load_tiles(const std::vector<std::uint8_t>& tnt, TntTiles& out);

// Cells holding a feature (index into the TNT feature list), row-major order.
struct TntFeatureCell {
    int x = 0, z = 0;
    std::uint16_t feature = 0;
};
bool tnt_feature_cells(const std::vector<std::uint8_t>& tnt, std::vector<TntFeatureCell>& out);

// The texture GAFs (textures\*.GAF in the game's enumeration order, without
// logos.GAF) and the logos GAF searched last, as the model loader uses them.
class TextureLibrary {
public:
    void load(Vfs& vfs);
    struct Hit {
        const render::ModelTexture* texture = nullptr;
        bool from_logos = false;
        int frames = 0;
    };
    Hit find(const std::string& name);

private:
    struct Gaf {
        std::vector<std::uint8_t> data;
        std::vector<std::string> names;
    };
    std::vector<Gaf> gafs_;   // textures, then logos last
    bool has_logos_ = false;
    std::map<std::pair<int, int>, std::unique_ptr<render::ModelTexture>> cache_;
};

// Drawable model for a 3DO tree, pieces indexed like to_pose_model(root).
// Texture binding follows the game: name looked up in the texture GAFs, then
// logos (10 frames = team coloured); not found -> flat colour 0xD1.
render::Model build_render_model(const Model3doPiece* root, TextureLibrary& textures);

} // namespace ta
