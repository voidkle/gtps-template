// gtbot: a scriptable Growtopia client for the private server. Bots log in through the real flow (login page -> ltoken ->
// OnSendToServer -> full login -> enter_game) and are driven from a Lua script.
//   ./gtbot.out script.lua [items.dat]          (the Lua API is described in docs/guide/gtbot.md and in the examples/ folder)
// Every call that waits (connect, join, walk, break_tile, sleep ...) keeps all bots serviced, so several bots can live in one script.
#include "proto.hpp"
#include "items.hpp"
#include <cstdio>
#include <cstdlib>
#include <map>
#include <cmath>
#include <cstring>
#include <thread>
#include <chrono>
#include <functional>
extern "C" {
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
}

static Items g_items;
static const int T_BACKGROUND = 18; // item action type
struct Cell { u16 fg = 0, bg = 0; };
struct Who { std::string name; float x = 0, y = 0; };

static std::string sh(const std::string &cmd) { std::string out; FILE *f = popen(cmd.c_str(), "r"); char b[4096]; size_t n; while ((n = fread(b, 1, sizeof b, f)) > 0) out.append(b, n); pclose(f); return out; }
static std::string clean(const std::string &s) { std::string o; for (size_t i = 0; i < s.size(); ++i) { if (s[i] == '`') { ++i; continue; } o += s[i]; } return o; }

// the tile extras this server writes (the same layouts as src/extras.hpp), so the map can be walked to its end
static bool skip_extra(Rd &r, int t, int ver, int item)
{
    auto z = [&](int n = 1) { r.skip(4 * n); }; auto s = [&] { r.str16(); };
    switch (t)
    {
        case 0x01: s(); r.skip(1); break;
        case 0x02: s(); z(); break;
        case 0x03: r.skip(1); z(); { u32 k = r.get<u32>(); r.skip(4 * k); } if (ver >= 12) z(); if (ver >= 13) z(); break;
        case 0x04: z(); r.skip(1); break;
        case 0x06: case 0x07: case 0x0c: case 0x0d: s(); s(); s(); r.skip(1); break;
        case 0x08: r.skip(1); break;
        case 0x09: z(); if (item == 5318 || (item == 10656 && ver > 16)) z(); break;
        case 0x0a: z(); r.skip(1); break;
        case 0x0b: z(); s(); break;
        case 0x0e: s(); r.skip(1); z(); r.skip(18); break;
        case 0x0f: case 0x1b: case 0x26: case 0x27: case 0x32: case 0x39: z(); break;
        case 0x10: r.skip(1); break;
        case 0x12: case 0x1a: r.skip(1); z(); break;
        case 0x13: r.skip(18); break;
        case 0x14: case 0x36: case 0x40: s(); break;
        case 0x15: s(); z(); r.skip(1); break;
        case 0x17: z(); break;
        case 0x18: z(2); break;
        case 0x19: r.skip(1); z(); break;
        case 0x1c: r.skip(1); z(); r.skip(1); break;
        case 0x1e: r.skip(1); z(); break;
        case 0x1f: r.skip(1); s(); z(3); r.skip(1); if (item == 0 || item == 3316) z(4); break;
        case 0x20: z(); break;
        case 0x21: if (item == 0 || item == 3394) s(); break;
        case 0x23: z(); s(); break;
        case 0x24: s(); z(3); break;
        case 0x25: s(); z(2); break;
        case 0x28: z(); break;
        case 0x29: z(); break;
        case 0x2a: r.skip(1); if (ver > 18) z(3); if (ver > 19) z(2); break;
        case 0x2b: z(4); break;
        case 0x2c: r.skip(1); z(2); break;
        case 0x2f: s(); z(); r.skip(1); break;
        case 0x30: s(); z(4); r.skip(6); if (ver > 8) z(); break;
        case 0x31: z(2); r.skip(1); break;
        case 0x35: z(2); break;
        case 0x51: z(2); break;
        case 0x37: z(3); if (ver > 13) r.skip(8); break;
        case 0x38: s(); z(); break;
        case 0x3c: z(); r.skip(1); break;
        case 0x3d: z(); r.skip(2); r.skip(28); r.skip(1); break;
        case 0x3e: z(2); r.skip(2); z(); break;
        case 0x3f: z(3); break;
        case 0x42: r.skip(1); break;
        case 0x43: case 0x4d: z(2); break;
        case 0x44: z(); s(); s(); z(); break;
        case 0x45: case 0x46: case 0x47: z(3); if (ver > 7) z(); if (t == 0x47) { z(2); r.skip(2); z(2); } break;
        case 0x48: z(3); break;
        case 0x49: case 0x4e: case 0x4f: z(); break;
        case 0x4b: z(); s(); break;
        case 0x4c: z(78); break;
        case 0x50: z(2); break;
        case 0x05: case 0x11: case 0x16: case 0x1d: case 0x22: case 0x2d: case 0x2e: case 0x33: case 0x34: case 0x3a: case 0x3b: case 0x41: case 0x4a: case 0x52: break;
        default: return false;
    }
    return r.ok;
}

