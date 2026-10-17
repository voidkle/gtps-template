// wire helpers: little-endian buffer, 56-byte game packet header, variant lists (docs: guide/game-packet, guide/variant-list)
#pragma once
#include <cstdint>
#include <cstring>
#include <string>
#include <variant>
#include <vector>
#include <enet/enet.h>

using u8 = uint8_t; using u16 = uint16_t; using u32 = uint32_t; using i32 = int32_t; using u64 = uint64_t; using i64 = int64_t;

struct Buf // writer
{
    std::vector<u8> d;
    template<class T> Buf &put(T v) { u8 b[sizeof v]; memcpy(b, &v, sizeof v); d.insert(d.end(), b, b + sizeof v); return *this; }
    Buf &str16(const std::string &s) { put<u16>(s.size()); d.insert(d.end(), s.begin(), s.end()); return *this; }
    Buf &str32(const std::string &s) { put<u32>(s.size()); d.insert(d.end(), s.begin(), s.end()); return *this; }
    Buf &raw(const void *p, size_t n) { auto *b = (const u8 *)p; d.insert(d.end(), b, b + n); return *this; }
    Buf &zero(size_t n) { d.insert(d.end(), n, 0); return *this; }
};

struct Rd // reader, never reads past the end (a short read leaves ok = false and returns 0)
{
    const u8 *p; size_t n, o = 0; bool ok = true;
    Rd(const u8 *p, size_t n) : p(p), n(n) {}
    template<class T> T get() { T v{}; if (o + sizeof v > n) { ok = false; o = n; return v; } memcpy(&v, p + o, sizeof v); o += sizeof v; return v; }
    std::string str16() { u16 l = get<u16>(); return take(l); }
    std::string str32() { u32 l = get<u32>(); return take(l); }
    std::string take(size_t l) { if (o + l > n) { ok = false; o = n; return {}; } std::string s((const char *)p + o, l); o += l; return s; }
    void skip(size_t l) { if (o + l > n) { ok = false; o = n; } else o += l; }
};

enum Msg : u32 { HELLO = 1, TEXT = 2, GAME_MSG = 3, GAME_PACKET = 4 };

struct GamePacket // 56 bytes
{
    u8 type = 0, objectType = 0, jumpCount = 0, buildRange = 0;
    i32 netID = 0, targetNetID = 0, flags = 0;
    float floatVar = 0;
    i32 itemID = 0;
    float posX = 0, posY = 0, speedX = 0, speedY = 0, particleID = 0;
    i32 tileX = 0, tileY = 0;
    u32 extraLen = 0;
};
static_assert(sizeof(GamePacket) == 56);

enum : i32 { S_UPDATE = 0x04, S_EXTENDED = 0x08, S_LEFT = 0x10, S_RIGHT = 0x20 };

inline void send_raw(ENetPeer *peer, const void *p, size_t n, bool reliable = true)
{
    ENetPacket *pk = enet_packet_create(p, n, reliable ? ENET_PACKET_FLAG_RELIABLE : 0);
    if (enet_peer_send(peer, 0, pk)) enet_packet_destroy(pk);
}

inline void send_hello(ENetPeer *peer) { u32 m = HELLO; send_raw(peer, &m, 4); }

// the client overwrites the last byte of a text message with NUL, so we always append one
inline void send_text(ENetPeer *peer, Msg type, const std::string &text)
{
    Buf b; b.put<u32>(type).raw(text.data(), text.size()).put<u8>(0);
    send_raw(peer, b.d.data(), b.d.size());
}
inline void send_action(ENetPeer *peer, const std::string &action, const std::string &rest = "")
{
    send_text(peer, GAME_MSG, "action|" + action + "\n" + rest);
}

inline void send_packet(ENetPeer *peer, GamePacket h, const std::vector<u8> &extra = {}, bool reliable = true)
{
    if (!extra.empty()) h.flags |= S_EXTENDED;
    h.extraLen = extra.size();
    Buf b; b.put<u32>(GAME_PACKET).raw(&h, sizeof h).raw(extra.data(), extra.size());
    send_raw(peer, b.d.data(), b.d.size(), reliable);
}

