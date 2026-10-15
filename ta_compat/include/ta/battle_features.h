#pragma once
// Imported definitions -> the battle's map features, animation sequences and
// effect art (sim/features.h, sim/effects.h, sim/anim.h), set up the way the
// original loads a battle (see docs/RE_MEMORY.md, "Features and wrecks").

#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "sim/battle.h"
#include "ta/featuredefs.h"
#include "ta/hpi.h"
#include "ta/weapondefs.h"

namespace ta {

// Loads GAF sequences into an animation library. The original keeps separate copies
// of a file loaded by different loaders (each 2D feature that loads its own file, and
// the shared cache weapons and the effect art use), and a loader that clears a
// sequence's loop byte changes only its own copy; `domain` names the copy.
class AnimImporter {
public:
    AnimImporter(Vfs& vfs, sim::AnimLibrary& lib) : vfs_(vfs), lib_(lib) {}
    // Address of sequence `seq` of anims\<file>.GAF in copy `domain`, 0 when missing.
    // `clear_loop`: the loader clears the sequence's loop byte.
    std::uint32_t sequence(const std::string& domain, const std::string& file, const std::string& seq, bool clear_loop);
    // The file's bytes (cached), nullptr when missing.
    const std::vector<std::uint8_t>* file(const std::string& name);
    // Sequence -> (file, index in the file) for the presentation.
    const std::map<std::uint32_t, std::pair<std::string, int>>& origins() const { return origins_; }
    sim::AnimLibrary& library() { return lib_; }

private:
    Vfs& vfs_;
    sim::AnimLibrary& lib_;
    std::map<std::string, std::vector<std::uint8_t>> files_;
    std::map<std::string, std::uint32_t> loaded_;  // domain|file|index -> address
    std::map<std::uint32_t, std::pair<std::string, int>> origins_;
};

// Reads one GAF sequence's timing and frame boxes (no address).
bool gaf_sequence_timing(const std::vector<std::uint8_t>& gaf, int seq, sim::AnimSequence& out);

// Feature types in table order. `models` receives a model id per 3D type (index into
// `model_names`, 1-based; 0 for 2D types).
std::vector<sim::FeatureTypeInfo> to_feature_types(const FeatureDefTable& defs, const std::vector<sim::WeaponType>& weapons,
                                                   AnimImporter& anims, std::vector<std::string>* model_names = nullptr);
// The unit type each feature resurrects into (FeatureTypeInfo::resurrect_type): its name up to
// the first '_' (within 63 characters) matched case-insensitively against `type_names` (the
// battle's unit type names by type index; index 0 is never matched).
void bind_resurrect_types(std::vector<sim::FeatureTypeInfo>& features, const FeatureDefTable& defs,
                          const std::vector<std::string>& type_names);

// Explosion sequences of the weapon types (index = weapon slot).
void bind_weapon_explosions(const WeaponDefTable& defs, std::vector<sim::WeaponType>& weapons, AnimImporter& anims);

// The effect art: the three generated glows (12, 15 and 15 frames of 2 steps), fx.gaf's
// explosion, explode2-5, nuke1 and the water and lava splashes.
void load_effect_art(AnimImporter& anims, sim::EffectsEnvironment& env);

// The map grid's features from a TNT file, after Battle::reset_features_and_effects: every
// cell's owner nibble set to "nobody", void cells marked, then each listed feature placed in
// row-major order (feature index below the TNT's feature count).
// `place` false: only the owner nibbles and void cells (a saved game places its own features).
void place_tnt_features(sim::Battle& battle, const std::vector<std::uint8_t>& tnt, bool place = true);

// The route search's per-type blocking flags (Battle::feature_blocks) from the battle's
// feature types (flag 0x40, the one the original's passability reads). Call after
// `battle.features` is set and before the passability grids are built.
void bind_feature_blocks(sim::Battle& battle);

} // namespace ta