struct Bot
{
    lua_State *L = nullptr;
    std::string name, pw, host = "127.0.0.1"; int https = 443, port = 17091;
    ENetHost *eh = nullptr; ENetPeer *peer = nullptr;
    int stage = 0;                      // 0 = connecting, 1 = redirected (waiting for the new HELLO), 2 = full login sent, 3 = in the world menu
    std::string token, uuid, ltoken;
    bool connected = false, in_world = false, failed = false; std::string fail_msg;
    int netid = -1; float x = 0, y = 0;
    std::string world; int w = 0, h = 0; std::vector<Cell> tiles; bool partial = false;
    std::map<int, Who> players;         // other players by netID
    std::map<int, int> inv; i64 gems = 0;
    std::map<std::string, int> handlers; // event name -> Lua registry reference
    int self_ref = LUA_NOREF;

    void connect_to(const std::string &ip, int p)
    {
        if (eh) enet_host_destroy(eh);
        eh = enet_host_create(ENET_ADDRESS_TYPE_IPV4, nullptr, 1, 2, 0, 0); eh->usingNewPacket = 1; eh->checksum = enet_crc32; enet_host_compress_with_range_coder(eh);
        ENetAddress a{.type = ENET_ADDRESS_TYPE_IPV4, .port = (enet_uint16)p}; enet_address_set_host_ip(&a, ip.c_str()); peer = enet_host_connect(eh, &a, 2, 0);
    }
    void text(int type, const std::string &t) { Buf b; b.put<u32>(type).raw(t.data(), t.size()).put<u8>(0); send_raw(peer, b.d.data(), b.d.size()); }

    // ---- events: handler(bot, ...)
    void fire(const std::string &ev, const std::vector<std::string> &args = {}, const std::vector<double> &nums = {})
    {
        auto it = handlers.find(ev); if (it == handlers.end()) return;
        lua_rawgeti(L, LUA_REGISTRYINDEX, it->second); lua_rawgeti(L, LUA_REGISTRYINDEX, self_ref);
        for (auto &s : args) lua_pushlstring(L, s.data(), s.size());
        for (double d : nums) lua_pushnumber(L, d);
        if (lua_pcall(L, 1 + args.size() + nums.size(), 0, 0)) { fprintf(stderr, "[%s] handler for '%s': %s\n", name.c_str(), ev.c_str(), lua_tostring(L, -1)); lua_pop(L, 1); }
    }

    // ---- the world, as the client reads it
    void parse_world(const u8 *d, size_t n)
    {
        Rd r(d, n); tiles.clear(); partial = false;
        u16 version = r.get<u16>(); r.get<u32>(); world = r.str16(); w = r.get<i32>(); h = r.get<i32>(); u32 count = r.get<u32>(); r.skip(5);
        for (u32 i = 0; i < count && r.ok; ++i)
        {
            Cell c; c.fg = r.get<u16>(); c.bg = r.get<u16>(); r.get<u16>(); u16 fl = r.get<u16>();
            if (fl & 2) r.get<u16>();
            bool ok = true;
            if (fl & 1)
            {
                u8 t = r.get<u8>();
                ok = skip_extra(r, t, version, c.fg);
                if (!ok) fprintf(stderr, "[gtbot] unknown tile extra 0x%02x at tile %u (fg %u)\n", t, i, c.fg);
            }
            if (!r.ok) break;
            tiles.push_back(c);
            if (!ok) { partial = true; break; }
        }
        if (tiles.size() != count) { partial = true; tiles.resize(count); }
    }
    Cell *tile(int tx, int ty) { return tx >= 0 && ty >= 0 && tx < w && ty < h && (size_t)(ty * w + tx) < tiles.size() ? &tiles[ty * w + tx] : nullptr; }

