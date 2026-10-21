// Tests for the TA compatibility layer (no game data required).

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include <array>
#include <filesystem>
#include <fstream>

#include "client/sound_rules.h"
#include "ta/battle_loader.h"
#include "ta/crt_strtod.h"
#include "ta/hpi.h"
#include "ta/inflate.h"
#include "ta/notice_texts.h"
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
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
