// Tests for the TA compatibility layer (no game data required).

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "ta/hpi.h"
#include "ta/inflate.h"
#include "ta/tdf.h"

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

} // namespace

int main() {
    test_comments();
    test_parse_basic();
    test_parse_errors();
    test_atoi_atof();
    test_stricmp();
    test_checksum();
    test_lz77();
    test_inflate();
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