    // ---- incoming
    void on_message(const u8 *d, size_t n)
    {
        u32 type; if (n < 4) return; memcpy(&type, d, 4);
        static const bool trace = getenv("GTBOT_TRACE") != nullptr; // GTBOT_TRACE=1: print every message the server sends, for comparing with a capture
        if (trace)
        {
            static const auto t0 = std::chrono::steady_clock::now(); double ts = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
            if (type == 4 && n >= 60) { GamePacket q; memcpy(&q, d + 4, sizeof q); if (q.type == 0x01) { auto v = read_varlist(d + 60, n - 60); std::string a; for (size_t i = 0; i < v.size(); ++i) a += (i ? ", " : "") + v[i].substr(0, 90); printf("TRACE %8.3f CALL netID=%d delay=%d %s\n", ts, q.netID, q.itemID, a.c_str()); } else printf("TRACE %8.3f GAME 0x%02x flags=0x%x item=%d tile=(%d,%d) extra=%zu\n", ts, q.type, q.flags, q.itemID, q.tileX, q.tileY, n - 60); }
            else printf("TRACE %8.3f MSG type %u, %zu bytes %.60s\n", ts, type, n - 4, type == 3 || type == 2 ? (const char *)d + 4 : "");
        }
        if (type == 1) { if (stage == 0) text(2, "protocol|227\nltoken|" + ltoken + "\nplatformID|0\n"); else if (stage == 1) { stage = 2; text(2, "tankIDName|" + name + "\nprotocol|227\ngame_version|5.6\nuser|" + user + "\ntoken|" + tok2 + "\nUUIDToken|" + uuid + "\ndoorID|0\nlmode|1\n"); } return; }
        if (type == 6 || type == 5 || type == 3 || type == 2) return;
        if (type != 4 || n < 60) return;
        GamePacket pk; memcpy(&pk, d + 4, sizeof pk); const u8 *ex = d + 60; size_t en = n - 60;
        switch (pk.type)
        {
            case 0x01: { auto v = read_varlist(ex, en); if (!v.empty()) variant(v); break; }
            case 0x04: parse_world(ex, en); in_world = true; break;
            case 0x03:
            {
                Cell *c = tile(pk.tileX, pk.tileY); if (!c) break;
                if (pk.itemID == 18) { if (c->fg) c->fg = 0; else c->bg = 0; }
                else if (pk.itemID != 32) { const Item *it = g_items.get(pk.itemID); if (it && it->type == T_BACKGROUND) c->bg = pk.itemID; else if (it && it->type != 0 && pk.itemID != 18) c->fg = pk.itemID; }
                else break;
                fire("tile", {}, {(double)pk.tileX, (double)pk.tileY, (double)c->fg, (double)c->bg, (double)pk.netID});
                break;
            }
            case 0x00: { auto it = players.find(pk.netID); if (it != players.end()) { it->second.x = pk.posX; it->second.y = pk.posY; } break; }
            case 0x09:
            {
                Rd r(ex, en); r.get<u8>(); r.get<u32>(); u16 k = r.get<u16>(); inv.clear();
                for (u16 i = 0; i < k && r.ok; ++i) { u16 id = r.get<u16>(); u8 c = r.get<u8>(); r.get<u8>(); inv[id] = c; }
                fire("inventory"); break;
            }
            case 0x0d: if (pk.buildRange > 0) inv[pk.itemID] += pk.buildRange; else if (pk.jumpCount > 0) { inv[pk.itemID] -= pk.jumpCount; if (inv[pk.itemID] <= 0) inv.erase(pk.itemID); } break;
            case 0x1d: if (pk.netID == netid) { x = pk.posX; y = pk.posY; } break;
            default: break;
        }
    }
    std::string user, tok2;
    void variant(const std::vector<std::string> &v)
    {
        const std::string &fn = v[0];
        if (fn == "OnSendToServer" && v.size() > 4)
        {
            uuid = v[4].substr(v[4].rfind('|') + 1); user = v[3]; tok2 = v[2]; stage = 1; connect_to(host, atoi(v[1].c_str())); return;
        }
        if (fn.rfind("OnSuperMainStartAcceptLogon", 0) == 0) { text(2, "action|enter_game\n"); return; }
        if (fn == "OnRequestWorldSelectMenu") { if (stage < 3) { stage = 3; connected = true; fire("login"); } in_world = false; return; }
        if (fn == "OnSpawn" && v.size() > 1)
        {
            const std::string &s = v[1]; auto get = [&](const char *k) { size_t q = s.find(std::string("\n") + k + "|"); if (q == std::string::npos) return std::string(); q += strlen(k) + 2; return s.substr(q, s.find('\n', q) - q); };
            int id = atoi(get("netID").c_str()); float px = 0, py = 0; std::string xy = get("posXY"); sscanf(xy.c_str(), "%f|%f", &px, &py);
            if (s.find("type|local") != std::string::npos) { netid = id; x = px; y = py; return; }
            Who q; q.name = clean(get("name")); q.x = px; q.y = py; players[id] = q; fire("spawn", {q.name}, {(double)id}); return;
        }
        if (fn == "OnRemove" && v.size() > 1) { int id = atoi(field(v[1], "netID").c_str()); auto it = players.find(id); std::string nm = it != players.end() ? it->second.name : ""; players.erase(id); fire("remove", {nm}, {(double)id}); return; }
        if (fn == "OnSetPos" && v.size() > 1) return;
        if (fn == "OnSetBux" && v.size() > 1) gems = atoll(v[1].c_str());
        if (fn == "OnFailedToEnterWorld") { failed = true; fail_msg = "failed to enter the world"; }
        if (fn == "OnConsoleMessage" && v.size() > 1) { if (v[1].find("inaccessible") != std::string::npos) { failed = true; fail_msg = clean(v[1]); } fire("console", {clean(v[1])}); }
        else if (fn == "OnTalkBubble" && v.size() > 2) fire("talk", {clean(v[2])}, {(double)atoi(v[1].c_str())});
        else if (fn == "OnDialogRequest" && v.size() > 1) fire("dialog", {v[1]});
        std::vector<std::string> rest(v.begin() + 1, v.end()); rest.insert(rest.begin(), fn); fire("variant", rest);
    }

