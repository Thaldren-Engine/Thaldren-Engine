// Tests for the TA compatibility layer (no game data required).

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <array>
#include <filesystem>
#include <fstream>
#include <algorithm>
#include <cctype>
#include <cstring>
#include <map>

#include "client/mouse_trails.h"
#include "client/sound_rules.h"
#include "ta/battle_loader.h"
#include "ta/crt_strtod.h"
#include "ta/hpi.h"
#include "ta/inflate.h"
#include "ta/notice_texts.h"
#include "ta/online_services.h"
#include "ta/palette_tables.h"
#include "ta/preferences.h"
#include "ta/restrict_list.h"
#include "ta/tdf.h"
#include "ta/translate.h"
#include "ta/unitdefs.h"

namespace {

int g_failures = 0;
int g_checks = 0;

#define CHECK(cond)                                                              \
    do {                                                                         \
        ++g_checks;                                                              \
        if (!(cond)) {                                                           \
            ++g_failures;                                                        \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);          \
        }                                                                        \
    } while (0)

void test_comments() {
    std::string t = "a=1; // x=2;\nb=3; /* c=4; */ d=5;";
    ta::tdf_blank_comments(t);
    CHECK(t == "a=1;        \nb=3;            d=5;");
    std::string u = "/*/ e=1; */f=2;";  // "/*/" does not close the comment
    ta::tdf_blank_comments(u);
    CHECK(u == "           f=2;");
}

void test_parse_basic() {
    auto r = ta::tdf_parse(
        "[UNITINFO]\r\n{\r\n  UnitName = ARMCOM ;\r\n  MaxVelocity=1.25;\r\n  Name=First;\r\n  Name=Second;\r\n"
        "  name=Lower;\r\n  [Sub]{x=1;}\r\n}\r\n");
    CHECK(r.error.empty());
    const ta::TdfSection* u = r.root->child("unitinfo");
    CHECK(u != nullptr);
    if (!u) return;
    CHECK(u->get_string("unitname", 64, "") == "ARMCOM");
    CHECK(u->get_fixed("maxvelocity", 0) == 0x14000);
    CHECK(u->get_int("missing", 7) == 7);
    // Exact duplicate replaces; a case-variant is a separate key inserted at the
    // case-insensitive lower bound, i.e. BEFORE the existing one, so lookups find it.
    CHECK(u->keys().size() == 4);
    CHECK(*u->raw("NAME") == "Lower");
    CHECK(u->child("SUB") != nullptr && u->child("SUB")->get_int("X", 0) == 1);
}

void test_parse_errors() {
    CHECK(!ta::tdf_parse("[A]{ x=1 }").error.empty());   // missing ';'
    CHECK(!ta::tdf_parse("[A] x=1; }").error.empty());   // missing '{'
    CHECK(!ta::tdf_parse("[A]{ x=1;").error.empty());    // unterminated section
    CHECK(ta::tdf_parse("").error.empty());
}

void test_atoi_atof() {
    CHECK(ta::msvc_atoi("  -42abc") == -42);
    CHECK(ta::msvc_atoi("+7") == 7);
    CHECK(ta::msvc_atoi("4294967297") == 1);  // wraps
    CHECK(ta::msvc_atoi("-") == 0);
    CHECK(ta::msvc_atof(" 1.5x") == 1.5);
    CHECK(ta::msvc_atof("2d2") == 200.0);
    CHECK(ta::msvc_atof(".25") == 0.25);
    CHECK(ta::msvc_atof("-.") == 0.0);
    CHECK(ta::msvc_atof("3e") == 3.0);
}

void test_stricmp() {
    CHECK(ta::stricmp_c("abc", "ABC") == 0);
    CHECK(ta::stricmp_c("a_", "aZ") < 0);  // '_'(0x5F) < 'z'(0x7A) after folding
    CHECK(ta::stricmp_c("ab", "abc") < 0);
}

void test_checksum() {
    const std::uint8_t d[] = {1, 2, 3};
    // sum=6, xor=0, sum(i^b)=1+3+1=5, xor(i+b)=1^3^5=7
    CHECK(ta::simple_checksum(d, 3) == (6u | (0u << 8) | (5u << 16) | (7u << 24)));
    CHECK(ta::simple_checksum(d, -1) == 0u);
}

void test_lz77() {
    // flags 0b110: literal 'A', match offset 1 count 3 (reads the bytes it just wrote), end marker.
    const std::uint8_t in[] = {0x06, 'A', 0x11, 0x00, 0x00, 0x00};
    std::vector<std::uint8_t> out;
    ta::decompress_lz77(in, sizeof(in), out);
    CHECK((out == std::vector<std::uint8_t>{'A', 'A', 'A', 'A'}));
}

void test_inflate() {
    // zlib stream of "hello hello hello" (stored block).
    const std::uint8_t z[] = {0x78, 0x01, 0x01, 0x11, 0x00, 0xEE, 0xFF, 'h', 'e', 'l', 'l', 'o', ' ', 'h', 'e', 'l', 'l',
                              'o', ' ', 'h', 'e', 'l', 'l', 'o', 0x3A, 0x9B, 0x06, 0x56};
    std::vector<std::uint8_t> out;
    const auto r = ta::zlib_decompress(z, out);
    CHECK(r == ta::InflateResult::Ok || r == ta::InflateResult::BadChecksum);
    CHECK(std::string(out.begin(), out.end()) == "hello hello hello");
}

