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
#include "ta/battle_loader.h"
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

// The battle request a SkirmishSetup stands for (sides by sidedata.tdf index, the human
// player Local, computer players Computer, idle opponents Local).
BattleRequest to_battle_request(Vfs& vfs, const SkirmishSetup& setup);

// One unit type's drawing data (index = battle type index = definition index).
struct ClientUnitType {
    UnitTypeAssets* assets = nullptr;  // the loaded battle's
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
    bool load(const std::string& game_dir, const SkirmishSetup& setup, std::string& error,
              const std::function<void(float, const std::string&)>& progress = {});
    // Loads the game data and starts the battle through ta::load_battle (the front end's way);
    // `human` is the slot the client plays. `progress` (optional) hears the loading stages.
    bool load(const std::string& game_dir, const BattleRequest& request, int human, std::string& error,
              const std::function<void(float, const std::string&)>& progress = {});
    // Connects the battle's presentation events to the client (after both exist).
    void attach(client::GameClient* client) { client_ = client; }
    // Network games: the outside the battle gets (the network side over the client's own) and
    // the hook that attaches the network side to the battle (before its start and after it).
    std::function<sim::BattleOutside&(sim::BattleOutside&)> net_wrap;
    std::function<void(sim::Battle&)> net_attach;
    // Extra listeners for the battle's events (the front end's music and statistics).
    std::function<void(std::int32_t)> on_activity;  // combat activity (music battle/peace switching)

    sim::Battle& battle() { return *lb_->battle; }
    // The game folder the battle was loaded from (the original's working folder: files the
    // commands write go there).
    const std::string& game_dir() const { return game_dir_; }
    client::Content& content() { return content_; }
    const SkirmishSetup& setup() const { return setup_; }
    const BattleRequest& request() const { return request_; }
    LoadedBattle& loaded() { return *lb_; }
    // Start position of a player (world units), for the camera: its first unit (the commander).
    std::array<int, 2> start_position(int player) const;
    const UnitDefTable& defs() const { return defs_; }
    const std::string& weapon_name(int slot) const { return weapons_.slots[static_cast<std::size_t>(slot & 0xFF)].name; }

    // Implementation parts (client_battle_setup.cpp / client_content.cpp / client_scene.cpp).
    bool setup_battle(std::string& error, const std::function<void(float, const std::string&)>& progress);
    bool build_content(std::string& error);
    void fill_scene(render::BattleScene& scene, int view_player, std::uint32_t frame_counter);

    struct Outside;

    // Game data.
    std::string game_dir_;
    Vfs vfs_;
    SkirmishSetup setup_;
    BattleRequest request_;
    int human_ = 0;
    std::unique_ptr<LoadedBattle> lb_;  // the battle and everything it points into
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
    // The battle (its weapons, animations and random generator live in lb_).
    std::unique_ptr<Outside> outside_;
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
    std::vector<std::uint8_t> shade_table_, water_table_;  // unit looks (lit structures, under water)
    std::deque<sim::Unit> feature_proxies_;                 // 3D features drawn as the original's stand-in unit
    std::deque<render::UnitInstance> feature_proxy_views_;
    std::map<std::string, const std::vector<render::Sprite8>*> scene_sequences_;  // by "file|sequence"
    render::FogOfWar fog_;
    render::PoseDrawCache pose_cache_;  // drawn unit poses between frames (presentation)
    render::UnitSpriteCache sprite_cache_;  // unit sprites between frames (presentation)
    std::array<std::vector<render::Sprite8>, 4> fog_black_, fog_gray_;
    render::BlendTable gray_table_;
    std::map<const sim::PoseModel*, const render::Model*> model_of_pose_;
    // Shards' copies of their quad's animation cursor (0x421700 copies the primitive's cursor
    // and nothing advances the copy): the frame when each shard slot was filled, noted after
    // the step that filled it, before the texture cursors move on.
    std::array<std::int32_t, sim::kMaxShards> shard_frame_{};
    std::array<bool, sim::kMaxShards> shard_live_{};
    void note_new_shards();
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
    // A named sound of the sound bank (allsound.tdf's section `section`, file `file`): a sound
    // of its own even when another section names the same file (the original's bank keeps one
    // entry per name, so their copies do not mix); later lookups of the same file share it.
    int bank_sample(const std::string& section, const std::string& file);
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
    int font_number = 0;     // which of the description's font gadgets (in order) the caption uses
    bool active = true;
    int quickkey = 0;
    int status = 0;
    std::string text, panel, filename;
    int stages = 0;          // buttons: switch stages
    bool grayed = false;     // buttons: grayedout
    std::string link;        // labels: the button they name
    std::string crdefault, escdefault, defaultfocus;  // header: Enter / Escape defaults, focus
    int max_chars = 0;       // text fields: maxchars
};
bool parse_gui(const std::vector<std::uint8_t>& file, std::vector<GuiGadget>& out);

// 8-bit PCX (RLE): pixels and its 256-colour palette.
bool decode_pcx(const std::vector<std::uint8_t>& file, render::Sprite8& out, render::Palette* palette);

} // namespace ta
