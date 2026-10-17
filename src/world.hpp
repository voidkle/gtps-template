// world model, generator and the wire/save format (docs: guide/world-data, guide/tile-extras)
#pragma once
#include "items.hpp"
#include <algorithm>
#include <chrono>
#include <fstream>
#include <filesystem>
#include <map>
#include <random>
#include <set>

inline std::mt19937 &rng() { static std::mt19937 r{std::random_device{}()}; return r; }
inline int rnd(int lo, int hi) { return std::uniform_int_distribution<int>(lo, hi)(rng()); }
inline i64 now_s() { return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count(); }
inline i64 now_ms() { return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

enum TileFlag : u16 { T_EXTRA = 0x01, T_LOCKED = 0x02, T_SPAWN_SEEDS = 0x08, T_SEEDLING = 0x10, T_FLIPPED = 0x20, T_ON = 0x40, T_PUBLIC = 0x80 };
enum ActionType : u8 { A_FIST = 0, A_WRENCH = 1, A_DOOR = 2, A_LOCK = 3, A_SIGN = 10, A_MAIN_DOOR = 13, A_BEDROCK = 15, A_LAVA = 16, A_FOREGROUND = 17, A_BACKGROUND = 18, A_SEED = 19, A_CLOTHES = 20, A_PORTAL = 26 };

struct Tile
{
    u16 fg = 0, bg = 0, parent = 0, flags = 0;
    std::vector<u8> extra;     // type byte + data, exactly as sent (empty = none)
    u16 damage = 0; i64 last_hit = 0; // server only: damage so far in raw units (a Fist punch is 6, the item's break value is hits * 6)
    i64 planted = 0;           // seeds: unix seconds, the wire sends the age
    std::string text;          // sign / door label, kept for rewrites
    i32 lock_owner = 0; std::vector<i32> access; u8 lock_flags = 0; // lock tiles: owner user id, access list, settings bits
    std::string aux;           // other server-only state as text (vending machines: "stock money readyAt")
    std::string target, door_id; // doors: destination "WORLD", "WORLD:ID" or ":ID", and this door's own id (server only)
};

struct Drop { u16 id = 0; float x = 0, y = 0; u8 count = 1, flags = 0; u32 uid = 0; };

struct World
{
    u16 version = 27;
    std::string name;
    i32 w = 100, h = 60;
    std::vector<Tile> tiles;
    std::vector<Drop> drops;
    u32 last_uid = 0;
    u16 base_weather = 0, terraform = 0, weather = 0;
    i32 owner = 0;                    // user id of the world lock owner, 0 = unowned
    std::string owner_name;
    std::map<i32, i64> banned;        // user id -> unix time the world ban ends (0 = never)
    bool nuked = false;               // moderators can close a world to everyone else
    i32 lock_tile = -1;               // index of the world lock
    int door_x = 0, door_y = 23;
    bool dirty = false;

    Tile *at(int x, int y) { return x < 0 || y < 0 || x >= w || y >= h ? nullptr : &tiles[y * w + x]; }

    void generate(const Items &items)
    {
        tiles.assign(w * h, {});
        door_x = rnd(2, w - 3); door_y = 23;
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x)
            {
                Tile &t = *at(x, y);
                if (y >= 24)
                {
                    t.bg = 14; // cave background
                    t.fg = 2;  // dirt
                    if (y >= 50 && rnd(0, 3) == 0) t.fg = 4;           // lava
                    else if (y >= 30 && rnd(0, 9) == 0) t.fg = 10;     // rock
                    if (y >= 54) t.fg = 8;                             // bedrock
                }
            }
        at(door_x, door_y)->fg = 6; at(door_x, door_y)->bg = 0;
        set_door(*at(door_x, door_y), "EXIT");
        at(door_x, door_y + 1)->fg = 8; at(door_x, door_y + 1)->bg = 14;
        dirty = true; (void)items;
    }

    // door extra: label + a flags byte (bit 3 = locked: only people with access may use it)
    static void set_door(Tile &t, const std::string &label, bool locked = false)
    {
        Buf b; b.put<u8>(0x01).str16(label).put<u8>(locked ? 0x08 : 0);
        t.extra = b.d; t.flags |= T_EXTRA; t.text = label;
    }
    static void set_sign(Tile &t, const std::string &text)
    {
        Buf b; b.put<u8>(0x02).str16(text).put<i32>(-1);
        t.extra = b.d; t.flags |= T_EXTRA; t.text = text;
    }
    static void set_seed(Tile &t, i64 planted)
    {
        t.planted = planted; t.flags |= T_EXTRA;
    }
    // rebuilds a lock tile's wire extra from its fields
    void set_lock(Tile &t, i32 owner_id, const std::vector<i32> &acc, u8 flags = 0)
    {
        t.lock_owner = owner_id; t.access = acc; t.lock_flags = flags;
        Buf b; b.put<u8>(0x03).put<u8>(flags).put<i32>(owner_id).put<u32>(acc.size());
        for (i32 a : acc) b.put<i32>(a);
        if (version >= 12) b.put<u32>(0);
        if (version >= 13) b.put<u32>(0);
        t.extra = b.d; t.flags |= T_EXTRA;
    }

    // seed extra is regenerated at send time because the age keeps growing
    std::vector<u8> extra_of(const Tile &t, const Items &items) const
    {
        const Item *it = items.get(t.fg);
        if (it && it->type == A_SEED) { Buf b; b.put<u8>(0x04).put<u32>(std::max<i64>(0, now_s() - t.planted)).put<u8>(t.extra.size() > 1 ? t.extra[1] : 3); return b.d; }
        return t.extra;
    }

    void write_tile(Buf &b, const Tile &t, const Items &items) const
    {
        b.put<u16>(t.fg).put<u16>(t.bg).put<u16>(t.parent).put<u16>(t.flags);
        if (t.flags & T_LOCKED) b.put<u16>(t.parent);
        if (t.flags & T_EXTRA) { auto e = extra_of(t, items); b.raw(e.data(), e.size()); }
    }

    // the 0x04 extra data
    std::vector<u8> serialize(const Items &items) const
    {
        Buf b;
        b.put<u16>(version).put<u32>(0).str16(name).put<i32>(w).put<i32>(h).put<u32>(tiles.size());
        b.zero(5);
        for (const Tile &t : tiles) write_tile(b, t, items);
        b.zero(12);
        b.put<u32>(drops.size()).put<u32>(last_uid);
        for (const Drop &d : drops) b.put<u16>(d.id).put<float>(d.x).put<float>(d.y).put<u8>(d.count).put<u8>(d.flags).put<u32>(d.uid);
        b.put<u16>(base_weather).put<u16>(terraform).put<u16>(weather).put<u16>(0).put<u32>(0);
        b.put<u32>(0); // version 27: world+0x1a4
        return b.d;
    }

    // one tile as the extra data of 0x05
    std::vector<u8> tile_update(int x, int y, const Items &items) const
    {
        Buf b; write_tile(b, tiles[y * w + x], items); return b.d;
    }

    // ---- save file: our own layout. extras are kept as raw wire bytes, so new tile types need no loader code
    void save(const std::string &dir, const Items &) const
    {
        std::filesystem::create_directories(dir);
        Buf b; b.put<u32>(0x36575447) // 'GTW6'
            .put<i32>(owner).put<i32>(lock_tile).put<i32>(door_x).put<i32>(door_y).str16(owner_name).put<u16>(version).str16(name)
            .put<i32>(w).put<i32>(h).put<u16>(base_weather).put<u16>(terraform).put<u16>(weather).put<u8>(nuked);
        b.put<u32>(banned.size()); for (auto &[uid, until] : banned) b.put<i32>(uid).put<i64>(until);
        for (const Tile &t : tiles) b.put<u16>(t.fg).put<u16>(t.bg).put<u16>(t.parent).put<u16>(t.flags).put<i64>(t.planted);
        u32 ne = 0; for (const Tile &t : tiles) if (!t.extra.empty() || !t.target.empty() || !t.door_id.empty() || !t.text.empty() || !t.aux.empty()) ++ne;
        b.put<u32>(ne);
        for (size_t i = 0; i < tiles.size(); ++i)
        {
            const Tile &t = tiles[i];
            if (t.extra.empty() && t.target.empty() && t.door_id.empty() && t.text.empty() && t.aux.empty()) continue;
            b.put<u32>(i).put<u32>(t.extra.size()).raw(t.extra.data(), t.extra.size()).str16(t.text).str16(t.target).str16(t.door_id).str16(t.aux);
        }
        b.put<u32>(last_uid).put<u32>(drops.size());
        for (const Drop &d : drops) b.put<u16>(d.id).put<float>(d.x).put<float>(d.y).put<u8>(d.count).put<u8>(d.flags).put<u32>(d.uid);
        std::ofstream f(dir + "/" + name + ".wld", std::ios::binary | std::ios::trunc);
        f.write((const char *)b.d.data(), b.d.size());
    }

    bool load(const std::string &dir, const std::string &n)
    {
        std::ifstream f(dir + "/" + n + ".wld", std::ios::binary);
        if (!f) return false;
        std::vector<u8> d((std::istreambuf_iterator<char>(f)), {});
        Rd r(d.data(), d.size());
        u32 magic = r.get<u32>();
        if (magic < 0x32575447 || magic > 0x36575447) return false; // GTW2 (no nuke flag), GTW3 (no ban list), GTW4 (bans without an end), GTW5
        owner = r.get<i32>(); lock_tile = r.get<i32>(); door_x = r.get<i32>(); door_y = r.get<i32>(); owner_name = r.str16();
        version = r.get<u16>(); name = r.str16(); w = r.get<i32>(); h = r.get<i32>();
        base_weather = r.get<u16>(); terraform = r.get<u16>(); weather = r.get<u16>();
        if (magic >= 0x33575447) nuked = r.get<u8>();
        banned.clear(); if (magic >= 0x34575447) { u32 nb = r.get<u32>(); for (u32 i = 0; i < nb && r.ok; ++i) { i32 uid = r.get<i32>(); banned[uid] = magic >= 0x35575447 ? r.get<i64>() : 0; } }
        if (!r.ok || w <= 0 || h <= 0 || (size_t)w * h > 65025) return false;
        tiles.assign((size_t)w * h, {});
        for (Tile &t : tiles) { t.fg = r.get<u16>(); t.bg = r.get<u16>(); t.parent = r.get<u16>(); t.flags = r.get<u16>(); t.planted = r.get<i64>(); }
        u32 ne = r.get<u32>();
        for (u32 k = 0; k < ne && r.ok; ++k)
        {
            u32 i = r.get<u32>(), len = r.get<u32>(); std::string raw = r.take(len);
            if (i >= tiles.size()) return false;
            Tile &t = tiles[i]; t.extra.assign(raw.begin(), raw.end()); t.text = r.str16(); t.target = r.str16(); t.door_id = r.str16(); if (magic >= 0x36575447) t.aux = r.str16();
            if (!t.extra.empty() && t.extra[0] == 0x03 && t.extra.size() >= 10) // lock: recover owner, access, settings
            {
                Rd lr(t.extra.data() + 1, t.extra.size() - 1);
                t.lock_flags = lr.get<u8>(); t.lock_owner = lr.get<i32>(); u32 c = lr.get<u32>();
                for (u32 q = 0; q < c && lr.ok; ++q) t.access.push_back(lr.get<i32>());
            }
        }
        last_uid = r.get<u32>(); u32 nd = r.get<u32>(); drops.clear();
        for (u32 i = 0; i < nd && r.ok; ++i) { Drop dr; dr.id = r.get<u16>(); dr.x = r.get<float>(); dr.y = r.get<float>(); dr.count = r.get<u8>(); dr.flags = r.get<u8>(); dr.uid = r.get<u32>(); drops.push_back(dr); }
        return r.ok;
    }
};