void test_crt_pow10_table() {
    // Fixture: the original runtime's two 29-entry tables (positive, negative powers).
    std::ifstream f(FIXTURES_DIR "/crt_pow10_0x5116F0.bin", std::ios::binary);
    std::vector<unsigned char> b((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    CHECK(b.size() == 696);
    if (b.size() != 696) return;
    int k = 0;
    for (int sign : {1, -1}) {
        for (int g = 0; g < 5; ++g) {
            for (int d = 1; d < (g < 4 ? 8 : 2); ++d, ++k) {
                int p = d;
                for (int i = 0; i < g; ++i) p *= 8;
                const ta::Ld12 e = ta::crt_pow10_entry(sign * p);
                CHECK(std::memcmp(e.data(), b.data() + 12 * k, 12) == 0);
            }
        }
    }
}

void test_crt_atof_quirks() {
    auto bits = [](double d) {
        std::uint64_t u;
        std::memcpy(&u, &d, 8);
        return u;
    };
    CHECK(ta::crt_atof("Infantry Kbot") == 0.0);   // no "inf" parsing
    CHECK(ta::crt_atof("1d10") == 1e10);           // 'd' exponent
    CHECK(bits(ta::crt_atof("-.0")) == 0x8000000000000000ull);
    CHECK(bits(ta::crt_atof("-x")) == 0);          // no digits: +0
    CHECK(ta::crt_atof("0.1") == 0.1);
    CHECK(ta::crt_atof("  12.5e1") == 125.0);
    // sscanf "%f" into a float (_atoflt 0x4F3AD0) rounds the 80-bit intermediate straight to
    // 24 bits; these differ from float(atof(s)). Values from the original (orig_harness).
    auto fbits = [](float f) {
        std::uint32_t b;
        std::memcpy(&b, &f, 4);
        return b;
    };
    CHECK(fbits(ta::crt_scanf_float("700.8E8")) == 0x518288BFu);
    CHECK(fbits(static_cast<float>(ta::crt_atof("700.8E8"))) == 0x518288C0u);
    CHECK(fbits(ta::crt_scanf_float("-203784012")) == 0xCD4257F4u);
    CHECK(fbits(ta::crt_scanf_float("+00061107158")) == 0x4C691AF5u);
    CHECK(fbits(ta::crt_scanf_float("\t0752353392")) == 0x4E336001u);
    CHECK(fbits(ta::crt_scanf_float("-")) == 0x80000000u);  // no digits: the sign is kept
    CHECK(bits(ta::crt_atof("-")) == 0);                       // (atof returns +0 then)
    CHECK(fbits(ta::crt_scanf_float("0.1")) == 0x3DCCCCCDu);
}

// The restrictions' list files (0x44B230 / 0x44B140): the table's size less one, then the listed
// definitions' key and count in index order; reading picks the first definition with the key.
void test_restrict_list() {
    // Definitions 0 (blank) .. 4; 3 is "norestrict" (not listed); 4 shares 2's key.
    const std::vector<std::uint32_t> keys = {0, 0x11111111, 0x22222222, 0x33333333, 0x22222222};
    std::vector<ta::RestrictListRow> rows = {{2, -1}, {4, 7}, {1, 0}};  // (the list's sorted order)
    std::vector<std::uint8_t> bytes;
    ta::write_restrict_list(keys, rows, bytes);
    const std::vector<std::uint8_t> expect = {4, 0, 0, 0,
                                              0x11, 0x11, 0x11, 0x11, 0, 0, 0, 0,
                                              0x22, 0x22, 0x22, 0x22, 0xFF, 0xFF, 0xFF, 0xFF,
                                              0x22, 0x22, 0x22, 0x22, 7, 0, 0, 0};
    CHECK(bytes == expect);
    // Read back into fresh counts: definition 4's pair goes to definition 2 (its key's first),
    // and the header's fourth pair (past the end) repeats the last one.
    std::vector<ta::RestrictListRow> back = {{2, 100}, {4, 100}, {1, 100}};
    CHECK(ta::read_restrict_list(bytes, keys, back) == 3);
    CHECK(back[0].count == 7 && back[1].count == 100 && back[2].count == 0);
    // Round trip with distinct keys.
    const std::vector<std::uint32_t> k2 = {0, 5, 6, 7};
    std::vector<ta::RestrictListRow> r2 = {{3, 55}, {1, -1}, {2, 0}};
    ta::write_restrict_list(k2, r2, bytes);
    std::vector<ta::RestrictListRow> b2 = {{3, 100}, {1, 100}, {2, 100}};
    ta::read_restrict_list(bytes, k2, b2);
    CHECK(b2[0].count == 55 && b2[1].count == -1 && b2[2].count == 0);
    // A short last word: the bytes there replace the low bytes of the last count.
    std::vector<std::uint8_t> cut = {1, 0, 0, 0, 5, 0, 0, 0, 9};
    std::vector<ta::RestrictListRow> b3 = {{1, 0x01020304}};
    CHECK(ta::read_restrict_list(cut, k2, b3) == 0);
    CHECK(b3[0].count == 9);  // (count word started at 0, see read_restrict_list)
    // Unknown keys change nothing; an empty file nothing.
    std::vector<std::uint8_t> other = {1, 0, 0, 0, 0x99, 0, 0, 0, 3, 0, 0, 0};
    std::vector<ta::RestrictListRow> b4 = {{1, 100}};
    ta::read_restrict_list(other, k2, b4);
    CHECK(b4[0].count == 100);
    CHECK(ta::read_restrict_list({}, k2, b4) == 0 && b4[0].count == 100);
    CHECK(ta::name_without_extension("Arm only.LST") == "Arm only");
}

// The translation table (0x4C54F0 / 0x4C5740): section name = base text (case-sensitive), the
// language's field (any case) = its translation; empty ones skipped, later sections replace.
void test_translations() {
    const std::string tdf =
        "/* comment */\n[KILLS]\n{\nGerman = ABSCHUSSE;\nFrench = VICTIMES;\n}\n"
        "[Kills]\n{\ngerman = Abschusse;\n}\n[Empty]\n{\nGerman = ;\n}\n"
        "[Twice]\n{\nGerman = one;\n}\n[Twice]\n{\nGerman = two;\n}\n";
    ta::load_translations(tdf, "German");
    CHECK(ta::translate("KILLS") == "ABSCHUSSE");
    CHECK(ta::translate("Kills") == "Abschusse");
    CHECK(ta::translate("kills") == "kills");
    CHECK(ta::translate("Empty") == "Empty");
    CHECK(ta::translate("Twice") == "two");
    CHECK(ta::translate("Other") == "Other");
    ta::load_translations(std::string(), "GERMAN");  // the language already loaded: kept
    CHECK(ta::translate("KILLS") == "ABSCHUSSE");
    ta::load_translations(tdf, "french");
    CHECK(ta::translate("KILLS") == "VICTIMES");
    CHECK(ta::translate("Kills") == "Kills");
    // Lowered names (0x434BF0): the lowered name's translation, else the name as given.
    ta::load_translations(
        "[canal crossing]\n{\nGerman = Kanal\xFC" "berquerung;\n}\n[b]\n{\nGerman = Same;\n}\n[a]\n{\nGerman = SAME;\n}\n"
        "[zz]\n{\nGerman = zZ;\n}\n",
        "german");
    CHECK(ta::lower_ascii("Canal CROSSING \xDC") == "canal crossing \xDC");
    CHECK(ta::translate_lowered("Canal Crossing") == "Kanal\xFC" "berquerung");
    CHECK(ta::translate_lowered("Lava Run") == "Lava Run");
    CHECK(ta::translate_lowered("ZZ") == "ZZ");  // the translation equals the lowered name in any case
    // The reverse lookup (0x4C5840): any case, the first base text in byte order.
    CHECK(ta::untranslate("KANAL\xFC" "BERQUERUNG") == "canal crossing");
    CHECK(ta::untranslate("same") == "a");
    CHECK(ta::untranslate("Lava Run").empty());
    ta::load_translations(tdf, "english");  // no english fields: every text as it is
    CHECK(ta::translate("KILLS") == "KILLS");
    CHECK(ta::untranslate("KILLS").empty());
}

// The skirmish schema pick (0x436860, map info kind 2) counts the set-up table's highest filled
// row + 1, not GS+0x2A3C (computers + 1): rows 0 (player) and 3 (computer) pick the 4-position
// schema, where computers + 1 would pick the 2-position one.
void test_skirmish_schema_rows() {
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "compat_tests_schema_rows";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir / "maps", ec);
    {
        std::ofstream f(dir / "maps" / "Rows.ota", std::ios::binary);
        auto schema = [&](int n, const char* type, int positions) {
            f << "[Schema " << n << "]\r\n{\r\nType=" << type << ";\r\n[specials]\r\n{\r\n";
            for (int k = 0; k < positions; ++k)
                f << "[special" << k << "]\r\n{\r\nspecialwhat=StartPos" << (k + 1) << ";\r\nXPos=" << (100 + 200 * k)
                  << ";\r\nZPos=" << (300 + 10 * k) << ";\r\n}\r\n";
            f << "}\r\n}\r\n";
        };
        f << "[GlobalHeader]\r\n{\r\n";
        schema(0, "Network 1", 2);
        schema(1, "Network 2", 4);
        schema(2, "Network 3", 3);
        f << "}\r\n";
    }
    ta::Vfs vfs;
    vfs.set_cd_drives({});
    vfs.mount_game_dir(dir);
    // The table: row 0 a player, row 3 a computer (rows 1, 2 open).
    std::array<int, 10> kinds{};
    kinds[0] = 1;
    kinds[3] = 2;
    const int computers = 1;
    CHECK(ta::setup_table_players(kinds, computers + 1) == 4);
    int schema = -2;
    CHECK(ta::map_start_position_count(vfs, "Rows", ta::setup_table_players(kinds, computers + 1), &schema) == 4);
    CHECK(schema == 1);
    // GS+0x2A3C (computers + 1 = 2) would have picked the 2-position schema.
    CHECK(ta::map_start_position_count(vfs, "Rows", computers + 1, &schema) == 2);
    CHECK(schema == 0);
    // Contiguous rows 0..2: 3 players, the 3-position schema.
    const std::array<int, 10> three{1, 2, 2};
    CHECK(ta::setup_table_players(three, 9) == 3);
    CHECK(ta::map_start_position_count(vfs, "Rows", 3, &schema) == 3);
    CHECK(schema == 2);
    // A row past the shown count (kept from a larger count) still counts: rows 0, 1 and 7.
    std::array<int, 10> stale{1, 2};
    stale[7] = 2;
    CHECK(ta::setup_table_players(stale, 2) == 8);
    // No schema has 8 positions: the largest count seen (4) is kept.
    CHECK(ta::map_start_position_count(vfs, "Rows", 8, &schema) == 4);
    CHECK(schema == 1);
    // No row filled: GS+0x2A3C.
    CHECK(ta::setup_table_players(std::array<int, 10>{}, 5) == 5);
    // No map info: -1 ("The terrain for the selected map does not exist.").
    CHECK(ta::map_start_position_count(vfs, "Missing", 4, &schema) == -1);
    CHECK(schema == -1);
    fs::remove_all(dir, ec);
}

// The model box from the footprint (0x42D079): shifts, halving and sums wrap at 32 bits.
void test_footprint_model_box() {
    ta::UnitDefRecord u;
    u.footprint_x = 3;
    u.footprint_z = 2;
    u.model_max_y = 0x50000;
    u.model_min_y = 0;
    ta::footprint_model_box(u);
    CHECK(u.model_min_x == -0x180000 && u.model_max_x == 0x180000);
    CHECK(u.model_min_z == -0x100000 && u.model_max_z == 0x100000);
    CHECK(u.model_size_x == 0x300000 && u.model_size_y == 0x50000 && u.model_size_z == 0x200000);
    CHECK(u.model_avg_size == 0x500000 / 3);
    // A footprint of 2048 shifts into the sign bit: -(2048 << 20) wraps to INT32_MIN, halved
    // toward zero; the size then wraps too (the original's shl / sub / add).
    u.footprint_x = 2048;
    u.footprint_z = 4095;
    ta::footprint_model_box(u);
    CHECK(u.model_min_x == -0x40000000 &&u.model_max_x == -0x40000000);
    CHECK(u.model_size_x == 0);
    CHECK(u.model_max_z == static_cast<std::int32_t>(0xFFF00000u) / 2 && u.model_min_z == static_cast<std::int32_t>(0x00100000u) / 2);
    CHECK(u.model_size_z == static_cast<std::int32_t>(static_cast<std::uint32_t>(-0x80000) - 0x80000u));
    CHECK(u.model_avg_size == u.model_size_z / 3);
}

// Unit notices carry the behaviour's own text (0x47F780: the caller's text replaces the
// notice table's; "<type>: <text>" on the console when priority > 10 - unitchattext).
void test_notice_texts() {
    using T = sim::SpeechText;
    CHECK(ta::speech_text(T::None) == nullptr);
    CHECK(std::string(ta::speech_text(T::UnableToCreate)) == "Unable to create any more units");
    CHECK(std::string(ta::speech_text(T::CantReachSite)) == "I can't reach the construction site");
    CHECK(std::string(ta::speech_text(T::CloudOfVapor)) == "That unit is a cloud of vapor and cannot be captured");
    CHECK(std::string(ta::speech_text(T::ResurrectionFailed)) == "Resurrection failed");    // 0x404DEF
    CHECK(std::string(ta::speech_text(T::RessurectionFailed)) == "Ressurection failed");    // 0x404F6F (sic)
    CHECK(std::string(ta::speech_text(T::RepairAborted)) == "Repair aborted.");
    CHECK(std::string(ta::speech_text(T::Guarding)) == "Guarding");
    for (int k = 1; k <= static_cast<int>(T::ResurrectionComplete); ++k) CHECK(ta::speech_text(static_cast<T>(k)) != nullptr);

    struct Host : client::NoticeQueue::Host {
        std::vector<std::string> shown;
        std::uint32_t tick() override { return 100; }
        std::uint32_t rand() override { return 0; }
        int sound_count(std::uint16_t, std::uint32_t) override { return 0; }
        void speak(std::uint16_t, std::uint32_t, int) override {}
        bool unit_alive(std::uint16_t) override { return true; }
        void show(std::uint16_t, const std::string& text) override { shown.push_back(text); }
    };
    Host h;
    client::NoticeQueue::Levels lv;  // unitchat 10, unitchattext 5
    client::NoticeQueue q;
    q.queue(h, lv, 3, 7, ta::speech_text(T::UnableToCreate));  // "cant" (priority 8)
    CHECK(q.size() == 1 && q.text_at(0) == "Unable to create any more units");
    q.frame(h, lv);
    CHECK(h.shown.size() == 1 && h.shown[0] == "Unable to create any more units");
    q.queue(h, lv, 3, 7);  // no text of its own: the table's
    CHECK(q.size() == 1 && q.text_at(0) == "Cannot Comply");
    q.frame(h, lv);
    // "ok" (priority 5) with the order's acknowledgement text: shown only above level 5.
    q.queue(h, lv, 3, 5, ta::speech_text(T::Guarding));
    CHECK(q.size() == 1 && q.text_at(0) == "Guarding");
    q.frame(h, lv);
    CHECK(h.shown.size() == 2 && h.shown[1] == "Cannot Comply");
    lv.unit_chat_text = 6;
    q.queue(h, lv, 3, 5, ta::speech_text(T::Guarding));
    q.frame(h, lv);
    CHECK(h.shown.size() == 3 && h.shown[2] == "Guarding");
}

// totala.ini [Preferences] as GetPrivateProfileIntA reads it (values measured with the system
// call on Windows 11) and the start-up unit limit (0x491200).
void test_preferences() {
    auto val = [](const std::string& v) { return ta::profile_int("[Preferences]\r\nUnitLimit=" + v + "\r\n", "Preferences", "UnitLimit", 250); };
    CHECK(val("300") == 300);
    CHECK(val(" 300") == 300);
    CHECK(val("300abc") == 300);
    CHECK(val("-5") == -5);
    CHECK(val("0x20") == 32);
    CHECK(val("0xFF") == 255);
    CHECK(val("0X1F") == 0);
    CHECK(val("") == 250);
    CHECK(val("abc") == 0);
    CHECK(val("+7") == 7);
    CHECK(val("4000000000") == -294967296);
    CHECK(val("\"77\"") == 77);
    CHECK(val("\"7") == 0);
    CHECK(val("7\"") == 7);
    CHECK(val("\" 8 \"") == 8);
    CHECK(val("'9'") == 9);
    CHECK(val("12 34") == 12);
    CHECK(val("\t45") == 45);
    CHECK(val("1e3") == 1);
    CHECK(val("-2147483649") == 2147483647);
    CHECK(val("99999999999999") == 276447231);
    CHECK(val("-0x10") == -16);
    CHECK(val("0x100000001") == 1);
    CHECK(val("00x5") == 0);
    CHECK(val("- 5") == 0);
    CHECK(val("0x") == 0);
    auto txt = [](const char* t) { return ta::profile_int(t, "Preferences", "UnitLimit", 250); };
    CHECK(txt("[preferences]\r\nunitlimit=111\r\n") == 111);
    CHECK(txt("[ Preferences ]\r\nUnitLimit=112\r\n") == 112);
    CHECK(txt("[Preferences]\r\n  UnitLimit  =  113  \r\n") == 113);
    CHECK(txt("[Preferences]\r\nUnitLimit=114\r\nUnitLimit=115\r\n") == 114);
    CHECK(txt("[Preferences]\r\n[Preferences]\r\nUnitLimit=116\r\n") == 250);
    CHECK(txt("[Other]\r\nUnitLimit=1\r\n[Preferences]\r\nUnitLimit=117\r\n[Preferences]\r\nUnitLimit=118\r\n") == 117);
    CHECK(txt("; c\r\n[Preferences];x\r\nUnitLimit=119;comment\r\n") == 119);
    CHECK(txt("[Preferences]\nUnitLimit=120\n") == 120);
    CHECK(txt("UnitLimit=121\r\n[Preferences]\r\n") == 250);
    CHECK(txt("[Preferences]\r\n;UnitLimit=122\r\n") == 250);
    CHECK(txt("[Preferences]\r\nUnitLimit\r\n") == 250);
    CHECK(txt("[Preferences]\r\nUnitLimit=\r\n") == 250);

    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path dir = fs::temp_directory_path(ec) / "compat_tests_prefs";
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    CHECK(ta::preferences_unit_limit(dir.string()) == 250);  // no file
    auto with = [&](const char* body) {
        std::ofstream(dir / "totala.ini", std::ios::binary) << body;
        return ta::preferences_unit_limit(dir.string());
    };
    CHECK(with("[Preferences]\r\nUnitLimit=400\r\n") == 400);
    CHECK(with("[Preferences]\r\nUnitLimit=900\r\n") == 500);
    CHECK(with("[Preferences]\r\nUnitLimit=5\r\n") == 20);
    CHECK(with("[Preferences]\r\nUnitLimit=-5\r\n") == 20);
    CHECK(with("[Preferences]\r\nUnitLimit=4000000000\r\n") == 20);  // a negative int
    CHECK(with("[Preferences]\r\nNoDirectSound=1\r\n") == 250);
    CHECK(ta::preferences_int(dir.string(), "NoDirectSound", 0) == 1);
    fs::remove_all(dir, ec);
}

// A minimal 32-bit DLL with an export table (module name, exported names) for the online
// services' recognition.
std::vector<std::uint8_t> fake_dll(const std::string& module, const std::vector<std::string>& names, bool dll = true, std::uint16_t machine = 0x14C) {
    std::vector<std::uint8_t> f(0x400, 0);
    auto p16 = [&](std::size_t o, std::uint32_t v) { f[o] = static_cast<std::uint8_t>(v); f[o + 1] = static_cast<std::uint8_t>(v >> 8); };
    auto p32 = [&](std::size_t o, std::uint32_t v) { p16(o, v & 0xFFFF); p16(o + 2, v >> 16); };
    f[0] = 'M';
    f[1] = 'Z';
    p32(0x3C, 0x80);
    p32(0x80, 0x4550);
    p16(0x84, machine);
    p16(0x86, 1);                      // one section
    p16(0x94, 0xE0);                   // optional header size
    p16(0x96, dll ? 0x2102 : 0x0102);  // characteristics
    const std::size_t opt = 0x98;
    p16(opt, 0x10B);
    p32(opt + 96, 0x1000);             // export directory RVA
    const std::size_t sec = opt + 0xE0;
    std::memcpy(&f[sec], ".rdata", 6);
    p32(sec + 8, 0x200);                // virtual size
    p32(sec + 12, 0x1000);              // RVA
    p32(sec + 16, 0x200);               // raw size
    p32(sec + 20, 0x200);               // raw offset
    // Export directory at RVA 0x1000 (file 0x200): name RVA, count, name table RVA.
    const std::size_t e = 0x200;
    std::size_t str = 0x300;  // strings from RVA 0x1100
    auto put = [&](const std::string& s) {
        const std::uint32_t rva = static_cast<std::uint32_t>(0x1000 + (str - 0x200));
        std::memcpy(&f[str], s.data(), s.size());
        str += s.size() + 1;
        return rva;
    };
    p32(e + 12, put(module));
    p32(e + 24, static_cast<std::uint32_t>(names.size()));
    p32(e + 32, 0x1040);  // name pointer table at file 0x240
    for (std::size_t i = 0; i < names.size(); ++i) p32(0x240 + 4 * i, put(names[i]));
    return f;
}

// The connection screen's online links (online.dll 3 and its extension DLLs, reproduced as
// rules; docs/RE_MEMORY.md "Online services") over a made-up machine.
void test_online_services() {
    struct Machine {
        std::map<std::string, std::vector<std::uint8_t>> files;  // upper-case path -> bytes
        std::vector<ta::OnlineSystem::Found> listing;
        std::map<std::string, std::pair<std::uint32_t, std::vector<std::uint8_t>>> values;  // "HKLM|key|value"
        std::vector<std::string> keys;                                                     // "HKLM|key"
        std::vector<std::pair<std::string, unsigned>> drives = {{"C:\\", 3}};
        bool drive_list = true;
    };
    auto up = [](std::string s) {
        for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        return s;
    };
    auto sys_of = [&](Machine& m) {
        ta::OnlineSystem s;
        s.find_files = [&m](const std::string&, const std::string& pattern) {
            return pattern == "ta*.dll" ? m.listing : std::vector<ta::OnlineSystem::Found>{};
        };
        s.exists = [&m, up](const std::string& p) { return m.files.count(up(p)) != 0; };
        s.read_file = [&m, up](const std::string& p) {
            auto it = m.files.find(up(p));
            return it == m.files.end() ? std::vector<std::uint8_t>{} : it->second;
        };
        s.key_opens = [&m](bool user, const std::string& key) {
            return std::find(m.keys.begin(), m.keys.end(), (user ? "HKCU|" : "HKLM|") + key) != m.keys.end();
        };
        s.query = [&m](bool user, const std::string& key, const std::string& value, std::uint32_t size, std::uint32_t& type,
                       std::vector<std::uint8_t>& data) -> long {
            const std::string k = (user ? "HKCU|" : "HKLM|") + key;
            if (std::find(m.keys.begin(), m.keys.end(), k) == m.keys.end()) return 2;
            auto it = m.values.find(k + "|" + value);
            if (it == m.values.end()) return 2;
            type = it->second.first;
            if (it->second.second.size() > size) return 234;  // ERROR_MORE_DATA
            data = it->second.second;
            return 0;
        };
        s.drives = [&m](std::vector<std::string>& roots) -> std::uint32_t {
            if (!m.drive_list) return 0;
            for (const auto& d : m.drives) roots.push_back(d.first);
            return static_cast<std::uint32_t>(roots.size() * 4);
        };
        s.drive_type = [&m](const std::string& root) {
            for (const auto& d : m.drives)
                if (d.first == root) return d.second;
            return 1u;
        };
        return s;
    };
    auto sz = [](const std::string& s) {
        std::vector<std::uint8_t> v(s.begin(), s.end());
        v.push_back(0);
        return v;
    };
    const std::string dir = "C:\\Games\\TA\\";
    const std::vector<std::string> ext_exports = {"TotalAExtAction", "TotalAExtGetButtonText", "TotalAExtVersion"};
    Machine m;
    auto add_ext = [&](const std::string& file, const std::string& module) {
        m.files[up(dir + file)] = fake_dll(module, ext_exports);
        m.listing.push_back({file, false});
    };
    // No online.dll: no links, a press fails without a reason.
    {
        ta::OnlineServices s(sys_of(m), "C:\\Games\\TA");
        CHECK(s.links().empty());
        std::string msg = "x";
        CHECK(s.press(0, msg, {}) == 2 && msg.empty());
    }
    m.files[up(dir + "online.dll")] = fake_dll("ONLINE.dll", {"ONLGetLinkInfo", "ONLGetVersion", "ONLLoadConfigFile", "ONLProcessButtonCommand"});
    // The listing in the system's order; a folder, a DLL that is no extension, a 64-bit one.
    add_ext("tadwngox.dll", "TADWNGOX.dll");
    add_ext("taheatx.dll", "TAHEATX.dll");
    m.listing.push_back({"tadir.dll", true});
    m.files[up(dir + "tafoo.dll")] = fake_dll("TAFOO.dll", {"Something"});
    m.listing.push_back({"tafoo.dll", false});
    m.files[up(dir + "ta64.dll")] = fake_dll("TAKALIX.dll", ext_exports, true, 0x8664);
    m.listing.push_back({"ta64.dll", false});
    add_ext("takalix.dll", "TAKALIX.dll");
    add_ext("tamplayx.dll", "TAMPLAYX.dll");
    add_ext("tatenx.dll", "TATENX.dll");
    add_ext("tawirepx.dll", "TAWIREPX.dll");
    auto names = [](const std::vector<ta::OnlineLink>& l) {
        std::string s;
        for (const auto& k : l) s += std::to_string(k.id) + ":" + k.name + ";";
        return s;
    };
    {
        // Nothing installed: the Mplayer one first (it changes places with DWANGO).
        ta::OnlineServices s(sys_of(m), "C:\\Games\\TA");
        CHECK(names(s.links()) == "0:www.cavedog.com;1:Unit Download;2:Get Mplayer;3:Get HEAT;4:Get Kali;5:DWANGO;6:Play Free on TEN;7:Get Wireplay (UK);");
        std::vector<std::pair<std::string, bool>> opened;
        bool ok = true;
        auto open = [&](const std::string& t, bool p) {
            opened.push_back({t, p});
            return ok;
        };
        std::string msg;
        CHECK(s.press(0, msg, open) == 0 && opened.back().first == "http://www.cavedog.com" && !opened.back().second);
        CHECK(s.press(1, msg, open) == 0 && opened.back().first == "http://www.totalannihilation.com/ta/unit0.htm");
        CHECK(s.press(5, msg, open) == 0 && opened.back().first == "http://www.dwango.com/html/totala.html");
        CHECK(s.press(6, msg, open) == 0 && opened.back().first == "http://www.ten.net/totala");
        CHECK(s.press(4, msg, open) == 0 && opened.back().first == "http://www.kali.net/games/annihilation");
        // No CD in a CD-ROM drive: the installation CD question with the button's text.
        const std::size_t before = opened.size();
        CHECK(s.press(2, msg, open) == 1 && msg == "Please insert the Installation CD (Disc 1) and select\n\n\"Get Mplayer\" again.");
        CHECK(s.press(3, msg, open) == 1 && msg == "Please insert the Installation CD (Disc 1) and select\n\n\"Get HEAT\" again.");
        CHECK(s.press(7, msg, open) == 1 && msg == "Please insert the Installation CD (Disc 1) and select\n\n\"Get Wireplay (UK)\" again.");
        CHECK(opened.size() == before);
        // The CD's set-up program on a CD-ROM drive (a fixed drive with it does not count).
        m.drives = {{"C:\\", 3}, {"E:\\", 5}};
        m.files[up("C:\\Internet\\HEAT\\katalyst.exe")] = {1};
        CHECK(s.press(3, msg, open) == 1);
        m.files[up("E:\\Internet\\HEAT\\katalyst.exe")] = {1};
        m.files[up("E:\\Internet\\Mplayer\\mplaynow.exe")] = {1};
        CHECK(s.press(3, msg, open) == 0 && opened.back().first == "E:\\Internet\\HEAT\\katalyst.exe" && opened.back().second);
        CHECK(s.press(2, msg, open) == 0 && opened.back().first == "E:\\Internet\\Mplayer\\mplaynow.exe");
        m.drive_list = false;
        CHECK(s.press(7, msg, open) == 2 && msg == "No CD-ROM drives found.");
        m.drive_list = true;
        // The opener failing.
        ok = false;
        CHECK(s.press(0, msg, open) == 2 && msg == "Unable to browse to URL: http://www.cavedog.com");
        CHECK(s.press(5, msg, open) == 2 && msg == "Unable to browse to URL: http://www.dwango.com/html/totala.html");
        CHECK(s.press(2, msg, open) == 2 && msg == "Unable to execute command: E:\\Internet\\Mplayer\\mplaynow.exe");
        ok = true;
        CHECK(s.press(9, msg, open) == 2 && msg == "Internal error");
        CHECK(s.press(0xFFFFFFFFu, msg, open) == 2 && msg.empty());
        CHECK(ta::OnlineServices::failure_text("DWANGO") ==
              "The DWANGO service failed or took\n\ntoo long. Please see the readme\n\nfile for more information.");
    }
    {
        // Installed services.
        m.drives = {{"C:\\", 3}};
        m.keys = {"HKLM|Software\\Segasoft\\HEAT\\Main", "HKCU|Software\\Kali\\Kali95\\Misc",
                  "HKLM|SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\App Paths\\Wireplay.exe"};
        m.values["HKLM|Software\\Segasoft\\HEAT\\Main|Current Version"] = {1, sz("1.0")};
        m.values["HKCU|Software\\Kali\\Kali95\\Misc|Location"] = {1, sz("C:\\Kali")};
        m.values["HKLM|SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\App Paths\\Wireplay.exe|Path"] = {2, sz("D:\\Wire/")};
        m.files[up(dir + "ta.mpi")] = {1};
        ta::OnlineServices s(sys_of(m), dir);
        CHECK(names(s.links()) == "0:www.cavedog.com;1:Unit Download;2:Play on Mplayer;3:Play on HEAT;4:Play on Kali;5:DWANGO;6:Play Free on TEN;7:Play on Wireplay;");
        std::vector<std::pair<std::string, bool>> opened;
        auto open = [&](const std::string& t, bool p) {
            opened.push_back({t, p});
            return true;
        };
        std::string msg;
        CHECK(s.press(2, msg, open) == 0 && opened.back().first == "http://www.mplayer.com/cgi-bin/mplayer-set-up.sh?dist=16Q");
        CHECK(s.press(3, msg, open) == 0 && opened.back().first == "http://www.heat.net/pub/totala");
        CHECK(s.press(4, msg, open) == 0 && opened.back().first == "C:\\Kali\\Kali95.exe" && opened.back().second);
        CHECK(s.press(7, msg, open) == 0 && opened.back().first == "D:\\Wire/Wireplay.exe" && opened.back().second);
        // A HEAT version longer than its 10-byte buffer: not installed. A Kali key without its
        // Location: the button says Play, the press goes to the web page.
        m.values["HKLM|Software\\Segasoft\\HEAT\\Main|Current Version"] = {1, sz("1.0.0.12345")};
        m.values.erase("HKCU|Software\\Kali\\Kali95\\Misc|Location");
        // The Mplayer key alone (no ta.mpi) shows Play; its press needs the Root Directory.
        m.files.erase(up(dir + "ta.mpi"));
        m.keys.push_back("HKLM|Software\\Mpath\\Mplayer\\Main\\");
        CHECK(names(s.links()) == "0:www.cavedog.com;1:Unit Download;2:Play on Mplayer;3:Get HEAT;4:Play on Kali;5:DWANGO;6:Play Free on TEN;7:Play on Wireplay;");
        CHECK(s.press(4, msg, open) == 0 && opened.back().first == "http://www.kali.net/games/annihilation");
        CHECK(s.press(2, msg, open) == 1);
        m.values["HKLM|Software\\Mpath\\Mplayer\\Main\\|Root Directory"] = {1, sz("C:\\Mplayer")};
        CHECK(s.press(2, msg, open) == 0 && opened.back().first == "http://www.mplayer.com/cgi-bin/mplayer-set-up.sh?dist=16Q");
        // An extension file gone since the links were made.
        m.files.erase(up(dir + "tatenx.dll"));
        CHECK(s.press(6, msg, open) == 2 && msg == "Unable to load extension DLL.");
        // Translated names (a translation replaces the name, 31 characters at most).
        auto tr = [](const std::string& t) { return t == "DWANGO" ? std::string("DWANGO-Netz mit einem sehr langen Namen") : t; };
        const auto l = s.links(tr);
        CHECK(l.size() == 7 && l[5].name == "DWANGO-Netz mit einem sehr lang");
    }
    {
        // At most ten extensions; the Mplayer one moves first only from places 1..9.
        Machine n;
        n.files[up(dir + "online.dll")] = m.files[up(dir + "online.dll")];
        for (int k = 0; k < 11; ++k) {
            const std::string f = "tadw" + std::to_string(k) + ".dll";
            n.files[up(dir + f)] = fake_dll("TADWNGOX.dll", ext_exports);
            n.listing.push_back({f, false});
        }
        n.files[up(dir + "tamplay.dll")] = fake_dll("TAMPLAYX.dll", ext_exports);
        n.listing.insert(n.listing.begin() + 10, {"tamplay.dll", false});
        ta::OnlineServices s(sys_of(n), dir);
        const auto l = s.links();
        CHECK(l.size() == 12 && l[2].name == "DWANGO" && l[11].name == "DWANGO");
        n.listing.erase(n.listing.begin() + 10);
        n.listing.insert(n.listing.begin() + 9, {"tamplay.dll", false});
        const auto l2 = s.links();
        CHECK(l2.size() == 12 && l2[2].name == "Get Mplayer" && l2[11].name == "DWANGO");
    }
}


// The palette table builders (0x4BA750 / 0x4BADF0 / 0x4BABD0): hashes of the tables the
// original's builders produce for generated palettes (palette_harness: the port gives the same
// bytes on these, the shipped palette and 200 random palettes).
std::uint64_t fnv64(const std::vector<std::uint8_t>& v) {
    std::uint64_t h = 1469598103934665603ull;
    for (std::uint8_t b : v) h = (h ^ b) * 1099511628211ull;
    return h;
}

void test_palette_tables() {
    namespace fs = std::filesystem;
    ta::PaletteBytes syn{}, bw{}, five{}, black{};
    for (std::size_t i = 0; i < 256; ++i) {
        syn[i * 4] = static_cast<std::uint8_t>(i);
        syn[i * 4 + 1] = static_cast<std::uint8_t>(255 - i);
        syn[i * 4 + 2] = static_cast<std::uint8_t>(i * 7);
        for (std::size_t k = 0; k < 3; ++k) {
            bw[i * 4 + k] = (i & 1) ? 255 : 0;
            five[i * 4 + k] = static_cast<std::uint8_t>((i % 5) * 60 + k);
        }
    }
    CHECK(fnv64(ta::build_alpha_table(syn)) == 0x8F6D3F3770684F23ull);
    CHECK(fnv64(ta::build_shade_table(syn)) == 0x543005E4849BC643ull);
    CHECK(fnv64(ta::build_light_table(syn)) == 0x65C5720D49449FA6ull);
    CHECK(fnv64(ta::build_alpha_table(black)) == 0x07B316F51699D983ull);
    CHECK(fnv64(ta::build_shade_table(black)) == 0x1D00C08067BE8383ull);
    CHECK(fnv64(ta::build_light_table(black)) == 0x1D00C08067BE8383ull);
    CHECK(fnv64(ta::build_alpha_table(bw)) == 0x0625475D44A8ED83ull);
    CHECK(fnv64(ta::build_shade_table(bw)) == 0xB074277A2E6C9183ull);
    CHECK(fnv64(ta::build_light_table(bw)) == 0xABE39FCB1F44C383ull);
    CHECK(fnv64(ta::build_alpha_table(five)) == 0x48BB4B2B5E2D81ABull);
    CHECK(fnv64(ta::build_shade_table(five)) == 0x3C2A2D8E243FAC7Eull);
    CHECK(fnv64(ta::build_light_table(five)) == 0x236C9D1E9C8CFA3Aull);
    {
        // Nothing within 40 of the colour's sum: the entry at the position the scan stopped.
        const ta::SortedPalette s = ta::sort_palette(bw);
        CHECK(s.sum[127] == 0 && s.sum[128] == 765);
        CHECK(ta::nearest_entry(bw, s, 127, 127, 127) == s.order[128]);
        CHECK(ta::nearest_entry(bw, s, 255, 255, 200) == s.order[128]);  // within 40 of 765: the first of the ties
        CHECK(ta::nearest_entry(bw, s, 255, 255, 255) == s.order[128]);
        // Above every sum + 40 (not possible with bytes) / below: position 0 when the scan ends.
        const ta::SortedPalette z = ta::sort_palette(black);
        CHECK(ta::nearest_entry(black, z, 200, 200, 200) == z.order[0]);
    }
    // The PCX colour map: the last 768 bytes of a file with manufacturer 0x0A, version 5.
    std::vector<std::uint8_t> pcx(0x80 + 10 + 0x300, 0);
    pcx[0] = 0x0A;
    pcx[1] = 0x05;
    for (std::size_t i = 0; i < 0x300; ++i) pcx[pcx.size() - 0x300 + i] = static_cast<std::uint8_t>(i * 3 + 1);
    {
        ta::PaletteBytes p{};
        CHECK(ta::pcx_palette(pcx, p));
        CHECK(p[0] == 1 && p[1] == 4 && p[2] == 7 && p[3] == 0 && p[4] == 10 && p[1023] == 0 && p[1022] == static_cast<std::uint8_t>(767 * 3 + 1));
        std::vector<std::uint8_t> bad = pcx;
        bad[1] = 3;
        CHECK(!ta::pcx_palette(bad, p));
    }
    // The start-up sequence over a game folder holding only palettes\PALETTE.PCX (and a stale
    // PALETTE.ALP): the PAL is built from the PCX and written, the stale table deleted, all three
    // tables built and written; a second load reads them back.
    std::error_code ec;
    const fs::path dir = fs::temp_directory_path(ec) / "compat_tests_palettes";
    fs::remove_all(dir, ec);
    fs::create_directories(dir / "palettes", ec);
    auto write = [](const fs::path& p, const std::vector<std::uint8_t>& b) {
        std::ofstream o(p, std::ios::binary);
        o.write(reinterpret_cast<const char*>(b.data()), static_cast<std::streamsize>(b.size()));
    };
    auto read = [](const fs::path& p) {
        std::ifstream in(p, std::ios::binary);
        return std::vector<std::uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    };
    write(dir / "palettes" / "PALETTE.PCX", pcx);
    write(dir / "palettes" / "PALETTE.ALP", std::vector<std::uint8_t>(65536, 0x11));
    write(dir / "palettes" / "PALETTE.SHD", {});  // empty: missing
    {
        ta::Vfs vfs;
        vfs.language.clear();
        vfs.set_cd_drives({});
        vfs.mount_game_dir(dir);
        ta::PaletteTables t;
        std::string fatal;
        CHECK(ta::load_palette_tables(vfs, true, t, fatal));
        CHECK(t.built_palette && t.built_alpha && t.built_shade && t.built_light);
        ta::PaletteBytes p{};
        ta::pcx_palette(pcx, p);
        CHECK(t.palette == p);
        CHECK(t.alpha == ta::build_alpha_table(p) && t.shade == ta::build_shade_table(p) && t.light == ta::build_light_table(p));
        CHECK(read(dir / "palettes" / "PALETTE.PAL") == std::vector<std::uint8_t>(p.begin(), p.end()));
        CHECK(read(dir / "palettes" / "PALETTE.ALP") == t.alpha);
        CHECK(read(dir / "palettes" / "PALETTE.SHD") == t.shade);
        CHECK(read(dir / "palettes" / "PALETTE.LHT") == t.light);
        ta::PaletteTables u;
        CHECK(ta::load_palette_tables(vfs, false, u, fatal));
        CHECK(!u.built_palette && !u.built_alpha && !u.built_shade && !u.built_light);
        CHECK(u.palette == t.palette && u.alpha == t.alpha && u.shade == t.shade && u.light == t.light);
    }
    {
        // A loaded PAL keeps loose tables; without side effects nothing is written.
        write(dir / "palettes" / "PALETTE.LHT", std::vector<std::uint8_t>(100, 0x22));  // short: zero-filled
        fs::remove(dir / "palettes" / "PALETTE.SHD", ec);
        ta::Vfs vfs;
        vfs.language.clear();
        vfs.set_cd_drives({});
        vfs.mount_game_dir(dir);
        ta::PaletteTables t;
        std::string fatal;
        CHECK(ta::load_palette_tables(vfs, false, t, fatal));
        CHECK(!t.built_palette && !t.built_alpha && t.built_shade && !t.built_light);
        CHECK(t.light.size() == 8192 && t.light[99] == 0x22 && t.light[100] == 0);
        CHECK(!fs::exists(dir / "palettes" / "PALETTE.SHD"));
    }
    {
        // No palettes folder: nothing to build the palette from (the original's fatal error
        // with the path); with a PCX but no folder... (not possible: a loose PCX needs it).
        fs::remove_all(dir, ec);
        fs::create_directories(dir, ec);
        ta::Vfs vfs;
        vfs.language.clear();
        vfs.set_cd_drives({});
        vfs.mount_game_dir(dir);
        ta::PaletteTables t;
        std::string fatal;
        CHECK(!ta::load_palette_tables(vfs, true, t, fatal));
        CHECK(fatal == "palettes\\PALETTE.PCX");
        CHECK(!fs::exists(dir / "palettes"));
    }
    fs::remove_all(dir, ec);
}

// The mouse trails (0x4B5980 / 0x4B6110): the calls, their parameters and flags.
void test_mouse_trails() {
    struct Call {
        std::uint32_t action, param;
        bool data;
        std::uint32_t flags;
    };
    std::vector<Call> calls;
    int setting = 7;
    bool query_works = true;
    client::MouseTrails m([&](std::uint32_t action, std::uint32_t param, void* data, std::uint32_t flags) {
        calls.push_back({action, param, data != nullptr, flags});
        if (action == 0x5E) {
            if (!query_works) return 0;
            *static_cast<std::uint32_t*>(data) = static_cast<std::uint32_t>(setting);
        } else if (action == 0x5D) {
            setting = static_cast<int>(param);
        }
        return 1;
    });
    m.switch_off();
    CHECK(calls.size() == 2);
    if (calls.size() == 2) {
        CHECK(calls[0].action == 0x5E && calls[0].param == 0 && calls[0].data && calls[0].flags == 1);
        CHECK(calls[1].action == 0x5D && calls[1].param == 0 && !calls[1].data && calls[1].flags == 1);
    }
    CHECK(setting == 0 && m.saved() == 7);
    m.restore();
    CHECK(calls.size() == 3 && calls[2].action == 0x5D && calls[2].param == 7 && !calls[2].data && calls[2].flags == 1);
    CHECK(setting == 7);
    // A failed query leaves the saved word as it was (0 at start): the restore sets 0.
    calls.clear();
    query_works = false;
    client::MouseTrails n([&](std::uint32_t action, std::uint32_t param, void* data, std::uint32_t flags) {
        calls.push_back({action, param, data != nullptr, flags});
        if (action == 0x5E) return query_works ? 1 : 0;
        setting = static_cast<int>(param);
        return 1;
    });
    n.switch_off();
    n.restore();
    CHECK(calls.size() == 3 && n.saved() == 0 && calls[2].param == 0 && setting == 0);
    // Without a system call nothing happens.
    client::MouseTrails none(nullptr);
    none.switch_off();
    none.restore();
    CHECK(none.saved() == 0);
}

} // namespace

int main() {
    test_crt_pow10_table();
    test_crt_atof_quirks();
    test_comments();
    test_parse_basic();
    test_parse_errors();
    test_atoi_atof();
    test_stricmp();
    test_checksum();
    test_lz77();
    test_inflate();
    test_restrict_list();
    test_translations();
    test_skirmish_schema_rows();
    test_footprint_model_box();
    test_notice_texts();
    test_preferences();
    test_online_services();
    test_palette_tables();
    test_mouse_trails();
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