struct U32 { u32 v; };      // variant type 5
struct I64 { int64_t v; };  // variant type 10
struct Vec2 { float x, y; };
struct Vec3 { float x, y, z; };
using Var = std::variant<std::string, const char *, i32, U32, float, Vec2, Vec3, I64>;

inline std::vector<u8> varlist(std::initializer_list<Var> vars)
{
    Buf b; b.put<u8>(vars.size());
    u8 idx = 0;
    for (const Var &v : vars)
    {
        b.put<u8>(idx++);
        std::visit([&](auto &&x) {
            using T = std::decay_t<decltype(x)>;
            if constexpr (std::is_same_v<T, std::string>)      b.put<u8>(2).str32(x);
            else if constexpr (std::is_same_v<T, const char *>) b.put<u8>(2).str32(x);
            else if constexpr (std::is_same_v<T, i32>)         b.put<u8>(9).put<i32>(x);
            else if constexpr (std::is_same_v<T, U32>)         b.put<u8>(5).put<u32>(x.v);
            else if constexpr (std::is_same_v<T, float>)       b.put<u8>(1).put<float>(x);
            else if constexpr (std::is_same_v<T, Vec2>)        b.put<u8>(3).template put<float>(x.x).template put<float>(x.y);
            else if constexpr (std::is_same_v<T, Vec3>)        b.put<u8>(4).template put<float>(x.x).template put<float>(x.y).template put<float>(x.z);
            else if constexpr (std::is_same_v<T, I64>)         b.put<u8>(10).put<int64_t>(x.v);
        }, v);
    }
    return b.d;
}

// CALL_FUNCTION (0x01). netID -1 = global function, otherwise the player it is about; delay in ms
inline void call(ENetPeer *peer, std::initializer_list<Var> vars, i32 netID = -1, i32 delay = 0)
{
    GamePacket h; h.type = 0x01; h.netID = netID; h.itemID = delay;
    send_packet(peer, h, varlist(vars));
}

// reads a variant list back (strings and numbers only), for tests and the occasional client call
inline std::vector<std::string> read_varlist(const u8 *p, size_t n)
{
    std::vector<std::string> out; Rd r(p, n);
    int count = r.get<u8>();
    for (int k = 0; k < count && r.ok; ++k)
    {
        size_t idx = r.get<u8>(); u8 t = r.get<u8>();
        if (out.size() <= idx) out.resize(idx + 1);
        switch (t)
        {
            case 1: out[idx] = std::to_string(r.get<float>()); break;
            case 2: out[idx] = r.str32(); break;
            case 3: r.skip(8); break;
            case 4: r.skip(12); break;
            case 5: out[idx] = std::to_string(r.get<u32>()); break;
            case 9: out[idx] = std::to_string(r.get<i32>()); break;
            case 10: out[idx] = std::to_string(r.get<int64_t>()); break;
            default: r.ok = false;
        }
    }
    return out;
}

// proton's HashString (used for items.dat and the ping challenge)
inline u32 hash_string(const u8 *s, size_t n)
{
    u32 h = 0x55555555;
    for (size_t i = 0; i < n; ++i) h = ((h >> 27) | (h << 5)) + s[i];
    return h;
}

// "key|value\n" text -> lookup (a leading '|' on a line is tolerated: the client writes "\n|text|x")
inline std::string field(const std::string &text, const std::string &key)
{
    size_t pos = 0;
    while (pos < text.size())
    {
        size_t e = text.find('\n', pos); if (e == std::string::npos) e = text.size();
        std::string line = text.substr(pos, e - pos);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.empty() && line[0] == '|') line.erase(0, 1);
        size_t bar = line.find('|');
        if (bar != std::string::npos && line.compare(0, bar, key) == 0 && bar == key.size()) return line.substr(bar + 1);
        pos = e + 1;
    }
    return {};
}
