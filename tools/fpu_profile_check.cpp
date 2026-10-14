// Compares an fpu_capture file (FSINCOS.CAP from any machine) with the
// engine's built-in FPU profiles, value by value, and measures how many
// rotations of typical model coordinates would come out different.
//
// Usage: fpu_profile_check <FSINCOS.CAP>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <random>
#include <string>
#include <vector>

#include "sim/x87_trig.h"

namespace {

using sim::x87::Ext80;
using sim::x87::FpuProfile;

bool same(const Ext80& a, const Ext80& b) {
    return a.mantissa == b.mantissa && (a.mantissa == 0 || (a.exponent == b.exponent && a.negative == b.negative));
}

struct Table {
    std::vector<Ext80> s, c;
};

Table snapshot(FpuProfile p) {
    sim::x87::set_fpu_profile(p);
    Table t;
    t.s.resize(65536);
    t.c.resize(65536);
    for (int k = -32768; k <= 32767; ++k) {
        if (k == 0) continue;
        sim::x87::sincos_for_angle(static_cast<std::int16_t>(k), t.s[static_cast<std::uint16_t>(k)], t.c[static_cast<std::uint16_t>(k)]);
    }
    return t;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: fpu_profile_check <FSINCOS.CAP>\n");
        return 2;
    }
    std::ifstream f(argv[1], std::ios::binary);
    std::vector<std::uint8_t> data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    std::string cpu;
    if (!sim::x87::load_fsincos_capture(data.data(), data.size(), &cpu)) {
        std::fprintf(stderr, "not a valid capture file\n");
        return 1;
    }
    std::printf("capture: %s\n", cpu.c_str());
    const Table cap = snapshot(FpuProfile::Captured);
    const struct { FpuProfile p; const char* name; } profiles[] = {
        {FpuProfile::IntelP6, "IntelP6 (built-in)"}, {FpuProfile::Model66Pi, "Model66Pi"}};
    std::mt19937 g(99);
    int result = 0;
    for (const auto& pr : profiles) {
        const Table t = snapshot(pr.p);
        int values = 0;
        for (int k = 0; k < 65536; ++k) {
            if (k == 0) continue;
            values += !same(cap.s[static_cast<std::size_t>(k)], t.s[static_cast<std::size_t>(k)]);
            values += !same(cap.c[static_cast<std::size_t>(k)], t.c[static_cast<std::size_t>(k)]);
        }
        // Effect on rotations: 8 model-scale points and 8 full-range points per angle.
        long long rot_diff = 0, rot_total = 0, wide_diff = 0;
        for (int k = -32768; k <= 32767; ++k) {
            if (k == 0) continue;
            for (int rep = 0; rep < 16; ++rep) {
                const bool wide = rep >= 8;
                const std::int32_t x0 = wide ? static_cast<std::int32_t>(g()) : static_cast<std::int32_t>(g() % 0x400000) - 0x200000;
                const std::int32_t y0 = wide ? static_cast<std::int32_t>(g()) : static_cast<std::int32_t>(g() % 0x400000) - 0x200000;
                std::int32_t xa = x0, ya = y0, xb = x0, yb = y0;
                sim::x87::set_fpu_profile(FpuProfile::Captured);
                sim::x87::rotate_point_2d(static_cast<std::int16_t>(k), xa, ya);
                sim::x87::set_fpu_profile(pr.p);
                sim::x87::rotate_point_2d(static_cast<std::int16_t>(k), xb, yb);
                ++rot_total;
                if (xa != xb || ya != yb) ++(wide ? wide_diff : rot_diff);
            }
        }
        std::printf("vs %-20s %6d of 131070 sin/cos values differ; rotations differing: %lld of %lld model-scale, "
                    "%lld of %lld full-range\n",
                    pr.name, values, rot_diff, rot_total / 2, wide_diff, rot_total / 2);
        if (pr.p == FpuProfile::IntelP6 && values != 0) result = 1;
    }
    // What emulators running the original game would compute instead:
    //  - 86Box / Bochs / MAME (SoftFloat): reduction with a 128-bit pi, then near-correct rounding;
    //  - DOSBox-X normal core: host double-precision sin()/cos() (53-bit values).
    struct Emu {
        const char* name;
        int kind;
    };
    for (const Emu emu : {Emu{"SoftFloat-style (true pi)", 0}, Emu{"host double sin/cos", 1}}) {
        sim::x87::set_fpu_profile(FpuProfile::Captured);
        long long diff_model = 0, diff_wide = 0, total = 0;
        std::mt19937 g2(7);
        for (int k = -32768; k <= 32767; ++k) {
            if (k == 0) continue;
            const double a = static_cast<double>(k) * sim::x87::angle_step();
            Ext80 s, c;
            if (emu.kind == 0) {
                sim::x87::sincos_correctly_rounded(a, s, c);
            } else {
                auto from = [](double d) {
                    Ext80 e;
                    if (d == 0) return e;
                    int ex = 0;
                    const double fr = std::frexp(std::fabs(d), &ex);
                    e.mantissa = static_cast<std::uint64_t>(std::ldexp(fr, 53)) << 11;
                    e.exponent = ex - 64;
                    e.negative = d < 0;
                    return e;
                };
                s = from(std::sin(a));
                c = from(std::cos(a));
            }
            for (int rep = 0; rep < 16; ++rep) {
                const bool wide = rep >= 8;
                const std::int32_t x0 = wide ? static_cast<std::int32_t>(g2()) : static_cast<std::int32_t>(g2() % 0x400000) - 0x200000;
                const std::int32_t y0 = wide ? static_cast<std::int32_t>(g2()) : static_cast<std::int32_t>(g2() % 0x400000) - 0x200000;
                std::int32_t xa = x0, ya = y0;
                sim::x87::rotate_point_2d(static_cast<std::int16_t>(k), xa, ya);
                const std::int32_t xb = sim::x87::fist_nearest(sim::x87::mul_pc53(x0, c) - sim::x87::mul_pc53(y0, s));
                const std::int32_t yb = sim::x87::fist_nearest(sim::x87::mul_pc53(y0, c) + sim::x87::mul_pc53(x0, s));
                ++total;
                if (xa != xb || ya != yb) ++(wide ? diff_wide : diff_model);
            }
        }
        std::printf("emulated with %-26s rotations differing from the capture: %lld of %lld model-scale, %lld of %lld full-range\n",
                    emu.name, diff_model, total / 2, diff_wide, total / 2);
    }
    return result;
}