    void service()
    {
        if (!eh) return; ENetEvent ev;
        while (enet_host_service(eh, &ev, 0) > 0)
        {
            if (ev.type == ENET_EVENT_TYPE_DISCONNECT) { if (stage == 1) continue; connected = false; in_world = false; fire("disconnect"); }
            else if (ev.type == ENET_EVENT_TYPE_RECEIVE) { on_message(ev.packet->data, ev.packet->dataLength); enet_packet_destroy(ev.packet); }
        }
    }
};

static std::vector<Bot *> g_bots;
static void pump(int ms) { auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms); do { for (Bot *b : g_bots) b->service(); std::this_thread::sleep_for(std::chrono::milliseconds(5)); } while (std::chrono::steady_clock::now() < end); }
static bool wait_until(const std::function<bool()> &f, int ms) { auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms); while (std::chrono::steady_clock::now() < end) { for (Bot *b : g_bots) b->service(); if (f()) return true; std::this_thread::sleep_for(std::chrono::milliseconds(5)); } return f(); }

// ---------------------------------------------------------------- Lua binding
static Bot *self(lua_State *L) { return *(Bot **)luaL_checkudata(L, 1, "Bot"); }
static int fail(lua_State *L, const std::string &m) { lua_pushnil(L); lua_pushstring(L, m.c_str()); return 2; }

