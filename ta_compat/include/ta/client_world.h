#pragma once
// A skirmish for the playable client from an installed copy of the original game:
// the battle set-up (adapter until the game-flow agent's battle set-up lands, see
// client_battle_setup.cpp), the interface content (client_content.cpp: GUI files,
// side data, fonts, cursors, sounds, build menus) and the battle view's scene
// (client_scene.cpp). See docs/RE_MEMORY.md, "Client".

#include <array>
#include <deque>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "client/content.h"
#include "client/game_client.h"
#include "render/battle_view.h"
#include "render/model.h"
#include "sim/battle.h"
#include "sim/rng.h"
#include "ta/battle_features.h"
#include "ta/battle_types.h"
#include "ta/featuredefs.h"
#include "ta/hpi.h"
#include "ta/render_assets.h"
#include "ta/tdf.h"
#include "ta/tnt.h"
#include "ta/unitdefs.h"
#include "ta/weapondefs.h"

namespace ta {

struct SkirmishPlayer {
    int side = 0;        // index into sidedata.tdf's sides (0 ARM, 1 CORE)
    bool ai = false;     // computer player (controller 2) instead of human
    int start_pos = -1;  // map start position (StartPosN - 1); -1 = by player index
};

struct SkirmishSetup {
    std::string map = "Moon Quartet";
    std::vector<SkirmishPlayer> players{{0, false, -1}, {1, true, -1}};
    int human = 0;                   // the player the client plays
    float start_energy = 1000.0f, start_metal = 1000.0f;
    bool line_of_sight = true;       // fog of war (live sight counts)
    bool mapping = true;             // the map starts unexplored
    bool true_los = false;
    std::uint32_t seed = 0x12345678u;  // the battle's random seed (the original takes it from the set-up)
};

// One unit type's drawing data (index = battle type index = definition index).
struct ClientUnitType {
    UnitTypeAssets assets;
    std::unique_ptr<Model3doPiece> root;
    render::Model model;
    bool ok = false;
};

class ClientWorld {
public:
    ClientWorld();
    ~ClientWorld();
    ClientWorld(const ClientWorld&) = delete;
    ClientWorld& operator=(const ClientWorld&) = delete;
    // Loads the game data and sets up the skirmish; false with a reason on failure.
    bool load(const std::string& game_dir, const SkirmishSetup& setup, std::string& error);
    // Connects the battle's presentation events to the client (after both exist).
    void attach(client::GameClient* client) { client_ = client; }

    sim::Battle& battle() { return *battle_; }
    client::Content& content() { return content_; }
    const SkirmishSetup& setup() const { return setup_; }
    // Start position of a player (world units), for the camera.
    std::array<int, 2> start_position(int player) const;
    const UnitDefTable& defs() const { return defs_; }
    const std::string& weapon_name(int slot) const { return weapons_.slots[static_cast<std::size_t>(slot & 0xFF)].name; }

    // Implementation parts (client_battle_setup.cpp / client_content.cpp / client_scene.cpp).
    bool setup_battle(std::string& error);
    bool build_content(std::string& error);
    void fill_scene(render::BattleScene& scene, int view_player, std::uint32_t frame_counter);

    struct Outside;

    // Game data.
    Vfs vfs_;
    SkirmishSetup setup_;
    render::Palette palette_{};
    render::BlendTable blend_;
    WeaponDefTable weapons_;
    FeatureDefTable fdefs_;
    UnitDefContext ctx_;
    UnitDefTable defs_;
    TextureLibrary textures_;
    std::vector<std::uint8_t> tnt_;
    TntTiles tiles_;
    sim::HeightMap hm_;
    TntInfo tnt_info_;
    std::vector<std::array<int, 2>> start_positions_;  // by start position index
    // The battle.
    std::vector<sim::WeaponType> weapon_types_;
    sim::BattleRng rng_;
    std::unique_ptr<Outside> outside_;
    std::unique_ptr<sim::Battle> battle_;
    std::unique_ptr<AnimImporter> anim_import_;
    std::vector<std::string> feature_models_;
    std::vector<std::unique_ptr<ClientUnitType>> types_;  // by battle type index
    // Interface content and the storage its pointers point into.
    client::Content content_;
    std::deque<std::vector<render::Sprite8>> sequences_;
    std::deque<render::Sprite8> sprites_;
    std::deque<client::Font> fonts_;
    std::map<std::string, std::vector<std::uint8_t>> gaf_cache_;
    std::map<std::string, int> sample_index_;
    client::GameClient* client_ = nullptr;
    // Scene drawing caches.
    std::map<std::uint32_t, std::vector<render::Sprite8>> seq_frames_;
    struct FeatureModel {
        std::vector<std::uint8_t> file;
        std::unique_ptr<Model3doPiece> root;
        sim::PoseModel pose;
        render::Model model;
        bool ok = false, tried = false;
    };
    std::map<std::uint32_t, FeatureModel> feature_model_cache_;
    std::array<std::vector<render::Sprite8>, 3> glows_;
    render::BlendTable light_table_;
    render::FogOfWar fog_;
    std::array<std::vector<render::Sprite8>, 4> fog_black_, fog_gray_;
    render::BlendTable gray_table_;
    std::map<const sim::PoseModel*, const render::Model*> model_of_pose_;
    struct WeaponModel {
        std::vector<std::uint8_t> file;
        std::unique_ptr<Model3doPiece> root;
        sim::PoseModel pose;
        render::Model model;
    };
    std::map<std::string, std::unique_ptr<WeaponModel>> weapon_models_;

    // Helpers.
    const std::vector<std::uint8_t>* gaf(const std::string& path);
    const std::vector<render::Sprite8>* sequence(const std::string& path, const std::string& name);
    const client::Font* font(const std::string& name);
    int sample(const std::string& name);  // sounds\<name>.wav into the sample table, -1 when missing
    const std::vector<render::Sprite8>* frames_of(std::uint32_t seq);
};

// Parsers (exposed for tests and tools).
// A one-bit font file (fonts\*.FNT): u8 height, u8, u8 raise, u8 first character, then
// u16 offsets per character from the first; each glyph: u8 width, then width x height bits,
// most significant first, rows packed one after another.
bool parse_fnt(const std::vector<std::uint8_t>& file, client::Font& out);

// One gadget of a GUI file (guis\*.GUI, the TDF format): [GADGETn] { [COMMON] {...} ... }.
struct GuiGadget {
    int id = 0;  // 0 the header (panel origin), 1 button, 5 label, 7 font, ...
    int assoc = 0;
    std::string name;
    int x = 0, y = 0, w = 0, h = 0;
    int attribs = 0, common_attribs = 0, colorf = 0, colorb = 0;
    bool active = true;
    int quickkey = 0;
    int status = 0;
    std::string text, panel, filename;
};
bool parse_gui(const std::vector<std::uint8_t>& file, std::vector<GuiGadget>& out);

// 8-bit PCX (RLE): pixels and its 256-colour palette.
bool decode_pcx(const std::vector<std::uint8_t>& file, render::Sprite8& out, render::Palette* palette);

} // namespace ta
