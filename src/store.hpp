// accounts, login tokens (docs: guide/official-login steps 2-4)
#pragma once
#include "world.hpp"
#include <map>
#include <set>
#include <mutex>
#include <sstream>
#include <openssl/evp.h>
#include <openssl/rand.h>

inline std::string hex(const u8 *p, size_t n) { static const char *d = "0123456789abcdef"; std::string s; for (size_t i = 0; i < n; ++i) { s += d[p[i] >> 4]; s += d[p[i] & 15]; } return s; }
inline std::string random_hex(size_t bytes) { std::vector<u8> b(bytes); RAND_bytes(b.data(), bytes); return hex(b.data(), bytes); }

struct Account
{
    std::string name, salt, hash;
    i32 user_id = 0;
    i64 gems = 0;
    int role = 0;                       // 0 player, 3 admin
    std::map<u16, u16> inv;             // item id -> amount
    u16 clothes[10] = {};               // hair shirt pants feet face hand back mask necklace ances
    u32 skin = 0x8295C3FF;
    u16 backpack = 16;                  // inventory slots
    std::vector<std::string> my_worlds;
    std::string home;                   // /sethome
    std::set<i32> friends;              // user ids
    u16 level = 1; u32 xp = 0;          // xp towards the next level
    i64 played = 0;                     // seconds played before this session
    i64 ban_until = 0;                  // unix seconds, -1 = permanent, 0 = not banned
    std::string ban_reason;
    std::string notes[5];               // notebook pages (raw text, <CR> for line breaks)
    std::vector<std::string> favs;      // favorite worlds, at most 10
    u16 presets[4][10] = {};            // wardrobe presets, one clothing slot each
    int preset_count = 2;
    int bb_item = 0, bb_show = 0, bb_buying = 0, bb_price = 0, bb_peritem = 1; // trade billboard
    int status = 0;                     // 0 online, 1 busy, 2 away
    std::set<int> seen_planted, seen_found; // seed diary
    std::vector<std::string> trades;    // trade history lines, newest last
    i64 last_seen = 0;
    i64 quest_day = 0;                  // day number of the last daily quest handed in
    i64 created = 0;
};

inline std::string clean_name(std::string s) // GrowIDs: letters and digits, 3..18
{
    std::string o; for (char c : s) if (isalnum((unsigned char)c)) o += c; return o;
}
inline bool is_banned(const Account &a) { return a.ban_until == -1 || a.ban_until > now_s(); }
inline std::string ban_text(const Account &a)
{
    std::string t = "`4Your account is banned";
    if (a.ban_until == -1) t += " permanently"; else t += " for another " + std::to_string((a.ban_until - now_s() + 3599) / 3600) + " hour(s)";
    return t + (a.ban_reason.empty() ? "." : ": " + a.ban_reason) + "``";
}
inline std::string lower(std::string s) { for (char &c : s) c = tolower((unsigned char)c); return s; }
inline std::string upper(std::string s) { for (char &c : s) c = toupper((unsigned char)c); return s; }

struct Store
{
    std::string dir = "data";
    std::mutex mu;
    std::map<std::string, Account> accounts; // key = lowercase name
    std::map<i32, std::string> names;         // user id -> name, for every account on disk
    struct Tok { std::string name; i64 expires; };
    std::map<std::string, Tok> web_tokens;    // issued by the login page
    struct Transfer { std::string name; i64 expires; std::string uuid; };
    std::map<i32, Transfer> transfers;        // OnSendToServer tokens, valid 60 s

    static std::string pbkdf2(const std::string &pw, const std::string &salt)
    {
        u8 out[32];
        PKCS5_PBKDF2_HMAC(pw.data(), pw.size(), (const u8 *)salt.data(), salt.size(), 100000, EVP_sha256(), 32, out);
        return hex(out, 32);
    }

    void index_names() // reads user id + name of every saved account
    {
        std::lock_guard g(mu);
        std::error_code ec;
        for (auto &e : std::filesystem::directory_iterator(dir + "/accounts", ec))
        { Account a; if (read(lower(e.path().stem().string()), a)) names[a.user_id] = a.name; }
    }
    std::string name_of(i32 uid) { auto it = names.find(uid); return it == names.end() ? "?" : it->second; } // caller holds mu

    std::string path(const std::string &lname) { return dir + "/accounts/" + lname + ".acc"; }

    void save(const Account &a)
    {
        std::filesystem::create_directories(dir + "/accounts");
        std::ofstream f(path(lower(a.name)), std::ios::trunc);
        f << "name " << a.name << "\nsalt " << a.salt << "\nhash " << a.hash << "\nuser " << a.user_id
          << "\ngems " << a.gems << "\nrole " << a.role << "\nskin " << a.skin << "\nbackpack " << a.backpack << "\ncreated " << a.created << "\nlevel " << a.level << "\nxp " << a.xp << "\nplayed " << a.played << "\nhome " << a.home << "\nstatus " << a.status << "\nlastseen " << a.last_seen << "\nquest " << a.quest_day << "\npresetcount " << a.preset_count << "\nbb " << a.bb_item << ' ' << a.bb_show << ' ' << a.bb_buying << ' ' << a.bb_price << ' ' << a.bb_peritem << "\nban " << a.ban_until << "\nbanreason " << a.ban_reason << "\n";
        f << "clothes"; for (u16 c : a.clothes) f << ' ' << c; f << "\n";
        for (auto &[id, n] : a.inv) f << "item " << id << ' ' << n << "\n";
        for (auto &w : a.my_worlds) f << "world " << w << "\n";
        for (i32 fr : a.friends) f << "friend " << fr << "\n";
        for (int i = 0; i < 5; ++i) if (!a.notes[i].empty()) { std::string t = a.notes[i]; for (char &c : t) if (c == '\n') c = '\x01'; f << "note " << i << ' ' << t << "\n"; }
        for (auto &fv : a.favs) f << "fav " << fv << "\n";
        for (int i = 0; i < 4; ++i) { f << "preset " << i; for (u16 c : a.presets[i]) f << ' ' << c; f << "\n"; }
        for (int id : a.seen_planted) f << "planted " << id << "\n";
        for (int id : a.seen_found) f << "found " << id << "\n";
        for (auto &t : a.trades) f << "trade " << t << "\n";
    }