static int l_new(lua_State *L)
{
    luaL_checktype(L, 1, LUA_TTABLE);
    Bot *b = new Bot; b->L = L;
    auto str = [&](const char *k, std::string &dst) { lua_getfield(L, 1, k); if (lua_isstring(L, -1)) dst = lua_tostring(L, -1); lua_pop(L, 1); };
    auto num = [&](const char *k, int &dst) { lua_getfield(L, 1, k); if (lua_isnumber(L, -1)) dst = (int)lua_tonumber(L, -1); lua_pop(L, 1); };
    str("name", b->name); str("password", b->pw); str("host", b->host); num("https", b->https); num("port", b->port);
    if (b->name.empty()) { delete b; return luaL_error(L, "Bot.new needs name and password"); }
    *(Bot **)lua_newuserdata(L, sizeof(Bot *)) = b; luaL_setmetatable(L, "Bot");
    lua_pushvalue(L, -1); b->self_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    g_bots.push_back(b); return 1;
}

static int l_connect(lua_State *L)
{
    Bot *b = self(L); b->stage = 0; b->connected = false;
    std::string json = sh("curl -skL https://" + b->host + ":" + std::to_string(b->https) + "/player/growid/login/validate --data-urlencode 'growId=" + b->name + "' --data-urlencode 'password=" + b->pw + "' 2>/dev/null");
    size_t k = json.find("\"token\":\""); if (k == std::string::npos) return fail(L, "login page refused: " + json.substr(0, 200));
    b->ltoken = json.substr(k + 9, json.find('"', k + 9) - k - 9);
    b->connect_to(b->host, b->port);
    if (!wait_until([&] { return b->connected; }, 15000)) return fail(L, "login timed out");
    lua_pushboolean(L, 1); return 1;
}
static int l_disconnect(lua_State *L) { Bot *b = self(L); if (b->peer) enet_peer_disconnect_now(b->peer, 0); b->connected = false; pump(100); return 0; }
static int l_connected(lua_State *L) { lua_pushboolean(L, self(L)->connected); return 1; }

static int l_join(lua_State *L)
{
    Bot *b = self(L); std::string w = luaL_checkstring(L, 2);
    if (!b->connected) return fail(L, "not logged in");
    b->in_world = false; b->failed = false; b->players.clear(); b->tiles.clear(); b->netid = -1;
    b->text(3, "action|join_request\nname|" + w + "\ninvitedWorld|0\n");
    if (!wait_until([&] { return (b->in_world && b->netid >= 0) || b->failed; }, 10000)) return fail(L, "join timed out");
    if (b->failed) return fail(L, b->fail_msg);
    pump(300); lua_pushboolean(L, 1); return 1;
}
static int l_leave(lua_State *L) { Bot *b = self(L); b->text(3, "action|quit_to_exit\n"); b->in_world = false; pump(200); return 0; }
static int l_say(lua_State *L) { Bot *b = self(L); b->text(2, std::string("action|input\n|text|") + luaL_checkstring(L, 2) + "\n"); return 0; }
static int l_send(lua_State *L) { Bot *b = self(L); b->text((int)luaL_checkinteger(L, 2), luaL_checkstring(L, 3)); return 0; } // raw: bot:send(2, "action|...")

static int l_packet(lua_State *L) // bot:packet{type=3, item=18, tile_x=.., tile_y=.., x=.., y=.., flags=..}
{
    Bot *b = self(L); luaL_checktype(L, 2, LUA_TTABLE); GamePacket p; p.netID = b->netid; p.posX = b->x; p.posY = b->y;
    auto f = [&](const char *k, double &out) { lua_getfield(L, 2, k); bool ok = lua_isnumber(L, -1); if (ok) out = lua_tonumber(L, -1); lua_pop(L, 1); return ok; };
    double v;
    if (f("type", v)) p.type = (u8)v; if (f("item", v)) p.itemID = (i32)v; if (f("tile_x", v)) p.tileX = (i32)v; if (f("tile_y", v)) p.tileY = (i32)v;
    if (f("x", v)) p.posX = (float)v; if (f("y", v)) p.posY = (float)v; if (f("flags", v)) p.flags = (i32)v; if (f("net_id", v)) p.netID = (i32)v; if (f("target", v)) p.targetNetID = (i32)v;
    send_packet(b->peer, p); return 0;
}

static void state(Bot *b, float x, float y, int flags) { GamePacket p; p.type = 0; p.netID = b->netid; p.posX = x; p.posY = y; p.flags = flags; send_packet(b->peer, p); b->x = x; b->y = y; }

