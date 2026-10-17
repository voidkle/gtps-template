// items.dat reader, versions 1..27 (docs: guide/items-dat)
#pragma once
#include "proto.hpp"
#include <fstream>
#include <iterator>

struct Item
{
    i32 id = 0; u16 flags = 0; u8 type = 0, material = 0;
    std::string name, texture, extra_file, info, extra_options, punch_options;
    u8 break_hits = 0, break_raw = 0, clothing = 0, max_amount = 200, collision = 0;
    u16 rarity = 0; i32 reset_time = 0, grow_time = 0;
    i32 flags2 = 0;
    u16 splice_a = 0, splice_b = 0;
};

struct Items
{
    u16 version = 0;
    std::vector<Item> list;
    std::vector<u8> raw; // the file as sent in 0x10
    u32 hash = 0;

    const Item *get(i32 id) const { return id >= 0 && (size_t)id < list.size() ? &list[id] : nullptr; }

    bool load(const std::string &path)
    {
        std::ifstream f(path, std::ios::binary);
        raw.assign(std::istreambuf_iterator<char>(f), {});
        if (raw.size() < 6) return false;
        hash = hash_string(raw.data(), raw.size());
        Rd r(raw.data(), raw.size());
        version = r.get<u16>();
        i32 count = r.get<i32>();
        static const char key[] = "PBG892FXX982ABC*";
        list.assign(count, {});
        for (i32 i = 0; i < count && r.ok; ++i)
        {
            Item &it = list[i];
            it.id = r.get<i32>();
            it.flags = r.get<u16>(); it.type = r.get<u8>(); it.material = r.get<u8>();
            std::string nm = r.str16();
            if (version >= 3) for (size_t k = 0; k < nm.size(); ++k) nm[k] ^= key[(k + it.id) % 16];
            it.name = nm;
            it.texture = r.str16(); r.get<i32>();
            r.get<u8>(); r.get<i32>(); r.get<u8>(); r.get<u8>(); r.get<u8>(); r.get<u8>();
            it.collision = r.get<u8>(); it.break_raw = r.get<u8>(); it.break_hits = it.break_raw / 6;
            it.reset_time = r.get<i32>();
            it.clothing = r.get<u8>(); it.rarity = r.get<u16>(); it.max_amount = r.get<u8>();
            it.extra_file = r.str16(); r.get<i32>(); r.get<i32>();
            if (version >= 4) { r.str16(); r.str16(); r.str16(); }
            if (version >= 5) r.str16();
            r.skip(4); r.get<i32>(); r.get<i32>(); // seed sprites + colors
            r.skip(4);                              // ingredients (u16 x2)
            it.grow_time = r.get<i32>();
            if (version >= 7) { r.get<i32>(); it.extra_options = r.str16(); }
            if (version >= 8) { r.str16(); r.str16(); }
            r.skip(8);                              // u64
            if (version >= 9) { it.flags2 = r.get<i32>(); r.skip(60); }
            if (version >= 10) r.skip(8);
            if (version >= 11) it.punch_options = r.str16();
            if (version >= 12) r.skip(4 + 9);
            if (version >= 13) r.get<i32>();
            if (version >= 14) r.get<i32>();
            if (version >= 15) { r.get<u8>(); r.skip(24); r.str16(); }
            if (version >= 16) r.str16();
            if (version >= 17) r.get<i32>();
            if (version >= 18) r.get<i32>();
            if (version >= 19) { r.get<u8>(); r.get<i32>(); r.get<i32>(); }
            if (version >= 20) r.get<u8>();
            if (version >= 21) r.get<u8>();
            if (version >= 22) it.info = r.str16();
            if (version >= 23) { it.splice_a = r.get<u16>(); it.splice_b = r.get<u16>(); }
            if (version >= 24) r.get<u8>();
            if (version >= 25) { r.str16(); r.get<i32>(); }
            if (version >= 26) r.get<u8>();
            if (version >= 27) r.str16();
        }
        return r.ok && r.o == raw.size();
    }
};