    bool read(const std::string &lname, Account &a)
    {
        std::ifstream f(path(lname)); if (!f) return false;
        for (std::string line; std::getline(f, line); )
        {
            std::istringstream in(line); std::string k; in >> k;
            if (k == "name") in >> a.name; else if (k == "salt") in >> a.salt; else if (k == "hash") in >> a.hash;
            else if (k == "user") in >> a.user_id; else if (k == "gems") in >> a.gems; else if (k == "role") in >> a.role;
            else if (k == "skin") in >> a.skin; else if (k == "backpack") in >> a.backpack; else if (k == "created") in >> a.created; else if (k == "level") in >> a.level; else if (k == "xp") in >> a.xp; else if (k == "played") in >> a.played; else if (k == "home") in >> a.home; else if (k == "ban") in >> a.ban_until; else if (k == "banreason") std::getline(in >> std::ws, a.ban_reason);
            else if (k == "clothes") for (u16 &c : a.clothes) in >> c;
            else if (k == "item") { int id, n; in >> id >> n; a.inv[id] = n; }
            else if (k == "world") { std::string w; in >> w; a.my_worlds.push_back(w); }
            else if (k == "friend") { i32 fr; in >> fr; a.friends.insert(fr); }
            else if (k == "status") in >> a.status; else if (k == "lastseen") in >> a.last_seen; else if (k == "quest") in >> a.quest_day; else if (k == "presetcount") in >> a.preset_count;
            else if (k == "bb") in >> a.bb_item >> a.bb_show >> a.bb_buying >> a.bb_price >> a.bb_peritem;
            else if (k == "note") { int i = 0; in >> i; std::string t; std::getline(in >> std::ws, t); for (char &c : t) if (c == '\x01') c = '\n'; if (i >= 0 && i < 5) a.notes[i] = t; }
            else if (k == "fav") { std::string w; in >> w; a.favs.push_back(w); }
            else if (k == "preset") { int i = 0; in >> i; if (i >= 0 && i < 4) for (u16 &c : a.presets[i]) in >> c; }
            else if (k == "planted") { int id; in >> id; a.seen_planted.insert(id); } else if (k == "found") { int id; in >> id; a.seen_found.insert(id); }
            else if (k == "trade") { std::string t; std::getline(in >> std::ws, t); a.trades.push_back(t); }
        }
        return true;
    }

    Account *find(const std::string &name) // caller holds mu
    {
        std::string key = lower(name);
        auto it = accounts.find(key);
        if (it != accounts.end()) return &it->second;
        Account a; if (!read(key, a)) return nullptr;
        return &(accounts[key] = a);
    }

    i32 next_user_id()
    {
        std::filesystem::create_directories(dir);
        std::string p = dir + "/next_user_id"; i32 n = 1;
        { std::ifstream f(p); f >> n; if (n < 1) n = 1; }
        std::ofstream(p, std::ios::trunc) << n + 1;
        return n;
    }

    // the web login page: logs in, or registers a new GrowID. returns error text, empty on success
    std::string login_or_register(std::string name, const std::string &pw, std::string &token_out, bool register_ok)
    {
        std::lock_guard g(mu);
        name = clean_name(name);
        if (name.size() < 3 || name.size() > 18) return "Your GrowID must be 3 to 18 letters or digits.";
        if (pw.size() < 6 || pw.size() > 40) return "Your password must be 6 to 40 characters.";
        Account *a = find(name);
        if (!a)
        {
            if (!register_ok) return "That GrowID doesn't exist.";
            Account n; n.name = name; n.salt = random_hex(16); n.hash = pbkdf2(pw, n.salt);
            n.user_id = next_user_id(); n.created = now_s();
            bool first = n.user_id == 1; n.role = first ? 3 : 0;
            n.inv[18] = 1; n.inv[32] = 1;
            save(n); names[n.user_id] = n.name; a = &(accounts[lower(name)] = n);
        }
        else if (pbkdf2(pw, a->salt) != a->hash) return "Wrong password.";
        if (is_banned(*a)) { std::string t = ban_text(*a); t.erase(std::remove(t.begin(), t.end(), '`'), t.end()); return t; }
        token_out = random_hex(24);
        web_tokens[token_out] = {a->name, now_s() + 30 * 24 * 3600};
        return {};
    }

    Account *by_web_token(const std::string &t) // caller holds mu
    {
        auto it = web_tokens.find(t);
        if (it == web_tokens.end() || it->second.expires < now_s()) return nullptr;
        return find(it->second.name);
    }

    i32 make_transfer(const std::string &name, std::string &uuid_out) // caller holds mu
    {
        i32 t; do t = rnd(1, 0x7fffffff); while (transfers.count(t));
        uuid_out = random_hex(8);
        transfers[t] = {name, now_s() + 60, uuid_out};
        return t;
    }
    Account *take_transfer(i32 t, const std::string &uuid) // caller holds mu
    {
        auto it = transfers.find(t);
        if (it == transfers.end()) return nullptr;
        Transfer tr = it->second; transfers.erase(it);
        if (tr.expires < now_s() || tr.uuid != uuid) return nullptr;
        return find(tr.name);
    }
};
