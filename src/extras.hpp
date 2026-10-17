// tile extras: fresh bytes per extra type as the client reads them from the server (docs: guide/tile-extras, guide/world-data)
#pragma once
#include "proto.hpp"
#include "items.hpp"

namespace extras
{
// action type -> extra type (world-data "Which tiles have an extra")
inline int type_for_action(int action_type)
{
    static const struct { int action, type; } map[] = {
    {2, 0x01}, {3, 0x03}, {10, 0x02}, {13, 0x01}, {15, 0x2a}, {19, 0x04}, {26, 0x01}, {30, 0x05}, {33, 0x06},
    {34, 0x07}, {36, 0x08}, {38, 0x09}, {40, 0x0a}, {43, 0x01}, {46, 0x0b}, {47, 0x0c}, {48, 0x0d}, {49, 0x0e},
    {50, 0x07}, {51, 0x0f}, {52, 0x10}, {53, 0x11}, {54, 0x12}, {55, 0x13}, {56, 0x14}, {57, 0x15}, {59, 0x16},
    {61, 0x17}, {62, 0x18}, {63, 0x19}, {65, 0x1a}, {66, 0x1b}, {67, 0x1c}, {68, 0x1d}, {71, 0x1e}, {72, 0x1f},
    {73, 0x20}, {74, 0x21}, {75, 0x22}, {76, 0x23}, {77, 0x24}, {78, 0x25}, {79, 0x26}, {80, 0x27}, {81, 0x28},
    {82, 0x29}, {83, 0x2b}, {84, 0x2c}, {85, 0x2d}, {86, 0x21}, {87, 0x2f}, {88, 0x30}, {89, 0x31}, {91, 0x32},
    {92, 0x33}, {93, 0x34}, {96, 0x35}, {97, 0x36}, {98, 0x37}, {99, 0x38}, {100, 0x39}, {101, 0x3a}, {102, 0x3b},
    {103, 0x3c}, {104, 0x3c}, {105, 0x3c}, {106, 0x3c}, {109, 0x3d}, {111, 0x3e}, {113, 0x3f}, {115, 0x40},
    {116, 0x42}, {117, 0x43}, {118, 0x44}, {123, 0x45}, {124, 0x46}, {125, 0x47}, {126, 0x48}, {127, 0x49},
    {130, 0x4a}, {131, 0x4b}, {133, 0x4c}, {134, 0x4d}, {137, 0x4e}, {140, 0x4f}, {141, 0x50}, {142, 0x51},
    };
    for (auto &m : map) if (m.action == action_type) return m.type;
    return 0;
}

inline bool supported_by_server(int action_type) { return type_for_action(action_type) != 0; }

// item_id 0 = the item the type is made for; it only matters where the client reads a field for particular items
inline std::vector<u8> make_default(int t, int ver, int item_id = 0)
{
    Buf b; b.put<u8>(t);
    auto z32 = [&](int n = 1) { while (n--) b.put<u32>(0); };
    auto s = [&] { b.str16(""); };
    switch (t)
    {
        case 0x01: s(); b.put<u8>(0); break;
        case 0x02: s(); b.put<i32>(-1); break;                           // gurotopia sends -1
        case 0x03: b.put<u8>(0); z32(2); if (ver >= 12) z32(); if (ver >= 13) z32(); break; // flags, owner, count
        case 0x04: z32(); b.put<u8>(3); break;                           // age 0, 3 fruit
        case 0x06: case 0x07: case 0x0c: case 0x0d: s(); s(); s(); b.put<u8>(0); break;
        case 0x08: b.put<u8>(0); break;
        case 0x09: z32(); if (item_id == 5318 || (item_id == 10656 && ver > 16)) z32(); break;
        case 0x0a: z32(); b.put<u8>(0x7f); break;
        case 0x0b: z32(); s(); break;
        case 0x0e: s(); b.put<u8>(0); z32(); b.zero(18); break;          // empty clothes: no 5712/10044 flag words
        case 0x0f: case 0x1b: case 0x26: case 0x27: case 0x32: case 0x39: z32(); break;
        case 0x10: b.put<u8>(4); break;
        case 0x12: case 0x1a: b.put<u8>(t == 0x1a ? 0x18 : 0); z32(); break;
        case 0x13: b.zero(18); break;
        case 0x14: case 0x36: case 0x40: s(); break;
        case 0x15: s(); z32(); b.put<u8>(0); break;
        case 0x17: z32(); break;
        case 0x18: z32(2); break;                                        // item, price
        case 0x19: b.put<u8>(0); z32(); break;
        case 0x1c: b.put<u8>(0); z32(); b.put<u8>(0); break;
        case 0x1e: b.put<u8>(0); z32(); break;
        case 0x1f:
            b.put<u8>(0); s(); z32(3); b.put<u8>(0);
            if (item_id == 0 || item_id == 3316) { b.put<u32>(21600).put<u32>(21600).put<u32>(0).put<u32>(0); } // food, water, colour, neglect
            break;
        case 0x20: z32(); break;
        case 0x21: if (item_id == 0 || item_id == 3394) s(); break;     // country code, Country Flag only
        case 0x23: z32(); s(); break;
        case 0x24: s(); z32(3); break;
        case 0x25: s(); z32(2); break;                                   // count 0, age
        case 0x28: b.put<u32>(0x4080ff00); break;
        case 0x29: z32(); break;
        case 0x2a: b.put<u8>(0); if (ver > 18) b.put<u32>(1).put<u32>(1).put<u32>(1); if (ver > 19) b.put<u32>(0).put<i32>(-1); break;
        case 0x2b: z32(4); break;
        case 0x2c: b.put<u8>(0); z32(2); break;
        case 0x2f: s(); z32(); b.put<u8>(0); break;
        case 0x30: s(); z32(4); b.zero(6); if (ver > 8) b.put<u32>(6); break; // skin of head 6; no Will/Golem/Infinity Crown flags
        case 0x31: b.put<u32>(2).put<u32>(50).put<u8>(0); break;
        case 0x35: z32(2); break;
        case 0x51: b.put<u32>(0).put<u32>(1); break;
        case 0x37: z32(3); if (ver > 13) b.put<u64>(0); break;           // heat, n, timer
        case 0x38: s(); z32(); break;
        case 0x3c: z32(); b.put<u8>(0); break;
        case 0x3d: b.put<u32>(0).put<u8>(0).put<u8>(0).zero(28).put<u8>(0); break; // size ? done | fish ? remaining progress ? rate target (7 u32) | paused
        case 0x3e: z32(2); b.zero(2); z32(); break;
        case 0x3f: z32(3); break;                                        // n, ?, state
        case 0x42: b.put<u8>(0); break;
        case 0x43: case 0x4d: z32(2); break;                             // value, n
        case 0x44: z32(); s(); s(); z32(); break;
        case 0x45: case 0x46: case 0x47:
            z32(3); if (ver > 7) z32();
            if (t == 0x47) { z32(2); b.zero(2); z32(2); }                // storage item, count, ?, look, offset, ?
            break;
        case 0x48: b.put<u32>(0).put<u32>(1).put<u32>(0); break;
        case 0x49: case 0x4e: case 0x4f: z32(); break;
        case 0x4b: z32(); s(); break;
        case 0x4c: for (int k = 0; k < 3; ++k) z32(26); break;           // 25 stats + pair count 0, three entries
        case 0x50: b.put<u32>(0).put<u32>(0xffffffff); break;
        // 0x05 0x11 0x16 0x1d 0x22 0x2d 0x2e 0x33 0x34 0x3a 0x3b 0x41 0x4a 0x52: type byte only
    }
    return b.d;
}

// full extra for an item incl. the common trailer, empty if the item has none
inline std::vector<u8> make_for_item(const Item &it, int ver)
{
    int t = type_for_action(it.type);
    if (!t) return {};
    std::vector<u8> v = make_default(t, ver, it.id);
    if (t == 0x3c) v.back() = it.type - 103;                             // faction byte from the item type
    Buf b; b.d = std::move(v);
    if (it.flags2 & 0x200)
    {
        if (it.id != 5814) b.put<u8>(0);
        b.put<u32>(0).put<u32>(0).put<u32>(0).put<u32>(0);               // guild id, emblem, level, emblem-drawn flag
    }
    else if (it.id == 6546 && ver >= 5)
    {
        b.zero(24);
        if (ver >= 15) b.zero(4);                                        // the u64 of version 11+ is not sent by the server
    }
    return b.d;
}
}