static int l_walk(lua_State *L) // bot:walk(tile_x, tile_y [, ms_per_step=90])
{
    Bot *b = self(L); float tx = luaL_checknumber(L, 2) * 32, ty = luaL_checknumber(L, 3) * 32; int step_ms = (int)luaL_optinteger(L, 4, 90);
    if (!b->in_world) return fail(L, "not in a world");
    int dir = tx < b->x ? 0x10 : 0x20;
    while (fabsf(b->x - tx) > 0.5f || fabsf(b->y - ty) > 0.5f)
    {
        float dx = tx - b->x, dy = ty - b->y, d = sqrtf(dx * dx + dy * dy), s = std::min(d, 24.f);
        state(b, b->x + dx / d * s, b->y + dy / d * s, dir); pump(step_ms);
    }
    lua_pushboolean(L, 1); return 1;
}
static int l_teleport(lua_State *L) { Bot *b = self(L); state(b, luaL_checknumber(L, 2) * 32, luaL_checknumber(L, 3) * 32, 0x20); pump(50); return 0; } // a state packet with no steps in between
static int l_position(lua_State *L) { Bot *b = self(L); lua_pushnumber(L, floor(b->x / 32 + 0.5)); lua_pushnumber(L, floor(b->y / 32 + 0.5)); return 2; }

static void act(Bot *b, int item, int tx, int ty) { GamePacket p; p.type = 0x03; p.netID = b->netid; p.itemID = item; p.tileX = tx; p.tileY = ty; p.posX = b->x; p.posY = b->y; p.flags = 0x20; send_packet(b->peer, p); }
static int l_punch(lua_State *L) { Bot *b = self(L); act(b, 18, (int)luaL_checkinteger(L, 2), (int)luaL_checkinteger(L, 3)); return 0; }
static int l_place(lua_State *L) { Bot *b = self(L); act(b, (int)luaL_checkinteger(L, 4), (int)luaL_checkinteger(L, 2), (int)luaL_checkinteger(L, 3)); return 0; }
static int l_wrench(lua_State *L) { Bot *b = self(L); act(b, 32, (int)luaL_checkinteger(L, 2), (int)luaL_checkinteger(L, 3)); return 0; }

static int l_break(lua_State *L) // bot:break_tile(x, y [, interval_ms=600 [, max_punches=60]]): punches until both layers are gone
{
    Bot *b = self(L); int tx = (int)luaL_checkinteger(L, 2), ty = (int)luaL_checkinteger(L, 3), gap = (int)luaL_optinteger(L, 4, 600), max = (int)luaL_optinteger(L, 5, 60);
    for (int i = 0; i < max; ++i)
    {
        Cell *c = b->tile(tx, ty); if (!c) return fail(L, "outside the world");
        if (!c->fg && !c->bg) { lua_pushboolean(L, 1); return 1; }
        act(b, 18, tx, ty); pump(gap);
    }
    Cell *c = b->tile(tx, ty); lua_pushboolean(L, c && !c->fg && !c->bg); return 1;
}

