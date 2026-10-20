// Tests for the TA compatibility layer (no game data required).

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include <fstream>

#include "ta/crt_strtod.h"
#include "ta/hpi.h"
#include "ta/inflate.h"
#include "ta/restrict_list.h"
#include "ta/tdf.h"
#include "ta/translate.h"

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
    ta::load_translations(tdf, "english");  // no english fields: every text as it is
    CHECK(ta::translate("KILLS") == "KILLS");
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
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
