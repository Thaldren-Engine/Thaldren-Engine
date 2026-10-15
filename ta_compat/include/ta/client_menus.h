#pragma once
// The original game's front end on the client's front end runner (client/front_end.h):
// its screens from guis\*.GUI with their backgrounds and art, its sounds and fonts, the
// skirmish set-up, options, loading, in-game menus, the end of a battle and the music
// tracks (spec: docs/RE_MEMORY.md, "Client", "Menus").
//
// Other parts (campaign, multiplayer, save/load) plug in through client::FrontEndHooks;
// they can build screens from the game's GUI files with MenuArt::layout() and
// client::LayoutScreen, and start battles with SkirmishSession-like sessions.

#include <deque>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "client/front_end.h"
#include "client/menu.h"
#include "client/music.h"
#include "ta/battle_loader.h"
#include "ta/client_world.h"
#include "ta/hpi.h"

namespace ta {

// Everything the menus draw and play, loaded from the game files on demand.
class MenuArt {
public:
    bool load(const std::string& game_dir, std::string& error);

    client::MenuContent& content() { return content_; }
    const std::string& game_dir() const { return game_dir_; }
    Vfs& vfs() { return vfs_; }

    // A screen's layout by GUI file name ("SKIRMISH" or "SKIRMISH.GUI"), with the
    // background the original shows behind it.
    const client::ScreenLayout* layout(const std::string& gui);
    // A bitmaps\<name>.PCX picture with its own palette (null when missing).
    const render::Sprite8* picture(const std::string& name, const render::Palette** palette = nullptr);
    // Frames of a GAF sequence (null when missing).
    const std::vector<render::Sprite8>* sequence(const std::string& gaf_path, const std::string& name);
    // A sound of the sound bank by its allsound.tdf section name (-1: none).
    int sound(const std::string& name);
    // Interface colour k for a palette (GUIPAL entry k's nearest colour in it).
    std::uint8_t gui_colour(const render::Palette& palette, int k) const;
    // A map's preview picture (the TNT's minimap), scaled into w x h; null when missing.
    const render::Sprite8* map_preview(const std::string& map, int w, int h);

    // The game's music tracks: music\<n>.mp3 for n = 1.. (decoded on demand).
    int music_tracks() const { return music_tracks_; }
    std::shared_ptr<const client::Sample> music_track(int n);

    // Skirmish maps (maps\*.ota with a network schema), sorted by name.
    const std::vector<std::string>& skirmish_maps();
    // Side names from gamedata\sidedata.tdf (ARM, CORE, ...).
    const std::vector<std::string>& sides() const { return sides_; }

private:
    const std::vector<std::uint8_t>* gaf(const std::string& path);
    client::Font gaf_font(const std::string& path);

    std::string game_dir_;
    Vfs vfs_;
    client::MenuContent content_;
    std::array<render::Rgb, 256> guipal_{};
    std::map<std::string, std::vector<std::uint8_t>> gafs_;
    std::map<std::string, const std::vector<render::Sprite8>*> seq_index_;
    std::deque<std::vector<render::Sprite8>> sequences_;
    std::deque<render::Sprite8> sprites_;
    std::deque<render::Palette> palettes_;
    std::map<std::string, std::pair<const render::Sprite8*, const render::Palette*>> pictures_;
    std::map<std::string, std::unique_ptr<client::ScreenLayout>> layouts_;
    std::deque<client::Font> fonts_;
    std::map<std::string, int> sound_index_;
    int music_tracks_ = 0;
    std::map<int, std::shared_ptr<const client::Sample>> music_cache_;
    std::vector<std::string> maps_;
    bool maps_done_ = false;
    std::vector<std::string> sides_;
    std::map<std::string, std::unique_ptr<render::Sprite8>> previews_;
};

// A skirmish (or any battle built from a BattleRequest) with the playable client.
class SkirmishSession : public client::BattleSession {
public:
    // Loads and starts the battle; null with `error` on failure.
    static std::unique_ptr<SkirmishSession> start(const std::string& game_dir, const BattleRequest& request, int human,
                                                  const client::ClientOptions& options, const client::LoadProgress& progress,
                                                  std::string& error);
    sim::Battle& battle() override { return world_->battle(); }
    client::Content& content() override { return world_->content(); }
    client::GameClient& client() override { return *client_; }
    client::BattleOutcome outcome() override;
    void surrender() override;
    std::vector<client::PlayerStats> stats() override;
    ClientWorld& world() { return *world_; }

private:
    std::unique_ptr<ClientWorld> world_;
    std::unique_ptr<client::GameClient> client_;
    bool surrendered_ = false;
};

// Client options from the settings (interface type, speeds, text, unit chat, ...).
client::ClientOptions client_options(const client::Settings& settings, int width, int height);

// Fills the front end set-up with the original's screens. `art` must outlive the run.
void make_front_end(MenuArt& art, client::MusicPlayer* music, client::FrontEndSetup& setup);

}  // namespace ta
