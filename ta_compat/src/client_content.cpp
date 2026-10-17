// The playable client's interface content from the original game's files:
// GUI files, side data, fonts, cursors, panels, build menus, sounds and shot
// looks (spec: docs/RE_MEMORY.md, "Client").

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <functional>

#include "client/audio.h"
#include "ta/client_world.h"
#include "ta/gaf.h"
#include "ta/model3do.h"

namespace ta {
namespace {

std::string upper(std::string s) {
    for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

TdfParseResult parse(const std::vector<std::uint8_t>& f) {
    return tdf_parse(std::string_view(reinterpret_cast<const char*>(f.data()), f.size()));
}

client::Region region(const TdfSection* side, const char* name) {
    client::Region r;
    if (const TdfSection* s = side ? side->child(name) : nullptr) {
        r.x1 = s->get_int("x1", 0);
        r.y1 = s->get_int("y1", 0);
        r.x2 = s->get_int("x2", 0);
        r.y2 = s->get_int("y2", 0);
    }
    return r;
}

// Nearest palette entry by the sum of absolute channel differences (first of equals).
std::uint8_t nearest(const render::Palette& pal, int r, int g, int b) {
    int best = 0, best_d = 1 << 30;
    for (int i = 0; i < 256; ++i) {
        const render::Rgb& c = pal[static_cast<std::size_t>(i)];
        const int d = std::abs(r - c.r) + std::abs(g - c.g) + std::abs(b - c.b);
        if (d < best_d) {
            best_d = d;
            best = i;
        }
    }
    return static_cast<std::uint8_t>(best);
}

// The box the selection outline and the pointer's pick test use (the original's model bounds
// routine called without its walk over the other pieces): the root piece alone. The box starts
// as the point 0 (so it always holds the unit's origin) and takes in the root piece's vertices
// moved by its offset, only when the piece has more than two vertices.
void model_box(const Model3doPiece* root, std::array<std::int32_t, 3>& lo, std::array<std::int32_t, 3>& hi) {
    lo = {0, 0, 0};
    hi = {0, 0, 0};
    if (!root || root->vertices.size() <= 2) return;
    for (const auto& v : root->vertices)
        for (std::size_t k = 0; k < 3; ++k) {
            const std::int32_t c = root->offset[k] + v[k];
            if (c > hi[k]) hi[k] = c;
            if (c < lo[k]) lo[k] = c;
        }
}

} // namespace

bool parse_fnt(const std::vector<std::uint8_t>& f, client::Font& out) {
    if (f.size() < 4) return false;
    out.height = f[0];
    out.raise = f[2];
    const int first = f[3];
    for (int c = first; c < 256; ++c) {
        const std::size_t at = 4 + static_cast<std::size_t>(c - first) * 2;
        if (at + 2 > f.size()) break;
        const std::uint16_t off = static_cast<std::uint16_t>(f[at] | (f[at + 1] << 8));
        if (off == 0 || off >= f.size()) continue;
        client::Glyph& g = out.glyphs[static_cast<std::size_t>(c)];
        g.width = f[off];
        g.present = true;
        g.ink.assign(static_cast<std::size_t>(g.width * out.height), 0);
        std::size_t bit = 0;
        for (std::size_t k = 0; k < g.ink.size(); ++k, ++bit) {
            const std::size_t byte = off + 1 + bit / 8;
            if (byte < f.size()) g.ink[k] = (f[byte] >> (7 - bit % 8)) & 1;
        }
    }
    return true;
}

bool parse_gui(const std::vector<std::uint8_t>& file, std::vector<GuiGadget>& out) {
    out.clear();
    const TdfParseResult r = parse(file);
    if (!r.root) return false;
    for (const auto& sec : r.root->children()) {
        GuiGadget g;
        const TdfSection* c = sec->child("COMMON");
        if (!c) continue;
        g.id = c->get_int("id", 0);
        g.assoc = c->get_int("assoc", 0);
        g.name = c->get_string("name", 32, "");
        g.x = c->get_int("xpos", 0);
        g.y = c->get_int("ypos", 0);
        g.w = c->get_int("width", 0);
        g.h = c->get_int("height", 0);
        g.attribs = c->get_int("attribs", 0);
        g.common_attribs = c->get_int("commonattribs", 0);
        g.colorf = c->get_int("colorf", 0);
        g.colorb = c->get_int("colorb", 0);
        g.active = c->get_int("active", 1) != 0;
        g.quickkey = sec->get_int("quickkey", 0);
        g.status = sec->get_int("status", 0);
        g.text = sec->get_string("text", 256, "");
        g.panel = sec->get_string("panel", 32, "");
        g.filename = sec->get_string("filename", 32, "");
        g.stages = sec->get_int("stages", 0);
        g.grayed = sec->get_int("grayedout", 0) != 0;
        g.link = sec->get_string("link", 32, "");
        g.crdefault = sec->get_string("crdefault", 32, "");
        g.escdefault = sec->get_string("escdefault", 32, "");
        g.defaultfocus = sec->get_string("defaultfocus", 32, "");
        out.push_back(std::move(g));
    }
    return true;
}

bool decode_pcx(const std::vector<std::uint8_t>& f, render::Sprite8& out, render::Palette* palette) {
    if (f.size() < 128 + 769 || f[0] != 10 || f[3] != 8 || f[65] != 1) return false;
    auto u16 = [&](std::size_t o) { return static_cast<int>(f[o] | (f[o + 1] << 8)); };
    const int w = u16(8) - u16(4) + 1, h = u16(10) - u16(6) + 1, stride = u16(66);
    if (w <= 0 || h <= 0 || stride < w) return false;
    out.width = w;
    out.height = h;
    out.has_key = false;
    out.pixels.assign(static_cast<std::size_t>(w) * static_cast<std::size_t>(h), 0);
    std::size_t p = 128;
    for (int y = 0; y < h; ++y) {
        int x = 0;
        while (x < stride && p < f.size()) {
            std::uint8_t b = f[p++];
            int n = 1;
            if ((b & 0xC0) == 0xC0) {
                n = b & 0x3F;
                b = p < f.size() ? f[p++] : 0;
            }
            for (int k = 0; k < n && x < stride; ++k, ++x)
                if (x < w) out.pixels[static_cast<std::size_t>(y * w + x)] = b;
        }
    }
    if (palette && f[f.size() - 769] == 12)
        for (int i = 0; i < 256; ++i)
            (*palette)[static_cast<std::size_t>(i)] = {f[f.size() - 768 + static_cast<std::size_t>(i) * 3], f[f.size() - 767 + static_cast<std::size_t>(i) * 3],
                                                       f[f.size() - 766 + static_cast<std::size_t>(i) * 3]};
    return true;
}

const std::vector<std::uint8_t>* ClientWorld::gaf(const std::string& path) {
    auto it = gaf_cache_.find(upper(path));
    if (it != gaf_cache_.end()) return it->second.empty() ? nullptr : &it->second;
    std::vector<std::uint8_t>& d = gaf_cache_[upper(path)];
    vfs_.read(path, d);
    return d.empty() ? nullptr : &d;
}

const std::vector<render::Sprite8>* ClientWorld::sequence(const std::string& path, const std::string& name) {
    const std::vector<std::uint8_t>* data = gaf(path);
    if (!data) return nullptr;
    GafDirectory dir;
    if (!gaf_read_directory(*data, dir)) return nullptr;
    const int s = gaf_find_sequence(dir, name);
    if (s < 0) return nullptr;
    sequences_.emplace_back();
    if (!gaf_sequence_frames(*data, s, sequences_.back()) || sequences_.back().empty()) {
        sequences_.pop_back();
        return nullptr;
    }
    return &sequences_.back();
}

const client::Font* ClientWorld::font(const std::string& name) {
    std::vector<std::uint8_t> f;
    if (!vfs_.read("fonts\\" + name + ".FNT", f)) return nullptr;
    fonts_.emplace_back();
    if (!parse_fnt(f, fonts_.back())) {
        fonts_.pop_back();
        return nullptr;
    }
    return &fonts_.back();
}

int ClientWorld::sample(const std::string& name) {
    if (name.empty()) return -1;
    const std::string key = upper(name);
    auto it = sample_index_.find(key);
    if (it != sample_index_.end()) return it->second;
    std::string path = name;
    if (path.find('.') == std::string::npos) path += ".wav";
    std::vector<std::uint8_t> f;
    client::Sample s;
    int index = -1;
    if ((vfs_.read("sounds\\" + path, f) || vfs_.read(path, f)) && client::decode_wav(f, s)) {
        s.name = key;
        content_.samples.push_back(std::move(s));
        index = static_cast<int>(content_.samples.size()) - 1;
    }
    sample_index_[key] = index;
    return index;
}

bool ClientWorld::build_content(std::string& error) {
    client::Content& c = content_;
    c.palette = palette_;
    c.blend = blend_;
    // Interface colours: each interface palette entry's nearest game palette index.
    {
        std::vector<std::uint8_t> gp;
        if (vfs_.read("palettes\\GUIPAL.PAL", gp) && gp.size() >= 1024)
            for (int i = 0; i < 256; ++i)
                c.hud[static_cast<std::size_t>(i)] = nearest(palette_, gp[static_cast<std::size_t>(i) * 4], gp[static_cast<std::size_t>(i) * 4 + 1],
                                                             gp[static_cast<std::size_t>(i) * 4 + 2]);
        else
            for (int i = 0; i < 256; ++i) c.hud[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(i);
    }
    const std::string common = "anims\\commongui.GAF";
    // Buttons from a GUI file's gadgets: the gadget's name is the art's sequence name.
    auto unit_index = [&](const std::string& name) -> std::uint16_t {
        for (std::size_t k = 1; k < defs_.units.size(); ++k)
            if (stricmp_c(defs_.units[k].unit_name, name) == 0) return static_cast<std::uint16_t>(k);
        return 0;
    };
    auto button_art = [&](const std::string& gui_gaf, const std::string& name, bool unit) -> const std::vector<render::Sprite8>* {
        if (const auto* s = sequence(gui_gaf, name)) return s;
        if (const auto* s = sequence(common, name)) return s;
        if (unit) {
            if (const auto* s = sequence("anims\\" + name + "_GADGET.GAF", name)) return s;
            std::vector<std::uint8_t> pcx;
            render::Sprite8 pic;
            render::Palette pp{};
            if (vfs_.read("unitpics\\" + name + ".PCX", pcx) && decode_pcx(pcx, pic, &pp)) {
                // Remapped to the game palette (normal, pressed and disabled all alike).
                std::map<int, std::uint8_t> remap;
                for (std::uint8_t& px : pic.pixels) {
                    auto it = remap.find(px);
                    if (it == remap.end())
                        it = remap.emplace(px, nearest(palette_, pp[px].r, pp[px].g, pp[px].b)).first;
                    px = it->second;
                }
                sequences_.emplace_back(3, pic);
                return &sequences_.back();
            }
        }
        return nullptr;
    };
    auto make_panel = [&](const std::string& gui_name, const std::string& prefix, client::Panel& out) -> bool {
        std::vector<std::uint8_t> f;
        std::vector<GuiGadget> gs;
        if (!vfs_.read("guis\\" + gui_name + ".GUI", f) || !parse_gui(f, gs) || gs.empty()) return false;
        const GuiGadget& head = gs[0];
        out.x = head.x;
        out.y = head.y;
        if (!head.panel.empty())
            if (const auto* p = sequence(common, head.panel)) out.background = &p->front();
        const std::string gui_gaf = "anims\\" + gui_name + ".GAF";
        for (std::size_t i = 1; i < gs.size(); ++i) {
            const GuiGadget& g = gs[i];
            if (g.id != 1) continue;
            client::Button b;
            b.x = out.x + g.x;
            b.y = out.y + g.y;
            b.w = g.w;
            b.h = g.h;
            b.quickkey = g.quickkey > 0 && g.quickkey < 256 ? std::tolower(g.quickkey) : 0;
            const std::string n = upper(g.name);
            const std::string tail = n.size() > prefix.size() && n.compare(0, prefix.size(), prefix) == 0 ? n.substr(prefix.size()) : n;
            static const std::pair<const char*, client::Action> roles[] = {
                {"ORDERS", client::Action::OrdersTab}, {"BUILD", client::Action::BuildTab},   {"PREV", client::Action::PrevPage},
                {"NEXT", client::Action::NextPage},    {"MOVE", client::Action::Move},        {"STOP", client::Action::Stop},
                {"PATROL", client::Action::Patrol},    {"ATTACK", client::Action::Attack},    {"DEFEND", client::Action::Guard},
                {"FIREORD", client::Action::FireOrders}, {"MOVEORD", client::Action::MoveOrders}, {"ONOFF", client::Action::OnOff},
                {"CAPTURE", client::Action::Capture},  {"RECLAIM", client::Action::Reclaim},  {"REPAIR", client::Action::Repair},
                {"LOAD", client::Action::Load},        {"UNLOAD", client::Action::Unload},    {"CLOAK", client::Action::Cloak},
                {"BLAST", client::Action::Special},
            };
            const std::uint16_t unit = unit_index(g.name);
            if (unit && n != "IGPATCH") {
                b.action = client::Action::BuildUnit;
                b.unit_type = unit;
            } else {
                for (const auto& [name, act] : roles)
                    if (tail == name) b.action = act;
            }
            b.staged = (g.attribs & 0x100) != 0;
            b.frames = button_art(gui_gaf, g.name, unit != 0);
            if (b.frames || b.action != client::Action::None) out.buttons.push_back(b);
        }
        return true;
    };

    // Sides.
    std::vector<std::uint8_t> sd;
    vfs_.read("gamedata\\sidedata.tdf", sd);
    const TdfParseResult sr = parse(sd);
    for (int k = 0;; ++k) {
        const TdfSection* s = sr.root ? sr.root->child("SIDE" + std::to_string(k)) : nullptr;
        if (!s) break;
        client::SideLayout L;
        L.logo = region(s, "LOGO");
        L.energy_bar = region(s, "ENERGYBAR");
        L.energy_num = region(s, "ENERGYNUM");
        L.energy_max = region(s, "ENERGYMAX");
        L.energy_zero = region(s, "ENERGY0");
        L.metal_bar = region(s, "METALBAR");
        L.metal_num = region(s, "METALNUM");
        L.metal_max = region(s, "METALMAX");
        L.metal_zero = region(s, "METAL0");
        L.energy_produced = region(s, "ENERGYPRODUCED");
        L.energy_consumed = region(s, "ENERGYCONSUMED");
        L.metal_produced = region(s, "METALPRODUCED");
        L.metal_consumed = region(s, "METALCONSUMED");
        L.logo2 = region(s, "LOGO2");
        L.unit_name = region(s, "UNITNAME");
        L.damage_bar = region(s, "DAMAGEBAR");
        L.unit_metal_make = region(s, "UNITMETALMAKE");
        L.unit_metal_use = region(s, "UNITMETALUSE");
        L.unit_energy_make = region(s, "UNITENERGYMAKE");
        L.unit_energy_use = region(s, "UNITENERGYUSE");
        L.mission_text = region(s, "MISSIONTEXT");
        L.unit_name2 = region(s, "UNITNAME2");
        L.damage_bar2 = region(s, "DAMAGEBAR2");
        L.name = region(s, "NAME");
        L.description = region(s, "DESCRIPTION");
        L.energy_colour = static_cast<std::uint8_t>(s->get_int("energycolor", 0));
        L.metal_colour = static_cast<std::uint8_t>(s->get_int("metalcolor", 0));
        L.font = font(s->get_string("font", 256, "console"));
        const std::string intgaf = "anims\\" + s->get_string("intgaf", 30, "") + ".GAF";
        if (const auto* q = sequence(intgaf, "PANELTOP")) L.panel_top = &q->front();
        if (const auto* q = sequence(intgaf, "PANELSIDE")) L.panel_side = &q->front();
        if (const auto* q = sequence(intgaf, "PANELBOT")) L.panel_bottom = &q->front();
        const std::string prefix = upper(s->get_string("nameprefix", 4, ""));
        if (!make_panel(prefix + "GEN", prefix, L.orders)) std::printf("no %sGEN.GUI\n", prefix.c_str());
        make_panel(prefix + "MAIN2", prefix, L.idle);
        c.sides.push_back(std::move(L));
    }
    if (c.sides.empty()) {
        error = "no sides in gamedata\\sidedata.tdf";
        return false;
    }

    // Cursors, radar dots, the paused sign.
    {
        const std::string cur = "anims\\CURSORS.GAF";
        const std::pair<client::CursorKind, const char*> looks[] = {
            {client::CursorKind::Normal, "cursornormal"},   {client::CursorKind::Select, "cursorselect"},
            {client::CursorKind::Move, "cursormove"},       {client::CursorKind::Attack, "cursorattack"},
            {client::CursorKind::Guard, "cursordefend"},    {client::CursorKind::Patrol, "cursorpatrol"},
            {client::CursorKind::Repair, "cursorrepair"},   {client::CursorKind::Reclaim, "cursorreclamate"},
            {client::CursorKind::Capture, "cursorcapture"}, {client::CursorKind::Load, "cursorload"},
            {client::CursorKind::Unload, "cursorunload"},   {client::CursorKind::FindSite, "cursorfindsite"},
            {client::CursorKind::TooFar, "cursortoofar"},   {client::CursorKind::Busy, "cursorhourglass"},
            {client::CursorKind::Red, "cursorred"},
        };
        for (const auto& [k, name] : looks) c.cursors.looks[static_cast<std::size_t>(k)] = sequence(cur, name);
        c.radar_dots = sequence("anims\\FX.GAF", "radlogo");
        c.team_logos = sequence("textures\\logos.GAF", "32xlogos");
        for (std::size_t p = 0; p < c.player_colour.size(); ++p) c.player_colour[p] = request_.start.players[p].colour;
        if (const auto* q = sequence("anims\\FX.GAF", "radlogohigh")) c.radar_dot_hover = &q->front();
        if (const auto* q = sequence("anims\\IGTITLES.GAF", "igpaused")) c.paused_sign = &q->front();
    }

    // The minimap picture stored in the map file (header word 10), cut to the map's shape.
    {
        auto rd32 = [&](std::size_t o) -> std::uint32_t {
            if (o + 4 > tnt_.size()) return 0;
            std::uint32_t v;
            std::memcpy(&v, &tnt_[o], 4);
            return v;
        };
        const std::uint32_t at = rd32(40);
        const int w = static_cast<int>(rd32(at)), h = static_cast<int>(rd32(at + 4));
        if (at && w > 0 && h > 0 && w <= 1024 && h <= 1024 && at + 8 + static_cast<std::size_t>(w) * h <= tnt_.size()) {
            const int mw = hm_.width(), mh = hm_.height();
            int cw = w, ch = h;
            if (mw > mh) ch = std::max(1, h * mh / mw);
            else if (mh > mw) cw = std::max(1, w * mw / mh);
            render::Sprite8 pic;
            pic.width = cw;
            pic.height = ch;
            pic.has_key = false;
            pic.pixels.resize(static_cast<std::size_t>(cw) * ch);
            for (int y = 0; y < ch; ++y)
                std::memcpy(&pic.pixels[static_cast<std::size_t>(y) * cw], &tnt_[at + 8 + static_cast<std::size_t>(y) * w], static_cast<std::size_t>(cw));
            sprites_.push_back(std::move(pic));
            c.minimap_picture = &sprites_.back();
        }
    }

    // Sounds: the interface's from allsound.tdf, the units' by their sound category.
    {
        std::vector<std::uint8_t> f;
        vfs_.read("gamedata\\ALLSOUND.TDF", f);
        const TdfParseResult a = parse(f);
        auto ui = [&](const char* sec) {
            const TdfSection* s = a.root ? a.root->child(sec) : nullptr;
            return s ? sample(s->get_string("sound", 64, "")) : -1;
        };
        // The status line's text by mission kind: the display name of each entry of the
        // original's mission table (0x00512344; the entries' first field, read from the static
        // tables the table is built from), by the entries' lookup names. English: the
        // translation table leaves them as they are.
        {
            static const std::pair<const char*, const char*> kStatus[] = {
                {"", "Ready"}, {"Activate", "Activate"}, {"AirStrike", "Airstrike"}, {"AirToAir", "Engaging target"},
                {"AirToGround", "Engaging target"}, {"AirToGroundHover", "Engaging target"}, {"Attack_Chase", "Attacking"},
                {"Attack_Kamikaze", "Attacking"}, {"Attack_NoMove", "Attacking"}, {"AttackSpecial", "Annihilating"},
                {"AttackUType", "Attacking"}, {"BeCarried", "Being transported"}, {"BuildingBuild", "Nanolathing"},
                {"BuildWeapon", "Nanolathing"}, {"Capture", "Capturing"}, {"Cloak_Off", "Decloaking"}, {"Cloak_On", "Cloaking"},
                {"Deactivate", "Deactivate"}, {"Follow_Ground", "Guarding"}, {"GetBuilt", "Under construction"},
                {"Ground_Pickup", "Loading"}, {"Ground_Unload", "Unloading"}, {"Guard_NoMove", "Ready"}, {"HelpBuild", "Nanolathing"},
                {"MakeSelectable", "Unit is available"}, {"MobileBuild", "Nanolathing"}, {"Move_Ground", "Moving"},
                {"Paralyze", "Paralyzed"}, {"Park", "Parking"}, {"Patrol", "Patrolling"}, {"QMove", "Ready with orders"},
                {"QPatrol", "Ready with orders"}, {"Reclaim", "Reclaiming"}, {"ReclaimUnit", "Reclaiming"},
                {"RepairPatrol", "Repair patrol"}, {"RepairUnit", "Repairing"}, {"RepairUnitNoMove", "Repairing"},
                {"Resurrect", "Resurrecting"}, {"SelfDestruct", "SELF DESTRUCT ENGAGED"}, {"SelfDestructFG", "SELF DESTRUCT ENGAGED"},
                {"SelfRepair", "Repairing"}, {"Standby", "Standby"}, {"Standby_Mine", "Standby"}, {"Standing_FireOrder", "Acknowledged"},
                {"Standing_MoveOrder", "Acknowledged"}, {"Stop", "Stopping"}, {"Suppress", "Suppressing fire"},
                {"Teleport", "Teleporting"}, {"VTOL_Evade", "Evading"}, {"VTOL_Follow", "Guarding"}, {"VTOL_GetRepaired", "Under repair"},
                {"VTOL_HelpBuild", "Nanolathing"}, {"VTOL_LandIfCan", "Seeking to land"}, {"VTOL_Landing", "Landing"},
                {"VTOL_MobileBuild", "Nanolathing"}, {"VTOL_Move", "Moving"}, {"VTOL_Patrol", "Patrolling"}, {"VTOL_Pickup", "Loading"},
                {"VTOL_Reclaim", "Reclaiming"}, {"VTOL_ReclaimUnit", "Reclaiming"}, {"VTOL_RepairPatrol", "Repair patrol"},
                {"VTOL_RepairUnit", "Repairing"}, {"VTOL_SeekAttack", "Seeking to attack"}, {"VTOL_SeekGuard", "Seeking to guard"},
                {"VTOL_Standby", "Standby"}, {"VTOL_Unload", "Unloading"}, {"Wait", "Waiting"}, {"WaitForAttack", "Waiting for attack"},
            };
            const std::vector<std::string> kinds = UnitDefContext::default_mission_names();
            c.mission_status.assign(kinds.size(), std::string());
            for (std::size_t k = 0; k < kinds.size(); ++k)
                for (const auto& [lookup, text] : kStatus)
                    if (kinds[k] == lookup) c.mission_status[k] = text;
        }
        auto& u = c.ui_sounds;
        u.immediate = ui("IMMEDIATEORDERS");
        u.orders_tab = ui("ORDERSBUTTON");
        u.build_tab = ui("BUILDBUTTON");
        u.next_build_menu = ui("NEXTBUILDMENU");
        u.add_build = ui("ADDBUILD");
        u.sub_build = ui("SUBBUILD");
        u.ok_to_build = ui("OKTOBUILD");
        u.not_ok_to_build = ui("NOTOKTOBUILD");
        u.set_fire = ui("SETFIREORDERS");
        u.set_move = ui("SETMOVEORDERS");
        u.special = ui("SPECIALORDERS");
        u.select_squad = ui("SELECTSQUAD");
        u.create_squad = ui("CREATESQUAD");
        u.select_multiple = ui("SelectMultipleUnits");
        u.message = ui("MessageArrived");
        sample_index_["TREEBURN"] = ui("TREEBURN");
    }
    std::vector<const TdfSection*> categories;
    std::vector<std::uint8_t> sound_tdf;
    vfs_.read("gamedata\\SOUND.TDF", sound_tdf);
    const TdfParseResult st = parse(sound_tdf);
    if (st.root)
        for (const auto& s : st.root->children()) categories.push_back(s.get());

    // Unit types: names, notices, selection boxes, build menus.
    std::vector<std::string> commanders;
    for (int k = 0;; ++k) {
        const TdfSection* s = sr.root ? sr.root->child("SIDE" + std::to_string(k)) : nullptr;
        if (!s) break;
        commanders.push_back(s->get_string("commander", 32, ""));
    }
    // Download menus: (unit menu, page, slot, unit).
    struct Entry {
        std::uint16_t menu;
        int page, slot;
        std::uint16_t unit;
    };
    std::vector<Entry> downloads;
    for (const std::string& file : vfs_.enumerate("download\\*.tdf")) {
        std::vector<std::uint8_t> f;
        if (!vfs_.read("download\\" + file, f)) continue;
        const TdfParseResult d = parse(f);
        if (!d.root) continue;
        for (const auto& e : d.root->children()) {
            const std::uint16_t menu = unit_index(e->get_string("UNITMENU", 32, ""));
            const std::uint16_t unit = unit_index(e->get_string("UNITNAME", 32, ""));
            if (menu && unit) downloads.push_back({menu, e->get_int("MENU", 1), e->get_int("BUTTON", 0), unit});
        }
    }
    static const int slot_xy[6][2] = {{0, 27}, {64, 27}, {0, 91}, {64, 91}, {0, 155}, {64, 155}};
    c.types.resize(types_.size());
    for (std::size_t k = 1; k < types_.size() && k < defs_.units.size(); ++k) {
        const UnitDefRecord& d = defs_.units[k];
        client::UnitTypeLook& look = c.types[k];
        look.name = d.name;
        look.pick_min = {d.model_min_x, d.model_min_y, d.model_min_z};
        look.pick_max = {d.model_max_x, d.model_max_y, d.model_max_z};
        look.pick_rank = ((static_cast<std::int64_t>(d.model_size_y) * 0x8000 >> 16) + d.model_size_z) * static_cast<std::int64_t>(d.model_size_x) >> 16;
        look.description = d.description;
        look.unit_name = d.unit_name;
        look.cost_metal = d.build_cost_metal;
        look.cost_energy = d.build_cost_energy;
        for (const std::string& cm : commanders)
            if (stricmp_c(cm, d.unit_name) == 0) look.commander = true;
        look.sounds.fill(-1);
        if (d.sound_category >= 0 && static_cast<std::size_t>(d.sound_category) < categories.size()) {
            const TdfSection* cat = categories[static_cast<std::size_t>(d.sound_category)];
            static const char* const keys[24] = {nullptr,   "select",   "underattack", "activate", "deactivate", "ok",       "arrived",
                                                 "cant",    "unitcomplete", "build",   "repair",   "working",    "load",     "unload",
                                                 "cloak",   "uncloak",  "capture",     "count5",   "count4",     "count3",   "count2",
                                                 "count1",  "count0",   nullptr};
            for (int id = 1; id < 24; ++id) {
                const char* key = id == 23 ? "canceldestruct" : keys[id];
                if (!key) continue;
                // The notice's sounds: <key>, then <key>1, <key>2, ... (one is picked when it speaks).
                auto& list = look.sound_lists[static_cast<std::size_t>(id)];
                bool found = false;
                std::string v = cat->get_string(key, 64, "", &found);
                if (found) list.push_back(sample(v));
                for (int n = 1; n <= 9; ++n) {
                    v = cat->get_string(std::string(key) + std::to_string(n), 64, "", &found);
                    if (!found) break;
                    list.push_back(sample(v));
                }
                if (!list.empty()) look.sounds[static_cast<std::size_t>(id)] = list.front();
            }
        }
        for (const auto& [key, cat] : defs_.categories.all())
            if (k < 512 && (cat.second[k >> 5] & (1u << (k & 31)))) look.categories.push_back(upper(cat.first));
        if (types_[k] && types_[k]->ok) {
            model_box(types_[k]->root.get(), look.box_min, look.box_max);
        }
        // Build menu pages: <unitname>1..N.GUI, then the download entries' pages and slots.
        std::vector<client::Panel>& pages = look.build_pages;
        const std::string prefix = upper(d.unit_name.substr(0, 3));
        for (int p = 1; p <= 9; ++p) {
            client::Panel panel;
            if (!make_panel(d.unit_name + std::to_string(p), prefix, panel)) break;
            pages.push_back(std::move(panel));
        }
        for (const Entry& e : downloads) {
            if (e.menu != k || e.page < 1 || e.slot < 0 || e.slot > 5) continue;
            while (static_cast<int>(pages.size()) < e.page) {
                // A page only downloads fill: the first page's other buttons and empty slots.
                client::Panel np;
                if (!pages.empty()) {
                    np.x = pages.front().x;
                    np.y = pages.front().y;
                    np.background = pages.front().background;
                    for (const client::Button& b : pages.front().buttons)
                        if (b.action != client::Action::BuildUnit) np.buttons.push_back(b);
                } else {
                    np.x = 0;
                    np.y = 128;
                    if (const auto* bg = sequence(common, prefix + "PAN")) np.background = &bg->front();
                }
                pages.push_back(std::move(np));
            }
            client::Panel& pg = pages[static_cast<std::size_t>(e.page - 1)];
            client::Button b;
            b.action = client::Action::BuildUnit;
            b.unit_type = e.unit;
            b.x = pg.x + slot_xy[e.slot][0];
            b.y = pg.y + slot_xy[e.slot][1];
            b.w = b.h = 64;
            b.frames = button_art("", defs_.units[e.unit].unit_name, true);
            // It takes the slot's place (an empty slot's filler goes).
            pg.buttons.erase(std::remove_if(pg.buttons.begin(), pg.buttons.end(),
                                            [&](const client::Button& o) { return o.x == b.x && o.y == b.y && o.w == 64 && o.h == 64; }),
                             pg.buttons.end());
            pg.buttons.push_back(b);
        }
    }

    // Shots: their sounds (by name, into the sample table) and looks.
    c.shots.resize(256);
    for (std::size_t i = 0; i < 256 && i < lb_->weapons.size(); ++i) {
        const WeaponDefRecord& d = weapons_.slots[i];
        sim::WeaponType& t = lb_->weapons[i];
        auto id = [&](const std::string& n) { const int s = sample(n); return s < 0 ? std::uint16_t{0xFFFF} : static_cast<std::uint16_t>(s); };
        t.sound_start = id(d.sound_start);
        t.sound_hit = id(d.sound_hit);
        t.sound_water = id(d.sound_water);
        client::ShotLook& look = c.shots[i];
        look.colour = c.hud[d.color];
        look.colour2 = c.hud[d.color2];
        look.second_line = d.color2 != 0;
        switch (d.render_type) {
        case 0: case 7: look.look = render::ProjectileLook::Line; break;
        case 4: look.look = render::ProjectileLook::Sprite; look.frames = sequence("anims\\FX.GAF", "cannonshell"); break;
        case 5: look.look = render::ProjectileLook::Sprite; look.frames = sequence("anims\\FX.GAF", "flamestream"); break;
        case 1: case 3: case 6: {
            std::string name = d.model_name;
            if (name.empty() && d.model_slot >= 0 && d.model_slot < 256) name = weapons_.slots[static_cast<std::size_t>(d.model_slot)].model_name;
            auto& wm = weapon_models_[upper(name)];
            if (!wm && !name.empty()) {
                wm = std::make_unique<WeaponModel>();
                if (vfs_.read("objects3d\\" + name + ".3DO", wm->file)) {
                    auto m = load_3do(wm->file, true);
                    if (m.root) {
                        wm->root = std::move(m.root);
                        wm->pose = to_pose_model(wm->root.get());
                        wm->model = build_render_model(wm->root.get(), textures_);
                    }
                }
            }
            if (wm && wm->root) {
                look.look = render::ProjectileLook::Model;
                look.model = &wm->model;
                look.pose_model = &wm->pose;
            } else {
                look.look = render::ProjectileLook::Line;
            }
            break;
        }
        default: look.hidden = true; break;
        }
        if (look.look == render::ProjectileLook::Sprite && !look.frames) look.look = render::ProjectileLook::Line;
    }

    // Scene art.
    for (int k = 0; k < 3; ++k) glows_[static_cast<std::size_t>(k)] = render::glow_frames(k, 0xC0FFEEu + static_cast<std::uint32_t>(k));
    {
        std::vector<std::uint8_t> lht;
        if (vfs_.read("palettes\\PALETTE.LHT", lht)) light_table_ = render::light_blend_table(lht);
        // Unit looks: the shade table (lit structures) and the water tint (units seen under water).
        if (!vfs_.read("palettes\\PALETTE.SHD", shade_table_) || shade_table_.size() < 32 * 256) shade_table_.clear();
        water_table_ = render::water_tint_table(palette_);
    }
    {
        std::vector<std::uint8_t> g;
        GafDirectory dir;
        if (vfs_.read("anims\\fog.gaf", g) && gaf_read_directory(g, dir))
            for (int k = 0; k < 4; ++k) {
                gaf_sequence_frames(g, gaf_find_sequence(dir, "Black" + std::to_string(k + 1)), fog_black_[static_cast<std::size_t>(k)]);
                gaf_sequence_frames(g, gaf_find_sequence(dir, "Gray" + std::to_string(k + 1)), fog_gray_[static_cast<std::size_t>(k)]);
                fog_.black[static_cast<std::size_t>(k)] = &fog_black_[static_cast<std::size_t>(k)];
                fog_.gray[static_cast<std::size_t>(k)] = &fog_gray_[static_cast<std::size_t>(k)];
            }
        gray_table_ = render::gray_remap_table(palette_);
        fog_.gray_table = &gray_table_;
    }
    // Features' names and reclaim values (hover info), the victory and defeat signs.
    // The bottom bar shows a feature's description (the original's +0x80 text, also when empty).
    for (const FeatureDefRecord& r : lb_->feature_defs.records()) c.features.push_back({r.description, 0, 0, r.flags});
    if (const auto* q = sequence("anims\\IGTITLES.GAF", "igvictory")) c.victory_sign = &q->front();
    if (const auto* q = sequence("anims\\IGTITLES.GAF", "igdefeat")) c.defeat_sign = &q->front();
    c.fill_scene = [this](render::BattleScene& scene, int view, std::uint32_t frame) { fill_scene(scene, view, frame); };
    c.start_messages.push_back("Skirmish on " + setup_.map);
    return true;
}

} // namespace ta