static int l_tile(lua_State *L) { Bot *b = self(L); Cell *c = b->tile((int)luaL_checkinteger(L, 2), (int)luaL_checkinteger(L, 3)); if (!c) return fail(L, "outside the world"); lua_pushinteger(L, c->fg); lua_pushinteger(L, c->bg); return 2; }
static int l_world(lua_State *L) { Bot *b = self(L); lua_pushstring(L, b->world.c_str()); lua_pushinteger(L, b->w); lua_pushinteger(L, b->h); lua_pushboolean(L, b->partial); return 4; }
static int l_inventory(lua_State *L) { Bot *b = self(L); lua_newtable(L); for (auto &[id, n] : b->inv) { lua_pushinteger(L, id); lua_pushinteger(L, n); lua_settable(L, -3); } return 1; }
static int l_count(lua_State *L) { Bot *b = self(L); auto it = b->inv.find((int)luaL_checkinteger(L, 2)); lua_pushinteger(L, it == b->inv.end() ? 0 : it->second); return 1; }
static int l_gems(lua_State *L) { lua_pushinteger(L, self(L)->gems); return 1; }
static int l_name(lua_State *L) { lua_pushstring(L, self(L)->name.c_str()); return 1; }
static int l_netid(lua_State *L) { lua_pushinteger(L, self(L)->netid); return 1; }
static int l_players(lua_State *L)
{
    Bot *b = self(L); lua_newtable(L); int i = 1;
    for (auto &[id, q] : b->players) { lua_newtable(L); lua_pushinteger(L, id); lua_setfield(L, -2, "netid"); lua_pushstring(L, q.name.c_str()); lua_setfield(L, -2, "name"); lua_pushnumber(L, q.x / 32); lua_setfield(L, -2, "x"); lua_pushnumber(L, q.y / 32); lua_setfield(L, -2, "y"); lua_rawseti(L, -2, i++); }
    return 1;
}
static int l_item(lua_State *L) // item("World Lock") -> id, or item(242) -> name
{
    if (lua_isnumber(L, 1)) { const Item *it = g_items.get((int)lua_tointeger(L, 1)); if (!it) return fail(L, "no such item"); lua_pushstring(L, it->name.c_str()); return 1; }
    std::string want = luaL_checkstring(L, 1); for (char &c : want) c = tolower(c);
    for (const Item &it : g_items.list) { std::string n = it.name; for (char &c : n) c = tolower(c); if (n == want) { lua_pushinteger(L, it.id); return 1; } }
    return fail(L, "no such item");
}
static int l_on(lua_State *L) { Bot *b = self(L); std::string ev = luaL_checkstring(L, 2); luaL_checktype(L, 3, LUA_TFUNCTION); lua_pushvalue(L, 3); b->handlers[ev] = luaL_ref(L, LUA_REGISTRYINDEX); return 0; }
static int l_wait(lua_State *L) { pump((int)luaL_checkinteger(L, 2)); return 0; }
static int l_sleep(lua_State *L) { pump((int)luaL_checkinteger(L, 1)); return 0; }
static int l_clock(lua_State *L) { lua_pushnumber(L, std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count()); return 1; }

int main(int argc, char **argv)
{
    if (argc < 2) { puts("usage: gtbot.out script.lua [items.dat]"); return 1; }
    setvbuf(stdout, nullptr, _IOLBF, 0); enet_initialize();
    if (!g_items.load(argc > 2 ? argv[2] : "../items.dat") && !g_items.load("items.dat")) puts("(no items.dat found: item() and block placement by type are limited; pass the path as the second argument)");
    lua_State *L = luaL_newstate(); luaL_openlibs(L);
    luaL_newmetatable(L, "Bot");
    static const luaL_Reg methods[] = {
        {"connect", l_connect}, {"disconnect", l_disconnect}, {"connected", l_connected}, {"join", l_join}, {"leave", l_leave}, {"say", l_say}, {"send", l_send}, {"packet", l_packet},
        {"walk", l_walk}, {"teleport", l_teleport}, {"position", l_position}, {"punch", l_punch}, {"place", l_place}, {"wrench", l_wrench}, {"break_tile", l_break}, {"tile", l_tile},
        {"world", l_world}, {"inventory", l_inventory}, {"count", l_count}, {"gems", l_gems}, {"name", l_name}, {"netid", l_netid}, {"players", l_players}, {"on", l_on}, {"wait", l_wait}, {nullptr, nullptr}};
    luaL_newlib(L, methods); lua_setfield(L, -2, "__index"); lua_pop(L, 1);
    lua_newtable(L); lua_pushcfunction(L, l_new); lua_setfield(L, -2, "new"); lua_setglobal(L, "Bot");
    lua_pushcfunction(L, l_sleep); lua_setglobal(L, "sleep"); lua_pushcfunction(L, l_item); lua_setglobal(L, "item"); lua_pushcfunction(L, l_clock); lua_setglobal(L, "clock");
    int rc = luaL_dofile(L, argv[1]);
    if (rc) { fprintf(stderr, "%s\n", lua_tostring(L, -1)); }
    for (Bot *b : g_bots) if (b->peer) enet_peer_disconnect_now(b->peer, 0);
    pump(200);
    return rc ? 1 : 0;
}
