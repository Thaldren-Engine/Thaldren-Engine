#include "ta/weapondefs.h"

#include <cstring>

#include "sim/x87.h"
#include "ta/tdf.h"

namespace ta {
namespace {

// Unit-conversion constants exactly as stored in the executable.
double from_bits(std::uint64_t b) {
    double d;
    std::memcpy(&d, &b, 8);
    return d;
}
const double kPerSecondToFixedPerTick = from_bits(0x40A1111111111111ull);    // 65536 / 30
const double kPerSecond2ToFixedPerTick2 = from_bits(0x40523456789ABCDFull);  // 65536 / 900
const double kSecondsToTicks = from_bits(0x403E000000000000ull);             // 30
const double kPerSecondToPerTick = from_bits(0x3FA1111111111111ull);         // 1 / 30
const double kDegreesToRadians = from_bits(0x3F91DF46A2529D34ull);           // close to, but not exactly, pi/180

std::int32_t scaled(const TdfSection& s, const char* key, double k) {
    return sim::x87::ftol32(s.get_float(key, 0.0) * k);
}

void set_bit(std::uint32_t& flags, int bit, std::int32_t v) {
    flags = (flags & ~(1u << bit)) | ((static_cast<std::uint32_t>(v) & 1u) << bit);
}

void load_section(const TdfSection& s, WeaponDefTable& t, const WeaponDefOptions& opt) {
    const std::int32_t id = s.get_int("ID", -1);
    if (id < 0 || id > 255) {
        // The game writes this record outside its 256-entry table (memory corruption).
        t.notes.push_back("weapon section with out-of-range ID " + std::to_string(id) + ": " + s.name());
        return;
    }
    WeaponDefRecord& w = t.slots[static_cast<std::size_t>(id)];
    w.loaded = true;
    w.name = s.name();
    if (w.name.size() > 31) t.notes.push_back("weapon name longer than its 32-byte field: " + w.name);
    w.description = s.get_string("name", 0x40, "");
    w.weapon_velocity = scaled(s, "weaponvelocity", kPerSecondToFixedPerTick);
    w.start_velocity = scaled(s, "startvelocity", kPerSecondToFixedPerTick);
    w.weapon_acceleration = scaled(s, "weaponacceleration", kPerSecond2ToFixedPerTick2);
    w.range = s.get_int("range", 0x7FFF);
    w.coverage = s.get_int("coverage", 0);
    w.reload_time = static_cast<std::int16_t>(scaled(s, "reloadtime", kSecondsToTicks));
    w.energy_per_shot = static_cast<float>(s.get_float("energypershot", 0.0));
    w.metal_per_shot = static_cast<float>(s.get_float("metalpershot", 0.0));
    w.area_of_effect = static_cast<std::int16_t>(s.get_int("areaofeffect", 0));
    w.edge_effectiveness = static_cast<float>(s.get_float("edgeeffectiveness", 0.0));
    w.weapon_timer = static_cast<std::int16_t>(scaled(s, "weapontimer", kSecondsToTicks));
    std::uint32_t& f = w.flags;
    set_bit(f, 27, s.get_int("noautorange", 0));
    w.turn_rate = static_cast<std::int16_t>(scaled(s, "turnrate", kPerSecondToPerTick));
    w.burst = static_cast<std::int16_t>(s.get_int("burst", 0));
    w.burst_rate = static_cast<std::int16_t>(scaled(s, "burstrate", kSecondsToTicks));
    w.spray_angle = static_cast<std::int16_t>(s.get_int("sprayangle", 0));
    w.duration = static_cast<std::int16_t>(scaled(s, "duration", kSecondsToTicks));
    w.random_decay = static_cast<std::int16_t>(scaled(s, "randomdecay", kSecondsToTicks));
    w.smoke_delay = static_cast<std::int16_t>(scaled(s, "smokedelay", kSecondsToTicks));
    w.flight_time = static_cast<std::int16_t>(scaled(s, "flighttime", kSecondsToTicks));
    w.hold_time = static_cast<std::int16_t>(scaled(s, "holdtime", kSecondsToTicks));
    w.min_barrel_angle = static_cast<float>(s.get_float("minbarrelangle", -11.25) * kDegreesToRadians);
    w.fire_starter = static_cast<std::uint8_t>(s.get_int("firestarter", 0));
    w.render_type = static_cast<std::uint8_t>(s.get_int("rendertype", 0));
    w.color = static_cast<std::uint8_t>(s.get_int("color", 0));
    w.color2 = static_cast<std::uint8_t>(s.get_int("color2", 0));
    set_bit(f, 11, s.get_int("soundtrigger", 0));
    set_bit(f, 12, s.get_int("guidance", 0));
    set_bit(f, 13, s.get_int("tracks", 0));
    set_bit(f, 0, s.get_int("lineofsight", 0));
    set_bit(f, 1, s.get_int("ballistic", 0));
    set_bit(f, 14, s.get_int("unitsonly", 0));
    set_bit(f, 15, s.get_int("groundbounce", 0));
    set_bit(f, 16, s.get_int("waterweapon", 0));
    set_bit(f, 17, s.get_int("toairweapon", 0));
    set_bit(f, 18, s.get_int("smoketrail", 0));
    set_bit(f, 19, s.get_int("turret", 0));
    set_bit(f, 20, s.get_int("selfprop", 0));
    set_bit(f, 21, s.get_int("propeller", 0));
    set_bit(f, 22, s.get_int("noexplode", 0));
    set_bit(f, 23, s.get_int("burnblow", 0));
    set_bit(f, 24, s.get_int("twophase", 0));
    set_bit(f, 25, s.get_int("cruise", 0));
    set_bit(f, 26, s.get_int("commandfire", 0));
    set_bit(f, 28, s.get_int("stockpile", 0));
    set_bit(f, 29, s.get_int("targetable", 0));
    set_bit(f, 30, s.get_int("interceptor", 0));
    set_bit(f, 3, s.get_int("beamweapon", 0));
    set_bit(f, 2, s.get_int("shellweapon", 0));
    set_bit(f, 8, s.get_int("dropped", 0));
    set_bit(f, 4, s.get_int("vlaunch", 0));
    set_bit(f, 5, s.get_int("meteor", 0));
    set_bit(f, 6, s.get_int("noradar", 0));
    set_bit(f, 7, s.get_int("paralyzer", 0));
    set_bit(f, 9, s.get_int("startsmoke", 0));
    set_bit(f, 10, s.get_int("endsmoke", 0));
    w.accuracy = static_cast<std::int16_t>(s.get_int("accuracy", 0));
    w.tolerance = static_cast<std::int16_t>(s.get_int("tolerance", 0));
    w.pitch_tolerance = static_cast<std::int16_t>(s.get_int("pitchtolerance", 0));
    w.shake_magnitude = s.get_int("shakemagnitude", 0);
    w.shake_duration = scaled(s, "shakeduration", kSecondsToTicks);

    // Model: reuse the model of an earlier slot whose stored model name matches.
    bool has_model = false;
    const std::string model = s.get_string("model", 0x100, "", &has_model);
    if (!has_model) {
        w.model_slot = -1;  // model name keeps its previous value
    } else {
        w.model_slot = id;
        bool shared = false;
        for (int j = 0; j < id; ++j) {
            if (stricmp_c(model, t.slots[static_cast<std::size_t>(j)].model_name) == 0) {
                w.model_name.clear();
                w.model_slot = t.slots[static_cast<std::size_t>(j)].model_slot;
                shared = true;
                break;
            }
        }
        if (!shared) w.model_name = model.substr(0, 63);
    }

    auto seq = [&](const char* gaf_key, const char* art_key) -> std::string {
        bool g = false, a = false;
        const std::string gaf = s.get_string(gaf_key, 0x100, "", &g);
        const std::string art = s.get_string(art_key, 0x100, "", &a);
        (void)gaf;
        return (g && a) ? art : std::string();
    };
    w.explosion_seq = seq("explosiongaf", "explosionart");
    w.water_explosion_seq = opt.lava_world ? seq("lavaexplosiongaf", "lavaexplosionart")
                                           : seq("waterexplosiongaf", "waterexplosionart");

    auto sound = [&](const char* key) {
        bool found = false;
        const std::string v = s.get_string(key, 0x100, "", &found);
        return found ? v : std::string();
    };
    w.sound_start = sound("soundstart");
    w.sound_hit = sound("soundhit");
    w.sound_water = sound("soundwater");

    if (const TdfSection* dmg = s.child("DAMAGE")) {
        w.default_damage = static_cast<std::int16_t>(dmg->get_int("default", 0));
        for (const auto& kv : dmg->keys()) {
            if (stricmp_c(kv.key, "default") == 0) continue;
            const std::int32_t value = dmg->get_int(kv.key, 0);  // lookup by name (first case-variant wins)
            w.has_damage_map = true;
            auto& m = w.damage_overrides;
            std::size_t lo = 0, hi = m.size();
            while (lo < hi) {
                const std::size_t mid = lo + (hi - lo) / 2;
                if (stricmp_c(m[mid].first, kv.key) < 0) lo = mid + 1;
                else hi = mid;
            }
            if (lo != m.size() && m[lo].first == kv.key) m[lo].second = value;
            else m.insert(m.begin() + static_cast<std::ptrdiff_t>(lo), {kv.key, value});
        }
    } else {
        w.default_damage = 0;
    }

    if (f & (1u << 19)) w.fire = WeaponFireFn::Turret;
    else if (f & (1u << 4)) w.fire = WeaponFireFn::VLaunch;
    else if (f & ((1u << 0) | (1u << 20))) w.fire = WeaponFireFn::LineOfSight;
    else if (f & (1u << 8)) w.fire = WeaponFireFn::Dropped;
}

} // namespace

WeaponDefTable load_weapon_defs(Vfs& vfs, const WeaponDefOptions& opt) {
    WeaponDefTable t;
    for (int i = 0; i < 256; ++i) t.slots[static_cast<std::size_t>(i)].index = static_cast<std::uint8_t>(i);
    for (const auto& file : vfs.enumerate("Weapons\\*.tdf")) {
        std::string path = "Weapons\\" + file;
        const auto dot = path.rfind('.');
        if (dot != std::string::npos) path.resize(dot);
        path += ".TDF";
        std::vector<std::uint8_t> data;
        if (!vfs.read(path, data) || data.empty()) continue;
        if (vfs.is_loose(path) && opt.cd_check_enabled) continue;
        auto r = tdf_parse(std::string_view(reinterpret_cast<const char*>(data.data()), data.size()));
        if (!r.root) continue;
        for (const auto& sec : r.root->children()) load_section(*sec, t, opt);
    }
    return t;
}

} // namespace ta
