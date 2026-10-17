// walks make_for_item bytes with a parser written from the doc layouts and checks it consumes exactly the buffer
//   ./extras_test.out   (run from gtps/test; reads ../../items.dat)
#include "extras.hpp"
#include <cstdio>
#include <map>

static int fails = 0;
// layout letters: B u8, H u16, I u32, Q u64, S str16, L u32 count + that many u32, P u32 count + 2*count u32
static void lay(Rd &r, const char *f)
{
    for (; *f; ++f)
        switch (*f)
        {
            case 'B': r.get<u8>(); break;
            case 'H': r.get<u16>(); break;
            case 'I': r.get<u32>(); break;
            case 'Q': r.get<u64>(); break;
            case 'S': r.str16(); break;
            case 'L': { u32 n = r.get<u32>(); r.skip(4ull * n); break; }
            case 'P': { u32 n = r.get<u32>(); r.skip(8ull * n); break; }
        }
}

// returns false for an unknown type
static bool walk(Rd &r, int t, int ver, int id)
{
    static const std::map<int, const char *> fixed = {
        {0x01, "SB"}, {0x02, "SI"}, {0x04, "IB"}, {0x05, ""}, {0x06, "SSSB"}, {0x07, "SSSB"}, {0x08, "B"}, {0x0a, "IB"}, {0x0b, "IS"},
        {0x0c, "SSSB"}, {0x0d, "SSSB"}, {0x0e, "SBIHHHHHHHHH"}, {0x0f, "I"}, {0x10, "B"}, {0x11, ""}, {0x12, "BI"}, {0x13, "HHHHHHHHH"},
        {0x14, "S"}, {0x15, "SIB"}, {0x16, ""}, {0x17, "I"}, {0x18, "II"}, {0x19, "BL"}, {0x1a, "BI"}, {0x1b, "I"}, {0x1c, "BIB"}, {0x1d, ""},
        {0x1e, "BI"}, {0x20, "L"}, {0x22, ""}, {0x23, "IS"}, {0x24, "SIII"}, {0x26, "I"}, {0x27, "I"}, {0x28, "I"}, {0x29, "I"},
        {0x2b, "IIII"}, {0x2c, "BIL"}, {0x2d, ""}, {0x2e, ""}, {0x2f, "SIB"}, {0x31, "IIB"}, {0x32, "I"}, {0x33, ""}, {0x34, ""},
        {0x35, "II"}, {0x36, "S"}, {0x38, "SI"}, {0x39, "I"}, {0x3a, ""}, {0x3b, ""}, {0x3c, "IB"}, {0x3d, "IBBIIIIIIIB"}, {0x3e, "IIBBI"},
        {0x40, "S"}, {0x41, ""}, {0x42, "B"}, {0x43, "IL"}, {0x44, "ISSL"}, {0x48, "III"}, {0x49, "I"}, {0x4a, ""}, {0x4b, "IS"},
        {0x4d, "IL"}, {0x4e, "I"}, {0x4f, "I"}, {0x50, "II"}, {0x51, "II"}, {0x52, ""}};
    auto it = fixed.find(t);
    if (it != fixed.end()) { lay(r, it->second); return true; }
    switch (t)
    {
        case 0x03: lay(r, "BIL"); if (ver >= 12) lay(r, "I"); if (ver >= 13) lay(r, "I"); return true;
        case 0x09: lay(r, "I"); if (id == 5318 || (id == 10656 && ver > 16)) lay(r, "I"); return true;
        case 0x1f: lay(r, "BSIIIB"); if (id == 3316) lay(r, "IIII"); return true;
        case 0x21: if (id == 3394) lay(r, "S"); return true;
        case 0x25: lay(r, "S"); { u32 n = r.get<u32>(); r.get<u32>(); r.skip(4ull * n); } return true; // count comes before the age
        case 0x2a: lay(r, "B"); if (ver > 18) lay(r, "III"); if (ver > 19) lay(r, "II"); return true;
        case 0x30: lay(r, "SIIIIHHH"); if (ver > 8) lay(r, "I"); return true;
        case 0x37: lay(r, "IL"); lay(r, "I"); if (ver > 13) lay(r, "Q"); return true; // heat, n + n values, timer
        case 0x3f: { u32 n = r.get<u32>(); for (u32 k = 0; k < n; ++k) lay(r, "IBIIS"); lay(r, "II"); return true; }
        case 0x45: case 0x46: lay(r, "III"); if (ver > 7) lay(r, "I"); return true;
        case 0x47: lay(r, "III"); if (ver > 7) lay(r, "I"); lay(r, "IIBBII"); return true;
        case 0x4c: for (int k = 0; k < 3; ++k) { r.skip(100); lay(r, "P"); } return true;
    }
    return false;
}

int main()
{
    Items items;
    if (!items.load("../../items.dat")) { printf("FAIL  items.dat\n"); return 1; }
    std::map<int, int> covered; int total = 0;
    for (int ver : {0, 4, 5, 12, 13, 15, 19, 20, 23, 27})
        for (const Item &it : items.list)
        {
            int t = extras::type_for_action(it.type);
            if (!t) continue;
            std::vector<u8> v = extras::make_for_item(it, ver);
            Rd r(v.data(), v.size());
            int got = r.get<u8>();
            bool known = got == t && walk(r, t, ver, it.id);
            if (it.flags2 & 0x200) { if (it.id != 5814) r.get<u8>(); lay(r, "IIII"); }
            else if (it.id == 6546 && ver >= 5) { lay(r, "IIIIII"); if (ver >= 15) lay(r, "I"); }
            if (!known || !r.ok || r.o != v.size()) { printf("FAIL  item %d (%s) action %d type 0x%02x ver %d: used %zu of %zu\n", it.id, it.name.c_str(), it.type, t, ver, r.o, v.size()); ++fails; }
            ++covered[t]; ++total;
        }
    // every extra type walks on its own too (items.dat does not have an item for each)
    for (int t = 1; t <= 0x52; ++t)
    {
        std::vector<u8> v = extras::make_default(t, 27);
        Rd r(v.data(), v.size()); r.get<u8>();
        if (!walk(r, t, 27, t == 0x1f ? 3316 : t == 0x21 ? 3394 : 0) || !r.ok || r.o != v.size()) { printf("FAIL  default type 0x%02x\n", t); ++fails; }
    }
    printf("%d items in %zu extra types; %s\n", total, covered.size(), fails ? "FAILED" : "PASS");
    return fails != 0;
}
