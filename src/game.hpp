// shared server state and helpers
#pragma once
#include "https.hpp"
#include <functional>
#include <set>

struct Player
{
    ENetPeer *peer = nullptr;
    Account *acct = nullptr;
    int stage = 0;                    // 0 connected, 1 redirected (login server done), 2 logged in
    int client_proto = 0;
    i32 netid = 0;
    std::string world;                // current world name, empty = world select menu
    float x = 0, y = 0; bool left = false;
    i64 joined_at = 0;
    std::string pending_uuid;
    i32 rest = -1;                    // tile index of the checkpoint to respawn at, -1 = the white door
    u32 state = 0;                    // character state bits sent in 0x14 (bit 0 = ghost form: walk through blocks)
    bool ghost = false;               // moderators: unseen in worlds, counts and friend lists
    // trading: a pair of players point at each other once both agreed
    i32 trade_req = 0;                // netID of the player we asked to trade with
    Player *trade_with = nullptr; std::map<int, int> offer; bool accepted = false, key_ok = false, trade_open = false; // trade_open: this player's trade window is showing // key_ok: confirmed the World Key warning
    std::string name() const { return acct ? acct->name : "?"; }
};

struct Server
{
    Config cfg;
    Store store;
    Items items;
    ENetHost *host = nullptr;
    std::map<std::string, World> worlds;
    std::vector<Player *> players;
    std::map<std::string, i32> next_netid; // per world

    Player *player_of(ENetPeer *p) { return (Player *)p->data; }
    std::vector<Player *> in_world(const std::string &w) { std::vector<Player *> v; for (Player *p : players) if (p->stage == 2 && p->world == w) v.push_back(p); return v; }
    World *world_of(Player *p) { auto it = worlds.find(p->world); return it == worlds.end() ? nullptr : &it->second; }
    int online() { int n = 0; for (Player *p : players) if (p->stage == 2 && !p->ghost) ++n; return n; }
};
inline Server S;

// ---- small senders
// a sound that comes from player `at` (quieter with distance), played for `to`
inline void sfx_at(Player *to, Player *at, const std::string &file) { call(to->peer, {"OnPlayPositioned", file}, at->netid); }
inline void sfx(Player *p, const std::string &file) { send_text(p->peer, GAME_MSG, "action|play_sfx\nfile|" + file + "\ndelayMS|0\n"); }
inline void console(Player *p, const std::string &msg) { call(p->peer, {"OnConsoleMessage", msg}); }
inline void bubble(Player *p, Player *about, const std::string &msg) { call(p->peer, {"OnTalkBubble", U32{(u32)about->netid}, msg, U32{0}, U32{0}}); }
inline void send_gems(Player *p)
{
    Var gems = S.cfg.protocol >= 227 ? Var(I64{p->acct->gems}) : Var((i32)p->acct->gems);
    call(p->peer, {"OnSetBux", gems, 1, 1});
}

inline std::vector<u8> inventory_blob(Account &a)
{
    Buf b; b.put<u8>(1).put<u32>(a.backpack).put<u16>(a.inv.size());
    for (auto &[id, n] : a.inv)
    {
        bool worn = false; for (u16 c : a.clothes) if (c == id && id) worn = true;
        b.put<u16>(id).put<u8>(std::min<int>(n, 255)).put<u8>(worn ? 1 : 0);
    }
    return b.d;
}
inline void send_inventory(Player *p)
{
    GamePacket h; h.type = 0x09; h.netID = p->netid;
    send_packet(p->peer, h, inventory_blob(*p->acct));
}

// MODIFY_ITEM_INVENTORY: jumpCount = amount to remove
inline void send_remove(Player *p, int id, int n) { GamePacket h; h.type = 0x0d; h.itemID = id; h.jumpCount = std::min(n, 255); send_packet(p->peer, h); }

inline int inv_count(Account &a, int id) { auto it = a.inv.find(id); return it == a.inv.end() ? 0 : it->second; }
inline void inv_add(Account &a, int id, int n) { a.inv[id] = std::min(200, inv_count(a, id) + n); }
inline void inv_remove(Account &a, int id, int n) { int c = inv_count(a, id) - n; if (c <= 0) a.inv.erase(id); else a.inv[id] = c; }

// ---- dialogs (docs: guide/dialog-elements)
struct Dialog
{
    std::string s = "set_default_color|`o\n";
    Dialog &label_icon(const std::string &t, int icon, const char *size = "big") { s += std::string("add_label_with_icon|") + size + "|" + t + "|left|" + std::to_string(icon) + "|\n"; return *this; }
    Dialog &text(const std::string &t) { s += "add_textbox|" + t + "|left|\n"; return *this; }
    Dialog &spacer() { s += "add_spacer|small|\n"; return *this; }
    Dialog &input(const std::string &name, const std::string &label, const std::string &def, int maxlen) { s += "add_text_input|" + name + "|" + label + "|" + def + "|" + std::to_string(maxlen) + "|\n"; return *this; }
    Dialog &hidden(const std::string &name, const std::string &v) { s += "embed_data|" + name + "|" + v + "\n"; return *this; }
    Dialog &quick_exit() { s += "add_quick_exit|\n"; return *this; }
    std::string end(const std::string &name, const std::string &cancel, const std::string &ok) { return s + "end_dialog|" + name + "|" + cancel + "|" + ok + "|\n"; }
};
inline void dialog(Player *p, const std::string &text) { call(p->peer, {"OnDialogRequest", text}); }
