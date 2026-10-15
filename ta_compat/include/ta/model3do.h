#pragma once
// 3DO model reader reproducing TotalA.exe's load-time processing
// (see docs/RE_MEMORY.md, "3DO models").

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "sim/pose.h"
#include "ta/hpi.h"

namespace ta {

struct Model3doPrimitive {
    std::int32_t color_index = 0;
    std::vector<std::uint16_t> indices;
    std::array<std::uint16_t, 3> first3{};  // the file's first three index slots (read past short lists)
    std::string texture;  // texture name from the file ("" = none)
    std::array<std::uint8_t, 32> raw{};  // original 32-byte record (offsets as in the file)
};

struct Model3doPiece {
    std::int32_t version = 0;
    std::int32_t selection_primitive = -1;
    std::array<std::int32_t, 3> offset{};  // from parent, 16.16-style integer units
    std::string name;
    std::vector<std::array<std::int32_t, 3>> vertices;
    std::vector<Model3doPrimitive> primitives;
    std::unique_ptr<Model3doPiece> child;    // first child
    std::unique_ptr<Model3doPiece> sibling;  // next sibling
};

struct Model3doLoadResult {
    std::unique_ptr<Model3doPiece> root;
    std::string error;  // empty on success
};

// Parse a .3DO file. Applies what the game does on load: primitive reordering
// (selection primitive first, then a bubble sort by average vertex Y over
// primitives 1..n-1) and, when negate_xz is set, negation of every vertex X/Z
// and piece X/Z offset (the game does this for unit and weapon models).
Model3doLoadResult load_3do(const std::vector<std::uint8_t>& file, bool negate_xz = true);

// Height as the game computes it for unit definitions: the largest vertex Y
// (accumulating parent Y offsets), never below 0.
std::int32_t model_height(const Model3doPiece* root);

// Number of pieces in the tree.
int model_piece_count(const Model3doPiece* root);

// Engine pose model of the tree. Node indices follow a depth-first walk:
// node, its child subtree, then its next sibling.
sim::PoseModel to_pose_model(const Model3doPiece* root);

// The ground footprint the game fits units to the terrain with: X and Z of the
// vertices named by the first four index slots of the root piece's selection
// primitive (after load-time negation). `file` is the model file `root` was
// loaded from. False when the model has no selection primitive.
bool selection_footprint(const std::vector<std::uint8_t>& file, const Model3doPiece* root,
                         std::array<std::array<std::int32_t, 2>, 4>& out);

} // namespace ta
