// gtps: a Growtopia private server written from the protocol docs in ../docs
#include "game.hpp"
#include "vanilla.hpp"
#include "extras.hpp"
#include <csignal>
#include <zlib.h>

static volatile std::sig_atomic_t g_stop = 0;

// implemented in features.hpp (included below, after the helpers they use)
static void profile_self(Player *p);
static bool features_profile_button(Player *p, const std::string &b);
static bool features_dialog(Player *p, const std::string &name, const std::string &text);
static bool features_action(Player *p, const std::string &action, const std::string &text);
static void send_billboard(Player *to, Player *of);
static void diary_plant(Player *p, int seed, bool by_splice);
static void vend_break(Player *p, World &w, Tile &t);
static void vend_wrench(Player *p, World &w, int x, int y);
static void record_trade(Player *a, Player *b, const std::map<int, int> &gave, const std::map<int, int> &got);
static void load_achievements();
static void vend_refresh(World &w);
static void trade_complete(Player *p);
static std::string offer_text_fwd(const std::map<int, int> &o);
static void trade_confirm_return(Player *p, const std::string &pressed);
static void social_portal(Player *p);

// ---------------------------------------------------------------- world helpers
static World *load_world(const std::string &name)
{
    auto it = S.worlds.find(name);
    if (it != S.worlds.end()) return &it->second;
    World w; w.name = name;
    if (!w.load(S.cfg.data_dir + "/worlds", name)) { w = World{}; w.name = name; w.generate(S.items); }
    else vend_refresh(w);   // machines saved in a state the real server wouldn't allow (a pack bigger than the stock) are redrawn as out of order
    return &(S.worlds[name] = std::move(w));
}

static void broadcast(const std::string &world, const std::function<void(Player *)> &fn, Player *except = nullptr)
{
    for (Player *p : S.in_world(world)) if (p != except) fn(p);
}

static const char *MOD_COLOR = "`p"; // moderators are drawn as @Name in this colour (`p = pink)
static std::string base_name(World *w, Player *p) // name with its colour, no trailing reset
{
    if (p->acct->role >= 2) return std::string(MOD_COLOR) + "@" + p->name();
    return std::string(w && w->owner == p->acct->user_id ? "`2" : "`w") + p->name();
}
static std::string shown_name(World *w, Player *p) { return base_name(w, p) + "``"; }


static std::string spawn_text(Player *p, bool local)
{
    std::string name = shown_name(S.world_of(p), p);
    return "spawn|avatar\nnetID|" + std::to_string(p->netid) + "\nuserID|" + std::to_string(p->acct->user_id) +
           "\ncolrect|0|0|20|30\nposXY|" + std::to_string((int)p->x) + "|" + std::to_string((int)p->y) +
           "\nname|" + name + "\ntitleIcon|{\"PlayerWorldID\":" + std::to_string(p->netid) + ",\"WrenchCustomization\":{\"WrenchForegroundCanRotate\":false,\"WrenchForegroundID\":-1,\"WrenchIconID\":-1}}\ncountry|us\ninvis|0\nmstate|0\nsmstate|0" + // moderators are named @Name; mstate would switch off the client's own reach and lock checks

           "\nonlineID|\n" + (local ? "type|local\n" : "");
}

// ---------------------------------------------------------------- hand items: punch patterns and reach (resources/punch.txt)
struct PunchInfo { int reach = 0; int damage = 6; int aim = 0; std::vector<std::pair<int, int>> offsets{{0, 0}}; }; // aim = N: N tiles in a line along the direction you punch
static std::map<int, PunchInfo> g_punch;
static void load_punch(const std::string &path)
{
    std::ifstream f(path);
    for (std::string line; std::getline(f, line); )
    {
        if (line.empty() || line[0] == '#') continue;
        int id = 0, reach = 0, damage = 6; char offs[256] = {};
        if (sscanf(line.c_str(), "%d|%d|%255[^|]|%d", &id, &reach, offs, &damage) < 3) continue;
        PunchInfo pi; pi.reach = reach; pi.damage = damage; pi.offsets.clear();
        if (!strncmp(offs, "aim:", 4)) { pi.aim = atoi(offs + 4); pi.offsets = {{0, 0}}; g_punch[id] = pi; continue; }
        std::istringstream is(offs); for (std::string o; std::getline(is, o, ','); ) { int dx, dy; if (sscanf(o.c_str(), "%d:%d", &dx, &dy) == 2) pi.offsets.push_back({dx, dy}); }
        if (pi.offsets.empty()) pi.offsets = {{0, 0}};
        g_punch[id] = pi;
    }
    printf("punch patterns: %zu\n", g_punch.size());
}
static const PunchInfo &punch_of(Account &a) { static PunchInfo none; auto it = g_punch.find(a.clothes[5]); return it == g_punch.end() ? none : it->second; }

// ---------------------------------------------------------------- worn items with effects (resources/mods.txt, plus rules for the big families)
struct Mod { std::string name, add, rem; bool dj = false; int speed = 0, brk = 0, bld = 0, dmg = 0, fx = 0; };
static std::map<int, Mod> g_mods;
static void load_mods(const std::string &path)
{
    std::ifstream f(path);
    for (std::string line; std::getline(f, line); )
    {
        if (line.empty() || line[0] == '#') continue;
        std::vector<std::string> v; size_t pos = 0;
        for (;;) { size_t e = line.find('|', pos); v.push_back(line.substr(pos, e == std::string::npos ? e : e - pos)); if (e == std::string::npos) break; pos = e + 1; }
        if (v.size() < 5) continue;
        Mod md; md.name = v[1]; md.add = v[2]; md.rem = v[3];
        std::istringstream is(v[4]);
        for (std::string t; is >> t; )
        {
            if (t == "dj") md.dj = true; else if (!t.compare(0, 6, "speed=")) md.speed = atoi(t.c_str() + 6); else if (!t.compare(0, 6, "break=")) md.brk = atoi(t.c_str() + 6);
            else if (!t.compare(0, 6, "build=")) md.bld = atoi(t.c_str() + 6); else if (!t.compare(0, 7, "damage=")) md.dmg = atoi(t.c_str() + 7); else if (!t.compare(0, 3, "fx=")) md.fx = atoi(t.c_str() + 3);
        }
        g_mods[atoi(v[0].c_str())] = md;
    }
    printf("mods: %zu items\n", g_mods.size());
}
static bool mod_for(int id, Mod &out)
{
    auto it = g_mods.find(id); if (it != g_mods.end()) { out = it->second; return true; }
    const Item *i = S.items.get(id); if (!i || i->type != A_CLOTHES) return false;
    auto has = [&](const char *s) { return i->name.find(s) != std::string::npos; };
    out = Mod{};
    if (i->clothing == 6 && (has("Wings") || has("Wing ") || has("Cape")) && i->name.find("Backpack") == std::string::npos) { out.name = "Double Jump"; out.add = "You can jump in mid-air!  It's like flying, but less."; out.rem = "Gravity - it's the law."; out.dj = true; return true; }
    if (i->clothing == 3 && has("Robinsons")) { out.name = "Speedy"; out.add = "You can move faster!"; out.rem = "You feel slower."; out.speed = 310; return true; }
    if (i->clothing == 5 && (has("Pickaxe") || has("Hammer"))) { out.dmg = 8; return true; } // the capture only showed the Heartbreaker Hammer: 8 damage a punch
    return false;
}
struct Effects { bool dj = false; int speed = 250, brk = 0, bld = 0, dmg = 6, fx = 0; };
static const PunchInfo &punch_of(Account &a);
static Effects effects_of(Account &a)
{
    Effects e;
    for (u16 c : a.clothes)
    {
        Mod m; if (!c || !mod_for(c, m)) continue;
        e.dj |= m.dj; e.speed = std::max(e.speed, m.speed); e.brk += m.brk; e.bld += m.bld; e.dmg = std::max(e.dmg, m.dmg); if (m.fx) e.fx = m.fx;
    }
    const PunchInfo &pi = punch_of(a); e.brk += pi.reach; e.dmg = std::max(e.dmg, pi.damage);
    return e;
}
// "<message> (`$Mod`` mod added)" for what changed in one slot
static void mod_lines(Player *p, int was, int now)
{
    Mod m;
    if (was && mod_for(was, m) && !m.name.empty()) console(p, m.rem + " (`$" + m.name + "`` mod removed)");
    if (now && mod_for(now, m) && !m.name.empty()) console(p, m.add + " (`$" + m.name + "`` mod added)");
}

static void send_clothing(Player *to, Player *of)
{
    const u16 *c = of->acct->clothes;
    Effects e = effects_of(*of->acct);
    call(to->peer, {"OnSetRoleSkinsAndIcons", U32{6}, U32{6}, 0}, of->netid);
    call(to->peer, {"OnSetClothing", Vec3{(float)c[0], (float)c[1], (float)c[2]}, Vec3{(float)c[3], (float)c[4], (float)c[5]},
                    Vec3{(float)c[6], (float)c[7], (float)c[8]}, U32{(of->ghost && to == of) || (of->state & 1) ? (u32)-140 : of->acct->skin}, Vec3{(float)c[9], 1, 0}}, of->netid, 100);
    call(to->peer, {"OnFlagMay2019", U32{0}}, of->netid);
    // the real server's 0x14: objectType = punch effect, jumpCount/buildRange = 0x80 + extra build/break reach, itemID = state bits, speedX = speed
    GamePacket h; h.type = 0x14; h.objectType = e.fx; h.jumpCount = 0x80 + e.bld; h.buildRange = 0x80 + e.brk; h.netID = of->netid; h.targetNetID = 255;
    h.floatVar = 125.f; h.itemID = of->state | (e.dj ? 2 : 0); h.posX = 1200; h.posY = 200; h.speedX = e.speed; h.speedY = 1000; h.tileX = h.tileY = -1;
    send_packet(to->peer, h);
}

static void send_world_menu(Player *p)
{
    std::map<std::string, int> counts;
    for (Player *q : S.players) if (q->stage == 2 && !q->ghost && !q->world.empty()) ++counts[q->world];
    Account &a = *p->acct;
    std::string menu = "default|" + (a.recent.empty() ? std::string("START") : a.recent.front()) + "\nadd_filter|\nadd_heading|Random Worlds|\n";
    auto floater = [&](const std::string &n, int count, const char *color) { menu += "add_floater|" + n + "|" + std::to_string(count) + "|0.5|" + color + "\n"; };
    for (auto &[n, c] : counts) floater(n, c, "3529161471");                          // worlds with people in them
    for (auto &n : a.my_worlds) floater(n, counts[n], "2147418367");                   // yours
    if (!counts.count("START")) floater("START", 0, "3529161471");
    if (!a.recent.empty()) { menu += "add_heading|Recently Visited Worlds<CR>|\n"; for (auto &n : a.recent) floater(n, counts[n], "3417414143"); }
    call(p->peer, {"OnRequestWorldSelectMenu", menu});
    console(p, "Where would you like to go? (`w" + thousands(S.online()) + "`` online)");
}

static void trade_end(Player *p, const std::string &why);
static void leave_world(Player *p)
{
    if (p->world.empty()) return;
    trade_end(p, "`4" + p->name() + " left, the trade is cancelled.``");
    if (int keys = inv_count(*p->acct, 1424)) { inv_remove(*p->acct, 1424, keys); send_remove(p, 1424, keys); } // a World Key only lasts while you are in the world
    std::string wn = p->world; p->world.clear();
    if (!p->ghost)
    {
        int left = 0; for (Player *q : S.in_world(wn)) if (!q->ghost) ++left;
        broadcast(wn, [&](Player *q) {
            call(q->peer, {"OnConsoleMessage", "`5<`w" + p->name() + "`` left, `w" + std::to_string(left) + "`` others here>``"}, -1, 100);
            sfx(q, "audio/door_shut.wav");
            call(q->peer, {"OnRemove", "netID|" + std::to_string(p->netid) + "\n", "pId|" + std::to_string(p->acct->user_id) + "\n"});
        });
    }
    if (S.in_world(wn).empty())
    {
        auto it = S.worlds.find(wn);
        if (it != S.worlds.end()) { it->second.save(S.cfg.data_dir + "/worlds", S.items); S.worlds.erase(it); }
    }
}

static bool valid_world_name(const std::string &n) { if (n.empty() || n.size() > 23) return false; for (char c : n) if (!isalnum((unsigned char)c)) return false; return true; }

static void join_world(Player *p, std::string name)
{
    name = upper(clean_name(name));
    if (name.empty()) name = "START";
    if (!valid_world_name(name)) { console(p, "Sorry, that world name is not allowed."); call(p->peer, {"OnFailedToEnterWorld"}); return; }
    if (World *nw = load_world(name); p->acct->role < 2)
    {
        if (nw->nuked) { console(p, "`4That world is inaccessible.``"); call(p->peer, {"OnFailedToEnterWorld"}); return; }
        auto b = nw->banned.find(p->acct->user_id);
        if (b != nw->banned.end() && b->second && b->second < now_s()) { nw->banned.erase(b); nw->dirty = true; b = nw->banned.end(); } // the hour is over
        if (b != nw->banned.end()) { console(p, "`4You have been banned from this world.``"); call(p->peer, {"OnFailedToEnterWorld"}); return; }
    }
    if (S.in_world(name).size() >= 50) { console(p, "That world is full."); call(p->peer, {"OnFailedToEnterWorld"}); return; }
    leave_world(p);
    World *w = load_world(name);
    p->world = name; p->netid = ++S.next_netid[name]; p->rest = -1;
    p->x = w->door_x * 32.f; p->y = w->door_y * 32.f; p->left = false;

    GamePacket h; h.type = 0x04; h.tileX = (i32)(now_ms() & 0x7fffffff);
    send_packet(p->peer, h, w->serialize(S.items));
    send_world_preamble(p);
    { auto &r = p->acct->recent; r.erase(std::remove(r.begin(), r.end(), name), r.end()); r.insert(r.begin(), name); if (r.size() > 10) r.resize(10); }
    auto others = S.in_world(name);
    for (Player *q : others)
    {
        if (q == p || (q->ghost && p->acct->role < 2)) continue; // ghosts are only seen by other moderators
        call(p->peer, {"OnSpawn", spawn_text(q, false)}, -1, -1);
        send_clothing(p, q); send_billboard(p, q);
    }
    call(p->peer, {"OnSpawn", spawn_text(p, true)}, -1, -1);
    send_clothing(p, p); send_billboard(p, p);
    call(p->peer, {"OnDisabledParticleList", ""});
    call(p->peer, {"OnMagicCompassTrackingItemIDChanged", U32{0}});
    call(p->peer, {"OnClearItemTransforms"}, p->netid, 250);
    for (Player *q : others)
        if (q != p && (!p->ghost || q->acct->role >= 2)) { call(q->peer, {"OnSpawn", spawn_text(p, false)}, -1, -1); send_clothing(q, p); send_billboard(q, p); }
    int seen = 0; for (Player *q : others) if (q != p && !q->ghost) ++seen;
    if (!p->ghost) for (Player *q : others) if (q != p)
    {   // the others see "<name> entered, N others here" as a bubble and a console line, with the door sound
        std::string msg = "`5<`w" + p->name() + "`` entered, `w" + std::to_string(seen) + "`` others here>``";
        call(q->peer, {"OnTalkBubble", U32{(u32)p->netid}, msg, U32{1}}); console(q, msg); sfx(q, "audio/door_open.wav");
    }
    console(p, "World `w" + name + "`` entered.  There are `w" + std::to_string(seen) + "`` other people here, `w" + std::to_string(S.online()) + "`` online.");
    sfx(p, "audio/door_open.wav");
    if (w->owner) console(p, "`5[`w" + name + "`` `$World Locked`` by " + w->owner_name + (w->owner == p->acct->user_id ? " (`2ACCESS GRANTED``)" : "") + "`5]``");
}

// ---------------------------------------------------------------- tiles

// ---------------------------------------------------------------- levels (xp for the next level = 50 * (level^2 + 2), max level 125)
static void add_xp(Player *p, int amount)
{
    Account &a = *p->acct;
    if (a.level >= 125 || amount <= 0) return;
    a.xp += amount;
    for (;;)
    {
        u32 need = 50 * (a.level * a.level + 2);
        if (a.level >= 125 || a.xp < need) break;
        a.xp -= need; ++a.level;
        call(p->peer, {"OnPlayerLeveledUp", U32{a.level}}); sfx(p, "audio/levelup2.wav");
        call(p->peer, {"OnParticleEffect", 46, Vec2{p->x, p->y}, 0.f, 0.f});
        std::string msg = "`w" + a.name + "`` is now level " + std::to_string(a.level) + "!";
        call(p->peer, {"OnTalkBubble", U32{(u32)p->netid}, msg, U32{0}});
        console(p, msg);
    }
}

static bool is_area_lock(int id) { return id == 202 || id == 204 || id == 206 || id == 4994; }
static int area_cap(int id) { return id == 202 ? 10 : id == 204 ? 48 : 200; }
static bool has_access(const Tile &lock, Player *p)
{
    i32 uid = p->acct->user_id;
    return lock.lock_owner == uid || std::find(lock.access.begin(), lock.access.end(), uid) != lock.access.end() || p->acct->role >= 3;
}
// the owner of a world lock, anyone on its access list and moderators may pull and kick; only the owner (and moderators) may world-ban
static bool world_admin(World &w, Player *p)
{
    if (p->acct->role >= 2) return true;
    return w.lock_tile >= 0 && has_access(w.tiles[w.lock_tile], p);
}
static bool world_owner(World &w, Player *p) { return p->acct->role >= 2 || (w.owner && w.owner == p->acct->user_id); }
// may `p` change tile (x, y)? `breaking` matters for Builder's Lock (others may place, not break)
static bool can_modify(World &w, Player *p, int x, int y, bool breaking)
{
    Tile *t = w.at(x, y); if (!t) return false;
    if (p->acct->role >= 3 || !(t->flags & T_LOCKED)) return true;
    const Tile &l = w.tiles[t->parent];
    if (has_access(l, p) || (l.flags & T_PUBLIC)) return true;
    return l.fg == 4994 && !breaking;
}
// grow = true: the drop appears with 1 and is raised by "change object" updates (netID -3), as a harvest does on the real server
static void spawn_drop(World &w, int item, int count, float x, float y, bool grow = false)
{
    Drop d; d.id = item; d.count = std::min(count, 200); d.x = x; d.y = y; d.uid = ++w.last_uid;
    w.drops.push_back(d);
    GamePacket h; h.type = 0x0e; h.netID = -1; h.targetNetID = -1; h.itemID = item; h.floatVar = grow ? 1 : d.count; h.posX = x; h.posY = y;
    broadcast(w.name, [&](Player *q) { send_packet(q->peer, h); });
    if (grow) for (int k = 2; k <= d.count; ++k)
    {
        GamePacket u = h; u.netID = -3; u.targetNetID = d.uid; u.floatVar = k;
        broadcast(w.name, [&](Player *q) { send_packet(q->peer, u); });
    }
}

static void sync_tile(World &w, int x, int y)
{
    GamePacket h; h.type = 0x05; h.tileX = x; h.tileY = y;
    auto data = w.tile_update(x, y, S.items);
    broadcast(w.name, [&](Player *q) { send_packet(q->peer, h, data); });
}

static void set_world_lock(World &w, int x, int y, Player *p, int item)
{
    int idx = y * w.w + x;
    w.owner = p->acct->user_id; w.owner_name = p->acct->name; w.lock_tile = idx;
    w.tiles[idx].fg = item; w.set_lock(w.tiles[idx], w.owner, {});
    for (int i = 0; i < (int)w.tiles.size(); ++i) { Tile &t = w.tiles[i]; if (i != idx) { t.flags |= T_LOCKED; t.parent = idx; } }
    if (std::find(p->acct->my_worlds.begin(), p->acct->my_worlds.end(), w.name) == p->acct->my_worlds.end()) p->acct->my_worlds.push_back(w.name);
}

static void remove_world_lock(World &w)
{
    w.owner = 0; w.owner_name.clear(); w.lock_tile = -1;
    for (Tile &t : w.tiles) { t.flags &= ~T_LOCKED; t.parent = 0; }
}

// claims up to `cap` unlocked tiles around the lock, nearest first (8-neighbour flood fill)
static void set_area_lock(World &w, int x, int y, Player *p, int item)
{
    int idx = y * w.w + x, cap = area_cap(item);
    w.tiles[idx].fg = item; w.set_lock(w.tiles[idx], p->acct->user_id, {});
    std::vector<int> queue{idx}; std::set<int> seen{idx}; int claimed = 0;
    for (size_t q = 0; q < queue.size() && claimed < cap; ++q)
    {
        int cx = queue[q] % w.w, cy = queue[q] / w.w;
        if (queue[q] != idx) { Tile &t = w.tiles[queue[q]]; if (!(t.flags & T_LOCKED) && !(t.fg && S.items.get(t.fg) && S.items.get(t.fg)->type == A_LOCK)) { t.flags |= T_LOCKED; t.parent = idx; ++claimed; } }
        for (int dy = -1; dy <= 1; ++dy) for (int dx = -1; dx <= 1; ++dx)
        {
            int nx = cx + dx, ny = cy + dy; if (!w.at(nx, ny) || std::abs(nx - x) > 6 || std::abs(ny - y) > 6) continue;
            if (seen.insert(ny * w.w + nx).second) queue.push_back(ny * w.w + nx);
        }
    }
}

static void remove_area_lock(World &w, int idx)
{
    for (Tile &t : w.tiles) if ((t.flags & T_LOCKED) && t.parent == idx) { t.flags &= ~T_LOCKED; t.parent = 0; }
}

static void send_lock_area(World &w, int idx)
{
    Buf b; u32 n = 0;
    for (int i = 0; i < (int)w.tiles.size(); ++i) if (i != idx && (w.tiles[i].flags & T_LOCKED) && w.tiles[i].parent == idx) { b.put<u16>(i); ++n; }
    GamePacket h; h.type = 0x0f; h.netID = w.tiles[idx].lock_owner; h.targetNetID = n; h.tileX = idx % w.w; h.tileY = idx / w.w; h.itemID = w.tiles[idx].fg;
    broadcast(w.name, [&](Player *q) { send_packet(q->peer, h, b.d); });
}

static void refresh_names(World &w)
{
    for (Player *q : S.in_world(w.name)) { std::string nm = shown_name(&w, q); broadcast(w.name, [&](Player *o) { call(o->peer, {"OnNameChanged", nm, "{\"PlayerWorldID\":" + std::to_string(q->netid) + ",\"WrenchCustomization\":{\"WrenchForegroundCanRotate\":false,\"WrenchForegroundID\":-1,\"WrenchIconID\":-1}}"}, q->netid); }); }
}

static void lock_announce(World &w, Player *p, bool locked)
{
    std::string msg = locked ? "`5[`w" + w.name + "`` has been `$World Locked`` by " + p->name() + "`5]``" : "`5[`w" + w.name + "`` has had its `$World Lock`` removed!`5]``";
    broadcast(w.name, [&](Player *q) { console(q, msg); sfx(q, locked ? "audio/use_lock.wav" : "audio/blip_lock.wav"); });
    refresh_names(w);
}

static bool tree_grown(const Tile &t, const Item *it) { return it && it->type == A_SEED && now_s() - t.planted >= std::max(1, it->grow_time); }

// ponytail: drop odds are made up, real Growtopia's table isn't in the client
static void drop_for_break(World &w, int x, int y, const Item *it, int id)
{
    float px = x * 32.f + rnd(0, 16), py = y * 32.f + rnd(0, 16);
    if (!(it->flags2 & 0x20000) && rnd(1, 100) <= 60) spawn_drop(w, 112, rnd(1, std::max(1, (int)it->rarity / 4 + 1)), px, py);
    if (!(it->flags & 0x10) && rnd(1, 100) <= 17) spawn_drop(w, id, 1, px, py);                        // DROPLESS items never drop themselves
    if (!(it->flags & 0x04) && it->type != A_SEED && rnd(1, 100) <= 13) spawn_drop(w, id + 1, 1, px, py); // SEEDLESS items have no seed
}

static void harvest(World &w, Tile &t, int x, int y, const Item *seed, int seed_id, Player *p)
{
    float px = x * 32.f, py = y * 32.f;
    int fruit = t.extra.size() > 1 ? t.extra[1] : 3;
    // ponytail: the real yield varied from 1 to 9 for 1-4 shown fruit; fruit..2*fruit is a guess in that range
    spawn_drop(w, seed_id - 1, rnd(fruit, fruit * 2), px + rnd(0, 16), py, true);
    spawn_drop(w, 112, rnd(1, std::max(1, (int)seed->rarity / 3 + 1)), px + 8, py + 4);
    if (rnd(1, 100) <= 40)
    {
        spawn_drop(w, seed_id, 1, px + rnd(0, 16), py);
        broadcast(w.name, [&](Player *q) { call(q->peer, {"OnTalkBubble", U32{(u32)p->netid}, "A `w" + seed->name + "`` falls out!", U32{0}, U32{0}}); });
    }
    GamePacket h; h.type = 0x0c; h.netID = p->netid; h.targetNetID = -1; h.tileX = x; h.tileY = y;
    broadcast(w.name, [&](Player *q) { send_packet(q->peer, h); });
}

// PERMANENT items ("never lost when destroyed", e.g. locks) always come back; with AUTOPICKUP they go straight into the
// breaker's inventory (the real server sends a drop, a "Collected" line and the pickup, then the break), else they drop
static void break_drops(Player *p, World &w, int x, int y, const Item *it, int id)
{
    if (!(it->flags & 0x08)) { drop_for_break(w, x, y, it, id); return; }
    float px = x * 32.f + 17, py = y * 32.f + 15;
    if (!(it->flags & 0x200)) { spawn_drop(w, id, 1, px, py); return; }
    spawn_drop(w, id, 1, px, py);
    u32 uid = w.last_uid; w.drops.pop_back();            // never lands in the world: it is collected at once
    console(p, "Collected `w1 " + it->name + "``.``");
    GamePacket g; g.type = 0x0e; g.netID = p->netid; g.targetNetID = -1; g.itemID = uid;
    broadcast(w.name, [&](Player *q) { send_packet(q->peer, g); });
    inv_add(*p->acct, id, 1);
}

static void clear_fg(Tile &t) { t.fg = 0; t.extra.clear(); t.text.clear(); t.target.clear(); t.door_id.clear(); t.aux.clear(); t.flags &= ~(T_EXTRA | T_SEEDLING | T_SPAWN_SEEDS | T_ON | T_FLIPPED); }

// refused because of a lock: always the sound, and for building also "That area is owned by <name>."
static void deny(Player *p, World &w, int x, int y, bool say)
{
    sfx_at(p, p, "audio/punch_locked.wav");
    if (!say) return;
    Tile *t = w.at(x, y); if (!t || !(t->flags & T_LOCKED)) return;
    std::string owner; { std::lock_guard g(S.store.mu); owner = S.store.name_of(w.tiles[t->parent].lock_owner); }
    bubble(p, p, "That area is owned by " + owner + ".");
}

static void punch_tile(Player *p, World &w, GamePacket &pk)
{
    Tile *t = w.at(pk.tileX, pk.tileY); if (!t) return;
    int id = t->fg ? t->fg : t->bg; if (!id) return;
    const Item *it = S.items.get(id); if (!it) return;
    bool is_lock = t->fg && it->type == A_LOCK;
    if (!is_lock && !can_modify(w, p, pk.tileX, pk.tileY, true)) { deny(p, w, pk.tileX, pk.tileY, false); return; }
    if (t->fg && (it->type == A_MAIN_DOOR || it->type == A_BEDROCK)) { bubble(p, p, "It's too heavy to break."); return; }
    if (is_lock && t->lock_owner != p->acct->user_id && p->acct->role < 3) { bubble(p, p, "Only the lock's owner can break it."); return; }
    i64 now = now_ms();
    if (now - t->last_hit > std::max(1, it->reset_time) * 1000LL) t->damage = 0;
    int dmg = effects_of(*p->acct).dmg;
    t->last_hit = now; t->damage += dmg;
    bool grown_tree = t->fg && tree_grown(*t, it);
    if (t->damage < std::max<int>(6, it->break_raw) && !grown_tree)
    {   // as the real server: itemID = damage dealt, targetNetID = the tool (picks how the cracks are drawn)
        GamePacket h; h.type = 0x08; h.netID = p->netid; h.itemID = dmg; h.targetNetID = p->acct->clothes[5]; h.posX = pk.posX; h.posY = pk.posY; h.tileX = pk.tileX; h.tileY = pk.tileY;
        broadcast(w.name, [&](Player *q) { send_packet(q->peer, h); });
        return;
    }
    t->damage = 0; w.dirty = true;
    if (!(it->flags & 0x08) && it->rarity < 999) add_xp(p, grown_tree ? 1 + it->rarity / 3 : 1 + it->rarity / 5); // locks and other permanent items give none
    if (t->fg)
    {
        if (it->type == 62) vend_break(p, w, *t);
        if (is_lock)
        {
            int idx = pk.tileY * w.w + pk.tileX;
            if (idx == w.lock_tile) { remove_world_lock(w); lock_announce(w, p, false); } else remove_area_lock(w, idx);
        }
        if (grown_tree) { harvest(w, *t, pk.tileX, pk.tileY, it, id, p); clear_fg(*t); return; }
        if (it->type != A_SEED) break_drops(p, w, pk.tileX, pk.tileY, it, id);
        clear_fg(*t);
    }
    else { break_drops(p, w, pk.tileX, pk.tileY, it, id); t->bg = 0; }
    GamePacket h; h.type = 0x03; h.netID = p->netid; h.itemID = 18; h.tileX = pk.tileX; h.tileY = pk.tileY;
    broadcast(w.name, [&](Player *q) { send_packet(q->peer, h); });
}

// the server enforces reach too: 2 tiles around the player, plus Rayman/Eye/Trout (breaking) or Diamond Horns (building)
static bool in_range(const GamePacket &pk, int range)
{
    int px = (int)(pk.posX + 10) / 32, py = (int)(pk.posY + 15) / 32;
    return std::abs(pk.tileX - px) <= range && std::abs(pk.tileY - py) <= range;
}

static void punch(Player *p, World &w, GamePacket &pk)
{
    if (!in_range(pk, 2 + effects_of(*p->acct).brk)) return;
    const PunchInfo &pi = punch_of(*p->acct);
    int dir = (pk.flags & S_LEFT) ? -1 : 1;
    if (pi.aim > 0) // a line away from the player through the tile they punched: row, column or diagonal
    {
        int px = (int)(p->x + 10) / 32, py = (int)(p->y + 15) / 32;
        int sx = (pk.tileX > px) - (pk.tileX < px), sy = (pk.tileY > py) - (pk.tileY < py);
        if (!sx && !sy) sx = dir;
        for (int i = 0; i < pi.aim; ++i) { GamePacket q = pk; q.tileX = pk.tileX + sx * i; q.tileY = pk.tileY + sy * i; punch_tile(p, w, q); }
        return;
    }
    for (auto [dx, dy] : pi.offsets)
    {
        GamePacket q = pk; q.tileX = pk.tileX + dx * dir; q.tileY = pk.tileY + dy;
        punch_tile(p, w, q);
    }
}

static bool placeable(const Item *, int id) { return id != 5814; } // guild lock needs guilds

// planting a seed on a young tree of another seed splices them (recipes: the two seeds stored on the result seed in items.dat, version 23+)
static int splice_result(int a, int b)
{
    static std::map<std::pair<int, int>, int> recipes;
    if (recipes.empty()) for (const Item &i : S.items.list) if (i.type == A_SEED && i.splice_a && i.splice_b) recipes[{std::min<int>(i.splice_a, i.splice_b), std::max<int>(i.splice_a, i.splice_b)}] = i.id;
    auto it = recipes.find({std::min(a, b), std::max(a, b)});
    return it == recipes.end() ? 0 : it->second;
}

static void splice(Player *p, World &w, GamePacket &pk, int placed, Tile &t)
{
    const Item *tree = S.items.get(t.fg);
    if (t.fg == placed) return;
    if (tree_grown(t, tree)) { bubble(p, p, "The tree is ready to harvest, it can't be spliced anymore."); return; }
    int res = splice_result(placed, t.fg);
    if (!res) { bubble(p, p, "`4These seeds can't be spliced together.``"); sfx(p, "audio/bleep_fail.wav"); return; }
    inv_remove(*p->acct, placed, 1);
    std::string a = S.items.get(placed)->name, b = tree->name, r = S.items.get(res)->name;
    if (r.size() > 5 && r.compare(r.size() - 5, 5, " Seed") == 0) r.replace(r.size() - 4, 4, "Tree");
    t.fg = res; t.planted = now_s(); t.extra = {0x04, (u8)rnd(1, 4)}; t.flags |= T_EXTRA | T_SEEDLING; w.dirty = true;
    // the real server's answer: a bubble, the success sound, the seed removed, the new tile (no block-placed packet)
    broadcast(w.name, [&](Player *q) { call(q->peer, {"OnTalkBubble", U32{(u32)p->netid}, "`w" + a + "`` and `w" + b + "`` have been spliced to make a `$" + r + "``!", U32{0}, U32{1}}); });
    sfx(p, "audio/success.wav");
    send_remove(p, placed, 1);
    sync_tile(w, pk.tileX, pk.tileY);
    add_xp(p, 1 + S.items.get(res)->rarity / 5);
    diary_plant(p, res, true);
}

static void place(Player *p, World &w, GamePacket &pk)
{
    int id = pk.itemID;
    const Item *it = S.items.get(id); Tile *t = w.at(pk.tileX, pk.tileY);
    if (!it || !t || inv_count(*p->acct, id) < 1) return;
    if (!in_range(pk, 2 + effects_of(*p->acct).bld)) return;
    if (it->type == A_CLOTHES || it->type == A_FIST || it->type == A_WRENCH) return;
    if (!can_modify(w, p, pk.tileX, pk.tileY, false)) { deny(p, w, pk.tileX, pk.tileY, true); return; }
    if (!placeable(it, id)) { console(p, "`4This item can't be placed on this server yet.``"); return; }
    if (it->type == A_SEED && t->fg && S.items.get(t->fg) && S.items.get(t->fg)->type == A_SEED) { splice(p, w, pk, id, *t); return; }
    bool is_bg = it->type == 18 || it->type == 22 || it->type == 23 || it->type == 28;
    if (it->type == A_SEED && t->fg) { bubble(p, p, "You can only use seeds on blank tiles or existing trees."); return; }
    if (is_bg ? t->bg != 0 : t->fg != 0) return;
    if (!is_bg && it->collision == 1 && it->type != A_SEED)
        for (Player *q : S.in_world(w.name)) if ((int)(q->x + 10) / 32 == pk.tileX && (int)(q->y + 15) / 32 == pk.tileY) return;
    if (it->type == 62 && !((t->flags & T_LOCKED) && w.tiles[t->parent].lock_owner == p->acct->user_id))
    { bubble(p, p, "`4A Vending Machine can only be placed in an area you own.``"); return; }
    if (it->type == A_LOCK)
    {
        if (id == 5814) { console(p, "`4Guild locks aren't supported yet.``"); return; }
        if (w.owner) { bubble(p, p, is_area_lock(id) ? "This world is already locked." : "Only one `$World Lock`` can be placed in a world, you'd have to remove the other one first."); return; }
        if (t->flags & T_LOCKED) return;
    }
    if (it->type == A_SEED) { const Tile *below = w.at(pk.tileX, pk.tileY + 1); if (!below || !below->fg) return; }

    GamePacket h; h.type = 0x03; h.netID = p->netid; h.itemID = id; h.tileX = pk.tileX; h.tileY = pk.tileY; h.flags = pk.flags & S_LEFT;
    if (is_bg) t->bg = id;
    else
    {
        t->fg = id;
        if (it->type == 2 || it->type == 13 || it->type == 26 || it->type == 43) World::set_door(*t, "");
        else if (it->type == 10) World::set_sign(*t, "");
        else if (it->type == 62) { t->extra = extras::make_for_item(*it, w.version); t->flags |= T_EXTRA; t->aux = "0 0 0 " + std::to_string(p->acct->user_id); }
        else if (it->type == A_SEED)
        {
            int fruit = rnd(2, 4);
            t->extra = {0x04, (u8)fruit}; t->planted = now_s(); t->flags |= T_EXTRA | T_SEEDLING;
            h.buildRange = fruit; diary_plant(p, id, false);
        }
        else if (it->type != A_LOCK && it->type != A_SEED && extras::type_for_action(it->type) && it->type != A_BEDROCK)
        { t->extra = extras::make_for_item(*it, w.version); if (!t->extra.empty()) t->flags |= T_EXTRA; }
        else if (it->type == A_LOCK) { if (is_area_lock(id)) set_area_lock(w, pk.tileX, pk.tileY, p, id); else set_world_lock(w, pk.tileX, pk.tileY, p, id); }
        if ((it->flags & 0x01) && (pk.flags & S_LEFT)) t->flags |= T_FLIPPED;
    }
    inv_remove(*p->acct, id, 1);
    if (it->type == A_SEED) add_xp(p, 1);
    w.dirty = true;
    if (it->type == A_LOCK) // the real server sends no block-placed packet for a lock: SEND_LOCK sets it, plus a sound, a bubble and the inventory change
    {
        send_lock_area(w, pk.tileY * w.w + pk.tileX);
        if (is_area_lock(id)) broadcast(w.name, [&](Player *q) { sfx_at(q, p, "audio/use_lock.wav"); call(q->peer, {"OnTalkBubble", U32{(u32)p->netid}, "Area locked.", U32{0}, U32{0}}); });
        send_remove(p, id, 1);
        call(p->peer, {"OnSetFriendOfWorldOwner", U32{0}}, p->netid);
        if (!is_area_lock(id)) lock_announce(w, p, true);
        return;
    }
    broadcast(w.name, [&](Player *q) { send_packet(q->peer, h); });
}

static void pickup(Player *p, World &w, GamePacket &pk)
{
    u32 uid = pk.itemID;
    auto it = std::find_if(w.drops.begin(), w.drops.end(), [&](const Drop &d) { return d.uid == uid; });
    if (it == w.drops.end()) return;
    if (std::abs(it->x - p->x) > 5 * 32 || std::abs(it->y - p->y) > 5 * 32) return;
    Account &a = *p->acct; const Item *item = S.items.get(it->id);
    if (it->id == 112) { a.gems += it->count; }
    else
    {
        if (!item) return;
        bool have = inv_count(a, it->id) > 0;
        if (!have && a.inv.size() >= a.backpack) return;
        if (inv_count(a, it->id) + it->count > std::max<int>(1, item->max_amount)) return;
        inv_add(a, it->id, it->count);
    }
    if (it->id != 112 && item) console(p, "Collected `w" + std::to_string(it->count) + " " + item->name + "``. Rarity: `w" + std::to_string(item->rarity) + "``");
    GamePacket h; h.type = 0x0e; h.netID = p->netid; h.targetNetID = -1; h.itemID = uid;
    broadcast(w.name, [&](Player *q) { send_packet(q->peer, h); });
    w.drops.erase(it); w.dirty = true; // the client adds the item or gems to its own copy when it sees the 0x0e
}

// ---------------------------------------------------------------- inventory actions
static void wear(Player *p, int id)
{
    const Item *it = S.items.get(id); Account &a = *p->acct;
    if (!it || inv_count(a, id) < 1 || it->type != A_CLOTHES || it->clothing > 9) return;
    u16 &slot = a.clothes[it->clothing];
    int was = slot; slot = slot == id ? 0 : id;
    mod_lines(p, was == id ? id : was, was == id ? 0 : id);
    if (slot) { if (!p->world.empty()) broadcast(p->world, [&](Player *q) { call(q->peer, {"OnEquipNewItem", id}, p->netid); }); else call(p->peer, {"OnEquipNewItem", id}, p->netid); }
    if (!p->world.empty()) broadcast(p->world, [&](Player *q) { send_clothing(q, p); });
    else send_clothing(p, p);
}

// double tapping a Diamond Lock shatters it into 100 World Locks; 100+ World Locks compress into a Diamond Lock; Blue Gem Lock -> 100 Diamond Locks
static bool convert_locks(Player *p, int id)
{
    Account &a = *p->acct; int from = id, to = 0, n = 0, back = 0;
    if (id == 1796) { to = 242; n = 100; }
    else if (id == 7188) { to = 1796; n = 100; }
    else if (id == 242) { to = 1796; back = 100; }
    else return false;
    const std::string fn = S.items.get(from)->name, tn = S.items.get(to)->name;
    auto say = [&](const std::string &t) { call(p->peer, {"OnTalkBubble", U32{(u32)p->netid}, t, U32{0}, U32{1}}); console(p, t); };
    auto add = [&](int item, int count) { GamePacket g; g.type = 0x0d; g.itemID = item; g.buildRange = count; send_packet(p->peer, g); };
    if (back)                      // World Locks -> Diamond Lock
    {
        if (inv_count(a, from) < back) return true;
        if (inv_count(a, to) + 1 > 200) { say("`4You can't hold any more `2" + tn + "``!``"); return true; }
        inv_remove(a, from, back); send_remove(p, from, back); inv_add(a, to, 1); add(to, 1);
        say("You compressed " + std::to_string(back) + " `2" + fn + "`` into a `2" + tn + "``!"); return true;
    }
    if (inv_count(a, from) < 1) return true;
    if (inv_count(a, to) + n > 200) { say("`4You don't have room for " + std::to_string(n) + " `2" + tn + "``!``"); return true; }
    inv_remove(a, from, 1); send_remove(p, from, 1); inv_add(a, to, n); add(to, n);
    say("You shattered a `2" + fn + "`` into " + std::to_string(n) + " `2" + tn + "``!"); return true;
}

static void drop_dialog(Player *p, int id, bool trash)
{
    const Item *it = S.items.get(id);
    if (!it || inv_count(*p->acct, id) < 1) return;
    if (it->flags & 0x8000) { console(p, "`4You can't " + std::string(trash ? "trash" : "drop") + " that.``"); return; }
    Dialog d;
    d.label_icon("`w" + std::string(trash ? "Trash " : "Drop ") + it->name + "``", id).spacer()
     .text(trash ? "`oHow many to `4destroy`o? (you have " + std::to_string(inv_count(*p->acct, id)) + ")``" : "`oHow many to drop? (you have " + std::to_string(inv_count(*p->acct, id)) + ")``")
     .input("count", "", std::to_string(inv_count(*p->acct, id)), 5).hidden("itemID", std::to_string(id)).quick_exit();
    dialog(p, d.end(trash ? "trash_item" : "drop_item", "Cancel", trash ? "`4Trash``" : "OK"));
}

static void do_drop(Player *p, int id, int n, bool trash)
{
    Account &a = *p->acct; const Item *it = S.items.get(id); World *w = S.world_of(p);
    n = std::min(n, inv_count(a, id));
    if (!it || n < 1 || (it->flags & 0x8000)) return;
    if (!trash && !w) return;
    for (u16 &c : a.clothes) if (c == id && inv_count(a, id) - n < 1) c = 0;
    inv_remove(a, id, n); send_remove(p, id, n);
    if (trash) { sfx(p, "audio/trash.wav"); console(p, "`oTrashed `w" + std::to_string(n) + " " + it->name + "``."); return; }
    float x = p->x + (p->left ? -32 : 40), y = p->y;
    Tile *t = w->at((int)(x + 16) / 32, (int)(y + 16) / 32);
    if (!t || (t->fg && S.items.get(t->fg) && S.items.get(t->fg)->collision == 1)) x = p->x;
    spawn_drop(*w, id, n, x, y); w->dirty = true;
    console(p, "`oDropped `w" + std::to_string(n) + " " + it->name + "``.");
}

static void info_dialog(Player *p, int id)
{
    const Item *it = S.items.get(id); if (!it) return;
    Dialog d; d.label_icon("`w" + it->name + "``", id).spacer().text(it->info.empty() ? "No description." : it->info);
    if (it->type == A_SEED) d.text("`oGrows in `w" + std::to_string(it->grow_time) + "`` seconds.");
    d.text("`oRarity: `w" + std::to_string(it->rarity) + "``").quick_exit();
    dialog(p, d.end("item_info", "", "OK"));
}

// ---------------------------------------------------------------- trading
// a World Key traded: the world lock's owner becomes `to`
static void transfer_world(Player *from, Player *to)
{
    World *w = S.world_of(from);
    if (!w || w->lock_tile < 0 || w->owner != from->acct->user_id) return;
    Tile &lt = w->tiles[w->lock_tile];
    w->owner = to->acct->user_id; w->owner_name = to->name(); w->set_lock(lt, w->owner, {}, lt.lock_flags); w->dirty = true;
    auto &mine = from->acct->my_worlds; mine.erase(std::remove(mine.begin(), mine.end(), w->name), mine.end());
    if (std::find(to->acct->my_worlds.begin(), to->acct->my_worlds.end(), w->name) == to->acct->my_worlds.end()) to->acct->my_worlds.push_back(w->name);
    send_lock_area(*w, w->lock_tile);
    std::string a = from->name(), b = to->name();
    broadcast(w->name, [&](Player *q) {
        console(q, "`1`w" + b + "`` traded nothing to " + a + ".``");
        call(q->peer, {"OnPlayPositioned", "audio/change_clothes.wav"}, from->netid, 600);
        sfx(q, "audio/use_lock.wav");
        console(q, "`5[```w" + w->name + "`` has been `$World Locked`` by " + b + "`5]``");
        console(q, "`1`2" + a + "`` traded 1 World Key to " + b + ".``");
        call(q->peer, {"OnPlayPositioned", "audio/keypad_hit.wav"}, to->netid, 1250);
    });
    refresh_names(*w);
}

static void trade_status(Player *to, Player *side, Player *other) // one OnTradeStatus about `side`, as seen by `to`
{
    if (!to->trade_open) return;
    std::string offer = "locked|0\nreset_locks|1\naccepted|" + std::string(side->accepted ? "1" : "0") + "\n";
    for (auto &[id, n] : side->offer) offer += "add_slot|" + std::to_string(id) + "|" + std::to_string(n) + "\n";
    std::string label = side == to ? "`oYour offer.``" : "`o" + side->name() + "'s offer.``";
    if (side == to) call(to->peer, {"OnTradeStatus", side->netid, label, "", offer});
    else call(to->peer, {"OnTradeStatus", side->netid, "", label, offer});
    (void)other;
}
static void trade_refresh(Player *a)
{
    Player *b = a->trade_with; if (!b) return;
    for (Player *to : {a, b}) { trade_status(to, a, b); trade_status(to, b, a); }
}
static void trade_end(Player *p, const std::string &why)
{
    Player *o = p->trade_with; p->trade_req = 0;
    if (!o) { if (p->trade_open) { p->trade_open = false; call(p->peer, {"OnForceTradeEnd"}); } return; }
    bool po = p->trade_open, oo = o->trade_open;
    p->trade_with = o->trade_with = nullptr; p->offer.clear(); o->offer.clear(); p->accepted = o->accepted = p->key_ok = o->key_ok = p->trade_open = o->trade_open = false;
    if (po) call(p->peer, {"OnForceTradeEnd"});
    if (oo) call(o->peer, {"OnForceTradeEnd"});
    if (!why.empty() && oo) console(o, why);
}
static void trade_request(Player *p, int netid)
{
    for (Player *q : S.in_world(p->world))
    {
        if (q->netid != netid || q == p) continue;
        if (p->trade_with == q) // they asked first: this answers it and opens our window (theirs is already open)
        {
            if (!p->trade_open) { p->trade_open = true; call(p->peer, {"OnStartTrade", q->name(), q->netid}); }
            return;
        }
        if (p->trade_with || q->trade_with) { console(p, "`4That player is busy.``"); return; }
        // the window opens at once for the one who asked ("waiting for <name>") and offers can already be added; the other gets the alert
        p->trade_with = q; q->trade_with = p; p->trade_open = true; q->trade_open = false;
        call(p->peer, {"OnStartTrade", q->name(), q->netid});
        console(q, "`#TRADE ALERT:`` `w`w" + p->name() + "```` wants to trade with you!  To start, use the `wWrench`` on that person's wrench icon, or type `w/trade `w" + p->name() + "``");
        sfx(q, "audio/cash_register.wav");
        return;
    }
}
static void trade_add(Player *p, int id, int n)
{
    Player *o = p->trade_with; const Item *it = S.items.get(id);
    if (!o || !p->trade_open || !it || n < 1) return;
    if ((it->flags & 0x8000) || inv_count(*p->acct, id) < 1) { console(p, "`4You can't trade that.``"); return; }
    if (!p->offer.count(id) && p->offer.size() >= 4) { console(p, "`4Only four items fit in a trade.``"); return; }
    p->offer[id] = std::min(inv_count(*p->acct, id), p->offer[id] + n);
    p->accepted = o->accepted = false;
    trade_refresh(p);
}
static void trade_accept(Player *p, const std::string &text)
{
    Player *o = p->trade_with; if (!o || !p->trade_open) return;
    bool on = field(text, "status") == "1";
    if (on) // the client lists what it shows of the other side: refuse if that no longer matches
    {
        std::map<int, int> seen; size_t pos = 0;
        while ((pos = text.find("item|", pos)) != std::string::npos) { int id = 0, n = 0; if (sscanf(text.c_str() + pos, "item|%d|%d", &id, &n) == 2) seen[id] = n; pos += 5; }
        if (seen != o->offer) { trade_refresh(p); return; }
    }
    p->accepted = on;
    trade_refresh(p);
    if (!(p->accepted && o->accepted && o->trade_open)) return;
    trade_complete(p);
}

// both accepted. Giving away a World Key first needs a confirmation from its owner (the trade window closes meanwhile)
static void trade_complete(Player *p)
{
    Player *o = p->trade_with; if (!o) return;
    for (Player *giver : {p, o})
        if (giver->offer.count(1424) && !giver->key_ok)
        {
            Player *taker = giver == p ? o : p; World *w = S.world_of(giver);
            call(p->peer, {"OnForceTradeEnd"}); call(o->peer, {"OnForceTradeEnd"}); p->trade_open = o->trade_open = false; // the windows close while the owner confirms
            std::string get; for (auto &[id, n] : taker->offer) get += "add_label_with_icon|small|(`w" + std::to_string(n) + "``) " + S.items.get(id)->name + "|left|" + std::to_string(id) + "|\nadd_spacer|small|\n";
            std::string give; for (auto &[id, n] : giver->offer) give += "add_label_with_icon|small|(`w" + std::to_string(n) + "``) " + S.items.get(id)->name + "|left|" + std::to_string(id) + "|\nadd_spacer|small|\n";
            dialog(giver, "set_default_color|`o\nadd_label_with_icon|big|`wTrade Confirmation``|left|1366|\nadd_spacer|small|\nadd_textbox|`4You'll give:``|left|\nadd_spacer|small|\n" + give +
                          "add_textbox|`2You'll get:``|left|\nadd_spacer|small|\n" + (get.empty() ? "add_textbox|`4Nothing!``|left|\nadd_spacer|small|\n" : get) +
                          "add_textbox|`4WARNING: ``You are about to sell your world `#" + (w ? w->name : std::string("?")) + "`` - the world lock ownership will be transferred over to `w" + taker->name() + "``.|left|\nadd_spacer|small|\nadd_spacer|small|\n" +
                          (get.empty() ? "add_textbox|`4SCAM WARNING: ``You are about to do a trade without receiving anything in return. Once you do the trade you cannot get the items back.|left|\n" : "") +
                          "add_textbox|`4Do you really want to do this?``|left|\nadd_spacer|small|\nadd_button|accept|Do The Trade!|noflags|0|0|\nadd_button|back|Cancel|noflags|0|0|\nend_dialog|trade_confirm|||\n");
            return;
        }
    auto fits = [](Player *to, Player *from) {
        int newslots = 0; for (auto &[id, n] : from->offer) { int have = inv_count(*to->acct, id); if (!have) ++newslots; if (have + n > std::max<int>(1, S.items.get(id)->max_amount)) return false; }
        return (int)to->acct->inv.size() + newslots <= to->acct->backpack;
    };
    for (Player *s : {p, o}) for (auto &[id, n] : s->offer) if (inv_count(*s->acct, id) < n) { trade_end(p, "`4Trade failed: an item is missing.``"); console(p, "`4Trade failed: an item is missing.``"); return; }
    if (!fits(p, o) || !fits(o, p)) { p->accepted = o->accepted = false; console(p, "`4Trade failed: not enough room.``"); console(o, "`4Trade failed: not enough room.``"); trade_refresh(p); return; }
    for (auto [from, to] : {std::pair{p, o}, std::pair{o, p}})
        for (auto &[id, n] : from->offer)
        {
            if (id == 1424) { inv_remove(*from->acct, id, n); send_remove(from, id, n); transfer_world(from, to); continue; } // the key is used up: the world changes owner
            inv_remove(*from->acct, id, n); inv_add(*to->acct, id, n);
            for (u16 &c : from->acct->clothes) if (c == id && !inv_count(*from->acct, id)) c = 0;
            send_remove(from, id, n);
            GamePacket h; h.type = 0x0d; h.itemID = id; h.buildRange = n; send_packet(to->peer, h);
        }
    auto gave = p->offer, got = o->offer;
    record_trade(p, o, gave, got);
    for (auto [from, to] : {std::pair{p, o}, std::pair{o, p}})
    {   // the real lines and effects: "<from> traded <items> to <to>.", the items fly over, a sound from the giver
        std::string what = offer_text_fwd(from->offer == gave ? gave : got);
        for (Player *v : {p, o}) call(v->peer, {"OnConsoleMessage", "`1`w" + from->name() + "`` traded " + (from->offer.empty() ? std::string("nothing") : what) + " to " + to->name() + ".``"}, -1, 300);
        if (!from->offer.empty())
        {
            GamePacket h; h.type = 0x13; h.buildRange = 3; h.netID = from->netid; h.targetNetID = to->netid; h.itemID = 600; h.tileX = h.tileY = from->offer.begin()->first;
            send_packet(p->peer, h); send_packet(o->peer, h);
        }
        call(from->peer, {"OnPlayPositioned", "audio/change_clothes.wav"}, from->netid, 600);
    }
    trade_end(p, ""); sfx(p, "audio/success.wav"); sfx(o, "audio/success.wav");
}

static void trade_confirm_return(Player *p, const std::string &pressed)
{
    if (!p->trade_with) return;
    if (pressed == "accept")
    {
        console(p, "`6[``Trade accepted, waiting for other player to accept`6]``");
        call(p->peer, {"OnTalkBubble", U32{(u32)p->netid}, "`6[``Trade accepted, waiting for other player to accept`6]``", U32{0}, U32{0}});
        p->key_ok = true; trade_complete(p);
    }
    else trade_end(p, "`4" + p->name() + " cancelled the trade.``");
}

// ---------------------------------------------------------------- friends and private messages
static std::map<i32, std::set<i32>> g_friend_req; // target user id -> who asked

static Player *online_user(i32 uid) { for (Player *q : S.players) if (q->stage == 2 && !q->ghost && q->acct && q->acct->user_id == uid) return q; return nullptr; }

static void friend_alert(Player *p, bool on)
{
    if (p->ghost) return;
    for (i32 uid : p->acct->friends)
        if (Player *q = online_user(uid)) if (q->acct->friends.count(p->acct->user_id))
            { console(q, "`3FRIEND ALERT:`` `w" + p->name() + "`` has logged " + (on ? "on" : "off") + "."); sfx(q, on ? "audio/friend_logon.wav" : "audio/friend_logoff.wav"); }
}

static void friend_request(Player *p, int netid)
{
    for (Player *q : S.in_world(p->world))
    {
        if (q->netid != netid || q == p) continue;
        i32 a = p->acct->user_id, b = q->acct->user_id;
        if (std::abs(p->x - q->x) > 3 * 32 || std::abs(p->y - q->y) > 3 * 32)
        { call(p->peer, {"OnTalkBubble", U32{(u32)p->netid}, "`5[```4Too far away`` - Stand closer to `w" + q->name() + "`` first!`5]``", U32{0}, U32{0}}); return; }
        if (p->acct->friends.count(b)) { console(p, "`oYou're already friends.``"); return; }
        if (g_friend_req[a].count(b)) { console(p, "`oYou already asked `w" + q->name() + "``.``"); return; }
        if (g_friend_req[b].count(a)) // they asked first: this is the accept
        {
            g_friend_req[b].erase(a); p->acct->friends.insert(b); q->acct->friends.insert(a);
            console(p, "`2You're now friends with `w" + q->name() + "``!``"); console(q, "`2You're now friends with `w" + p->name() + "``!``");
            return;
        }
        g_friend_req[a].insert(b);
        call(p->peer, {"OnTalkBubble", U32{(u32)p->netid}, "`5[``Friend request sent to `w" + q->name() + "```5]``", U32{0}, U32{0}});
        sfx(q, "audio/friend_beep.wav");
        console(q, "`3" + p->name() + "`` wants to be your friend! Wrench them and press `wAdd as friend`` to accept.");
        return;
    }
}

static void friends_dialog(Player *p)
{
    Account &a = *p->acct; int online = 0;
    Dialog d; d.label_icon("`wSocial Portal``", 1366);
    std::string rows;
    for (i32 uid : a.friends)
    {
        std::string nm; { std::lock_guard g(S.store.mu); nm = S.store.name_of(uid); }
        if (Player *q = online_user(uid)) { ++online; rows += "add_button|warp_" + std::to_string(uid) + "|`2" + nm + "`` (" + (q->world.empty() ? "in the world menu" : "in " + q->world) + ")|noflags|0|0|\n"; }
        else rows += "add_textbox|`4" + nm + "`` is offline.|left|\n";
        rows += "add_button|remove_" + std::to_string(uid) + "|`4Remove " + nm + "``|small_font|0|0|\n";
    }
    d.spacer().text("`oYou have `w" + std::to_string(a.friends.size()) + "`` friends, `2" + std::to_string(online) + "`` online.``");
    d.s += rows; d.quick_exit();
    dialog(p, d.end("friends_menu", "", "OK"));
}

static void friends_return(Player *p, const std::string &text)
{
    std::string b = field(text, "buttonClicked");
    if (b.rfind("warp_", 0) == 0) { Player *q = online_user(atoi(b.c_str() + 5)); if (q && p->acct->friends.count(q->acct->user_id) && !q->world.empty()) join_world(p, q->world); }
    else if (b.rfind("remove_", 0) == 0)
    {
        i32 uid = atoi(b.c_str() + 7); p->acct->friends.erase(uid);
        if (Player *q = online_user(uid)) { q->acct->friends.erase(p->acct->user_id); console(q, "`4" + p->name() + " removed you as a friend.``"); }
        else { std::lock_guard g(S.store.mu); if (Account *o = S.store.find(S.store.name_of(uid))) { o->friends.erase(p->acct->user_id); S.store.save(*o); } }
        friends_dialog(p);
    }
}

static void private_message(Player *p, const std::string &who, const std::string &text)
{
    for (Player *q : S.players)
        if (q->stage == 2 && q->acct && lower(q->name()) == lower(who))
        {
            sfx(q, "audio/msg.wav");
            console(q, "CP:0_PL:0_OID:_CT:[MSG]_`c>> from (`w" + p->name() + "`c) in [`w" + (p->world.empty() ? std::string("the world menu") : p->world) + "`c]! > `o" + text + "``");
            console(p, "`6>> Message sent to `w" + q->name() + "``: " + text); return;
        }
    console(p, "`4That player isn't online.``");
}

// ---------------------------------------------------------------- store (resources/store.txt: tab|btn|name|rttx|description|tex1|tex2|cost|id:amount,...)
struct Product { int tab = 0; std::string btn, name, rttx, desc; int t1 = 0, t2 = 0, cost = 0; std::vector<std::pair<int, int>> items; };
static std::vector<Product> g_products;

static void load_store(const std::string &path)
{
    std::ifstream f(path);
    for (std::string line; std::getline(f, line); )
    {
        if (line.empty() || line[0] == '#') continue;
        std::vector<std::string> v; size_t pos = 0;
        for (;;) { size_t e = line.find('|', pos); v.push_back(line.substr(pos, e == std::string::npos ? e : e - pos)); if (e == std::string::npos) break; pos = e + 1; }
        if (v.size() < 9) continue;
        Product pr; pr.tab = atoi(v[0].c_str()); pr.btn = v[1]; pr.name = v[2]; pr.rttx = v[3]; pr.desc = v[4]; pr.t1 = atoi(v[5].c_str()); pr.t2 = atoi(v[6].c_str()); pr.cost = atoi(v[7].c_str());
        std::istringstream is(v[8]); for (std::string it; std::getline(is, it, ','); ) { int id = 0, n = 0; if (sscanf(it.c_str(), "%d:%d", &id, &n) == 2) pr.items.push_back({id, n}); }
        g_products.push_back(pr);
    }
    printf("store: %zu products\n", g_products.size());
}

static int backpack_cost(Account &a) { int No = (a.backpack - 16) / 10 + 1; return 100 * No * No - 200 * No + 200; }

static void store_tab(Player *p, int tab, const std::string &select = "")
{
    static const char *desc[] = {"`2Welcome to the Growtopia Store!`` Pick a tab to browse.", "`2Locks And Stuff!``  Select the item you'd like more info on, or BACK to go back.",
        "`2Item Packs!``  Select the item you'd like more info on, or BACK to go back.", "`2Awesome Items!``  Select the item you'd like more info on, or BACK to go back.",
        "`2Weather Machines!``  Select the item you'd like more info on, or BACK to go back.", "", ""};
    std::string text = desc[tab];
    if (tab == 5) text = "`2Spend your Growtokens!`` (You have `5" + std::to_string(inv_count(*p->acct, 1486)) + "``) You earn Growtokens from Crazy Jim and Sales-Man. Select the item you'd like more info on, or BACK to go back.";
    if (tab == 6) text = "`2Spend your Vouchers!`` (You have `50``). Select the item you'd like more info on, or BACK to go back.";
    std::string s = "set_description_text|" + text + "\nenable_tabs|1\n";
    const char *names[][2] = {{"main_menu", "Home"}, {"locks_menu", "Locks And Stuff"}, {"itempack_menu", "Item Packs"}, {"bigitems_menu", "Awesome Items"}, {"weather_menu", "Weather Machines"}, {"token_menu", "Growtoken Items"}, {"vouchers_menu", "Voucher Items"}};
    const int order[] = {0, 1, 3, 4, 5, 2, 6}; // the slot each tab button is drawn in (as the real store lays them out)
    for (int i = 0; i < 7; ++i) s += std::string("add_tab_button|") + names[i][0] + "|" + names[i][1] + "|interface/large/btn_shop2.rttex||" + (i == tab ? "1" : "0") + "|" + std::to_string(order[i]) + "|0|0||||-1|-1|||0|0|CustomParams:|\n";
    static const std::set<std::string> featured = {"world_lock", "world_lock_10_pack", "small_lock", "big_lock", "huge_lock", "builders_lock", "upgrade_backpack", "grow_spray_5_pack", "deluxe_grow_spray"};
    for (Product &pr : g_products)
        if ((tab == 0 ? featured.count(pr.btn) > 0 : pr.tab == tab) && !(pr.btn == "upgrade_backpack" && p->acct->backpack >= 400))
            s += "add_button|" + pr.btn + "|" + pr.name + "|" + pr.rttx + "|" + pr.desc + "|" + std::to_string(pr.t1) + "|" + std::to_string(pr.t2) + "|" +
                 std::to_string(pr.btn == "upgrade_backpack" ? backpack_cost(*p->acct) : pr.cost) + "|0|||-1|-1||-1|-1||1||||||0|0|CustomParams:|\n";
    if (!select.empty()) s += "select_item|" + select + "\n";
    call(p->peer, {"OnStoreRequest", s});
}

static void store_buy(Player *p, const std::string &item, const std::string &select = "")
{
    static const std::map<std::string, int> tabs = {{"main", 0}, {"locks", 1}, {"itempack", 2}, {"bigitems", 3}, {"weather", 4}, {"token", 5}, {"vouchers", 6}};
    auto t = tabs.find(item);
    if (t != tabs.end()) { store_tab(p, t->second, select); return; }
    Account &a = *p->acct;
    for (Product &pr : g_products)
    {
        if (pr.btn != item) continue;
        int cost = pr.btn == "upgrade_backpack" ? backpack_cost(a) : std::abs(pr.cost);
        bool tokens = pr.tab == 5;
        int have = tokens ? inv_count(a, 1486) : (int)std::min<i64>(a.gems, 1 << 30);
        if (have < cost) { call(p->peer, {"OnStorePurchaseResult", "You can't afford `0" + pr.name + "``!  You're `$" + std::to_string(cost - have) + "`` " + (tokens ? "`2Growtokens``" : "Gems") + " short."}); return; }
        int newslots = 0; for (auto &[id, n] : pr.items) if (id != 9412 && !inv_count(a, id)) ++newslots;
        if (newslots > 0 && (int)a.inv.size() + newslots > a.backpack) { call(p->peer, {"OnStorePurchaseResult", "You don't have enough room in your backpack!"}); return; }
        if (tokens) inv_remove(a, 1486, cost); else a.gems -= cost;
        for (auto &[id, n] : pr.items)
        {
            if (id == 9412) { a.backpack += n; continue; } // Increase Backpack: slots, not an item
            int left = n; while (left > 0) { int take = std::min(left, 200 - inv_count(a, id)); if (take <= 0) break; inv_add(a, id, take); left -= take; }
        }
        send_inventory(p); send_gems(p); sfx(p, "audio/cash_register.wav");
        call(p->peer, {"OnStorePurchaseResult", "You've purchased `0" + pr.name + "`` for `$" + std::to_string(cost) + "`` " + (tokens ? "`2Growtokens``" : "Gems") + ".  You have `$" + std::to_string(tokens ? inv_count(a, 1486) : a.gems) + "`` left."});
        return;
    }
}

// ---------------------------------------------------------------- doors, wrench, respawn
static void teleport(Player *p, World &w, float x, float y, int sound = 0)
{
    p->x = x; p->y = y;
    GamePacket h; h.type = 0x1d; h.netID = p->netid; h.posX = x; h.posY = y; h.itemID = sound;
    broadcast(w.name, [&](Player *q) { send_packet(q->peer, h); });
}

static Tile *find_door(World &w, const std::string &id, int *ox = nullptr, int *oy = nullptr)
{
    if (id.empty()) return nullptr;
    for (int i = 0; i < (int)w.tiles.size(); ++i) if (w.tiles[i].fg && w.tiles[i].door_id == id) { if (ox) *ox = i % w.w; if (oy) *oy = i / w.w; return &w.tiles[i]; }
    return nullptr;
}

static void enter_door(Player *p, World &w, GamePacket &pk)
{
    Tile *t = w.at(pk.tileX, pk.tileY); const Item *it = t ? S.items.get(t->fg) : nullptr;
    if (!it) return;
    if (it->type == A_MAIN_DOOR) { leave_world(p); send_world_menu(p); return; }
    if (it->type == 27) { int idx = pk.tileY * w.w + pk.tileX; if (p->rest != idx) { p->rest = idx; call(p->peer, {"SetRespawnPos", U32{(u32)idx}}, p->netid); } return; } // checkpoint
    if (it->type != 2 && it->type != 26 && it->type != 43) return;
    if (t->extra.size() > 3 && (t->extra.back() & 0x08) && !can_modify(w, p, pk.tileX, pk.tileY, false)) { bubble(p, p, "The door is locked."); return; }
    std::string target = t->target, dest_world = target.substr(0, target.find(':')), dest_id = target.find(':') == std::string::npos ? "" : target.substr(target.find(':') + 1);
    if (target.empty()) { teleport(p, w, pk.tileX * 32.f, pk.tileY * 32.f); return; }
    if (dest_world.empty() || upper(dest_world) == w.name)
    {
        int x, y; if (find_door(w, dest_id, &x, &y)) teleport(p, w, x * 32.f, y * 32.f, 1); else teleport(p, w, w.door_x * 32.f, w.door_y * 32.f, 1);
        return;
    }
    join_world(p, dest_world);
    if (World *nw = S.world_of(p)) { int x, y; if (find_door(*nw, dest_id, &x, &y)) teleport(p, *nw, x * 32.f, y * 32.f, 1); }
}

static void lock_dialog(Player *p, World &w, int x, int y)
{
    Tile &t = w.tiles[y * w.w + x];
    std::string owner; { std::lock_guard g(S.store.mu); owner = S.store.name_of(t.lock_owner); }
    std::string xy = "embed_data|tilex|" + std::to_string(x) + "\nembed_data|tiley|" + std::to_string(y) + "\n";
    bool mine = has_access(t, p) && (t.lock_owner == p->acct->user_id || p->acct->role >= 3);
    if (!mine)
    {
        Dialog d; d.label_icon("`w" + S.items.get(t.fg)->name + "``", t.fg).spacer().text("`oOwned by `w" + owner + "``.").quick_exit();
        dialog(p, d.end("lock_info", "", "OK")); return;
    }
    bool is_world = (int)(y * w.w + x) == w.lock_tile;
    std::string s = "set_default_color|`o\nadd_label_with_icon|big|`wEdit " + S.items.get(t.fg)->name + "``|left|" + std::to_string(t.fg) + "|\nadd_popup_name|LockEdit|\nadd_label|small|`wAccess list:``|left\n" + xy + "add_spacer|small|\n";
    if (t.access.empty()) s += "add_label|small|Currently, you're the only one with access.``|left\n";
    { std::lock_guard g(S.store.mu); for (i32 uid : t.access) s += "add_checkbox|acc_" + std::to_string(uid) + "|`w" + S.store.name_of(uid) + "``|1|\n"; }
    s += "add_spacer|small|\nadd_player_picker|playerNetID|`wAdd``|\nadd_checkbox|checkbox_public|Allow anyone to Build and Break|" + std::string(t.flags & T_PUBLIC ? "1" : "0") + "\n";
    if (is_world)
        s += "add_checkbox|checkbox_disable_music|Disable Custom Music Blocks|0\nadd_text_input|tempo|Music BPM|100|3|\nadd_checkbox|checkbox_disable_music_render|Make Custom Music Blocks invisible|0\n"
             "add_smalltext|Your current home world is: " + (p->acct->home.empty() ? std::string("(none)") : p->acct->home) + "|left|\nadd_button|set_as_home_world|Set as Home World|noflags|0|0|\n"
             "add_text_input|minimum_entry_level|World Level: |1|3|\nadd_smalltext|Set minimum world entry level.|\nadd_button|getKey|Get World Key|noflags|0|0|\n";
    dialog(p, s + "end_dialog|lock_edit|Cancel|OK|\n");
}

static void wrench_tile(Player *p, World &w, GamePacket &pk)
{
    Tile *t = w.at(pk.tileX, pk.tileY); const Item *it = t ? S.items.get(t->fg) : nullptr;
    if (!it) return;
    std::string xy = std::to_string(pk.tileX), yy = std::to_string(pk.tileY);
    if (it->type == A_SIGN)
    {
        if (!can_modify(w, p, pk.tileX, pk.tileY, false)) return;
        Dialog d; d.label_icon("`wEdit " + it->name + "``", t->fg).spacer().text("`oWhat would you like to write on this sign?``")
         .input("sign_text", "", t->text, 100).hidden("tilex", xy).hidden("tiley", yy).quick_exit();
        dialog(p, d.end("sign_edit", "Cancel", "OK"));
    }
    else if (it->type == 2 || it->type == 13 || it->type == 26 || it->type == 43)
    {
        if (!can_modify(w, p, pk.tileX, pk.tileY, false) || it->type == A_MAIN_DOOR) return;
        bool open = !(t->extra.size() > 3 && t->extra.back() & 0x08);
        std::string s = "set_default_color|`o\nadd_label_with_icon|big|`wEdit " + it->name + "``|left|" + std::to_string(t->fg) + "|\n"
            "add_text_input|door_name|Label|" + t->text + "|100|\nadd_popup_name|DoorEdit|\nadd_text_input|door_target|Destination|" + t->target + "|24|\n"
            "add_smalltext|Enter a Destination in this format: `2WORLDNAME:ID``|left|\n"
            "add_smalltext|Leave `2WORLDNAME`` blank (:ID) to go to the door with `2ID`` in the `2Current World``.|left|\n"
            "add_text_input|door_id|ID|" + t->door_id + "|11|\nadd_smalltext|Set a unique `2ID`` to target this door as a Destination from another!|left|\n"
            "add_checkbox|checkbox_locked|Is open to public|" + (open ? "1" : "0") + "\nembed_data|tilex|" + xy + "\nembed_data|tiley|" + yy + "\nend_dialog|door_edit|Cancel|OK|\n";
        dialog(p, s);
    }
    else if (it->type == A_LOCK) lock_dialog(p, w, pk.tileX, pk.tileY);
    else if (it->type == 62) vend_wrench(p, w, pk.tileX, pk.tileY);
    else if (it->type == 9) // House Entrance and the other entrances: one checkbox
    {
        if (!can_modify(w, p, pk.tileX, pk.tileY, false)) return;
        dialog(p, "set_default_color|`o\nadd_label_with_icon|big|`wEdit " + it->name + "``|left|" + std::to_string(t->fg) + "|\nadd_checkbox|checkbox_public|Is open to public|" + std::string(t->flags & T_PUBLIC ? "1" : "0") +
                  "\nembed_data|tilex|" + xy + "\nembed_data|tiley|" + yy + "\nend_dialog|gateway_edit|Cancel|OK|\n");
    }
}

static std::string age_days(Account &a) { return std::to_string((now_s() - a.created) / 86400); }
static std::string hours_played(Player *q) { char b[32]; snprintf(b, sizeof b, "%.1f", (q->acct->played + now_s() - q->joined_at) / 3600.0); return b; }

static void wrench_player(Player *p, int netid)
{
    for (Player *q : S.in_world(p->world)) if (q->netid == netid)
    {
        Account &qa = *q->acct; std::string nid = std::to_string(netid);
        World *w = S.world_of(p);
        Dialog d; d.s = "embed_data|netID|" + nid + "\nadd_popup_name|WrenchMenu\nset_default_color|`o\n";
        if (q == p) { profile_self(p); return; }
        else
        {
            d.s += "add_label_with_icon|big|`w" + q->name() + " (`2" + std::to_string(qa.level) + "``)``|left|18|\nembed_data|netID|" + nid + "\n";
            d.s += "add_button|trade|`wTrade``|noflags|0|0|\nadd_button|sendpm|`wSend Message``|noflags|0|0|\n";
            d.text("(No Battle Leash equipped)").text("Your opponent needs a valid license to battle!");
            if (w && q->acct->role < 2 && !world_admin(*w, q))
            {
                if (world_owner(*w, p)) d.s += "add_button|kick|`4Kick``|noflags|0|0|\nadd_button|pull|`5Pull``|noflags|0|0|\nadd_button|worldban|`4World Ban``|noflags|0|0|\n";
            }
            if (!p->acct->friends.count(qa.user_id)) d.s += "add_button|friend_add|`wAdd as friend``|noflags|0|0|\n";
            d.s += "add_button|show_clothes|`wView worn clothes``|noflags|0|0|\nadd_button|ignore_player|`wIgnore Player``|noflags|0|0|\nadd_button|report_player|`wReport Player``|noflags|0|0|\n";
            d.spacer().quick_exit();
            dialog(p, d.end("popup", "", "Continue"));
        }
        return;
    }
}

static void world_action(Player *p, Player *t, const std::string &what);
static void profile_return(Player *p, const std::string &text)
{
    std::string b = field(text, "buttonClicked"); int netid = atoi(field(text, "netID").c_str());
    Player *q = nullptr; for (Player *o : S.in_world(p->world)) if (o->netid == netid) q = o;
    if (b == "trade" && q) trade_request(p, netid);
    else if (b == "friend_add" && q) friend_request(p, netid);
    else if (b == "sendpm" && q)
    {
        Dialog d; d.label_icon("`wMessage " + q->name() + "``", 18).spacer().input("pm_text", "", "", 100).hidden("pm_to", q->name()).quick_exit();
        dialog(p, d.end("pm_send", "Cancel", "Send"));
    }
    else if ((b == "pull" || b == "kick" || b == "worldban") && q) world_action(p, q, b);
    else if (b == "show_clothes" && q)
    {
        Dialog d; d.label_icon("`w" + q->name() + "'s worn clothes``", 18).spacer(); bool any = false;
        for (u16 c : q->acct->clothes) if (c && S.items.get(c)) { d.s += "add_label_with_icon|small|`o" + S.items.get(c)->name + "``|left|" + std::to_string(c) + "|\n"; any = true; }
        if (!any) d.text("`oNothing.``");
        d.quick_exit(); dialog(p, d.end("worn_clothes", "", "OK"));
    }
    else if (features_profile_button(p, b)) {}
    else if (!b.empty() && b != "button_ok") LOG("[wrench] %s pressed %s (not implemented)\n", p->name().c_str(), b.c_str());
}

static void respawn(Player *p)
{
    World *w = S.world_of(p); if (!w) return;
    call(p->peer, {"OnSetFreezeState", U32{2}}, p->netid);
    call(p->peer, {"OnKilled"}, p->netid);
    float x = w->door_x * 32.f, y = w->door_y * 32.f;
    if (p->rest >= 0 && p->rest < (int)w->tiles.size()) { x = (p->rest % w->w) * 32.f; y = (p->rest / w->w) * 32.f; }
    p->x = x; p->y = y;
    call(p->peer, {"OnSetPos", Vec2{x, y}}, p->netid, 1900);
    call(p->peer, {"OnSetFreezeState", U32{0}}, p->netid, 1900);
}

static void dialog_return(Player *p, const std::string &text)
{
    std::string name = field(text, "dialog_name");
    World *w = S.world_of(p);
    auto num = [&](const char *k) { return atoi(field(text, k).c_str()); };
    if (name == "popup") profile_return(p, text);
    else if (name == "pm_send") private_message(p, field(text, "pm_to"), field(text, "pm_text"));
    else if (name == "my_worlds_list") { std::string b = field(text, "buttonClicked"); if (b.rfind("warp_", 0) == 0) join_world(p, b.substr(5)); }
    else if (name == "friends_menu") friends_return(p, text);
    else if (name == "trade_add") trade_add(p, num("itemID"), num("count"));
    else if (name == "drop_item" || name == "trash_item") do_drop(p, num("itemID"), std::max(0, num("count")), name == "trash_item");
    else if (w && name == "lock_edit")
    {
        int x = num("tilex"), y = num("tiley"); Tile *t = w->at(x, y);
        if (!t || !t->fg || !S.items.get(t->fg) || S.items.get(t->fg)->type != A_LOCK) return;
        if (t->lock_owner != p->acct->user_id && p->acct->role < 3) return;
        std::string pressed = field(text, "buttonClicked");
        if (pressed == "set_as_home_world" && (int)(y * w->w + x) == w->lock_tile)
        {
            if (w->owner != p->acct->user_id) { console(p, "`4Only the owner of a locked world can make it their home world.``"); return; }
            p->acct->home = w->name; console(p, "`oHome world set to `w" + w->name + "``."); lock_dialog(p, *w, x, y); return;
        }
        if (pressed == "getKey" && (int)(y * w->w + x) == w->lock_tile)
        {
            if (!w->drops.empty()) { console(p, "`4You must pick up or remove all floating items in the world first.``"); return; }
            if (inv_count(*p->acct, 1424)) { console(p, "`4You already have a World Key.``"); return; }
            inv_add(*p->acct, 1424, 1);
            GamePacket g; g.type = 0x0d; g.itemID = 1424; g.buildRange = 1; send_packet(p->peer, g);
            call(p->peer, {"OnTalkBubble", U32{(u32)p->netid}, "You got a `#World Key``! You can now trade this world to other players.", U32{0}, U32{0}});
            sfx(p, "audio/use_lock.wav"); return;
        }
        std::string pick = field(text, "playerNetID");
        if (!pick.empty()) // the player picker answers at once
        {
            for (Player *q : S.in_world(p->world)) if (q->netid == atoi(pick.c_str()) && q != p && !has_access(*t, q))
            { auto acc = t->access; acc.push_back(q->acct->user_id); w->set_lock(*t, t->lock_owner, acc, t->lock_flags); console(p, "`2" + q->name() + "`` was given access."); }
            w->dirty = true; sync_tile(*w, x, y); lock_dialog(p, *w, x, y); return;
        }
        std::vector<i32> keep; for (i32 uid : t->access) if (field(text, "acc_" + std::to_string(uid)) != "0") keep.push_back(uid);
        if (field(text, "checkbox_public") == "1") t->flags |= T_PUBLIC; else t->flags &= ~T_PUBLIC;
        w->set_lock(*t, t->lock_owner, keep, t->lock_flags);
        w->dirty = true; sync_tile(*w, x, y);
        console(p, "`oLock settings saved.``");
    }
    else if (w && name == "gateway_edit")
    {
        Tile *t = w->at(num("tilex"), num("tiley")); const Item *it = t ? S.items.get(t->fg) : nullptr;
        if (!it || it->type != 9 || !can_modify(*w, p, num("tilex"), num("tiley"), false)) return;
        if (field(text, "checkbox_public") == "1") t->flags |= T_PUBLIC; else t->flags &= ~T_PUBLIC;
        w->dirty = true; sync_tile(*w, num("tilex"), num("tiley"));
    }
    else if (w && (name == "sign_edit" || name == "door_edit"))
    {
        Tile *t = w->at(num("tilex"), num("tiley")); const Item *it = t ? S.items.get(t->fg) : nullptr;
        if (!it || !can_modify(*w, p, num("tilex"), num("tiley"), false)) return;
        if (name == "sign_edit" && it->type == A_SIGN) World::set_sign(*t, field(text, "sign_text").substr(0, 100));
        else if (name == "door_edit" && (it->type == 2 || it->type == 26 || it->type == 43))
        {
            World::set_door(*t, field(text, "door_name").substr(0, 100), field(text, "checkbox_locked") == "0");
            t->target = upper(field(text, "door_target")).substr(0, 24);
            t->door_id = field(text, "door_id").substr(0, 11);
            if (!t->door_id.empty() && t->door_id.size() <= 3) // the real server warns about short ids
                call(p->peer, {"OnTalkBubble", U32{(u32)p->netid}, "`4Warning:`` That doorID is easy to guess.  People can use a doorID to warp directly in to this point from another world!", U32{0}, U32{0}});
        }
        else return;
        w->dirty = true; sync_tile(*w, num("tilex"), num("tiley"));
    }
    else features_dialog(p, name, text);
}

// ---------------------------------------------------------------- items.dat download
static void send_items(Player *p)
{
    static std::vector<u8> z; // compressed once: the client asks for it whenever its items.dat differs from ours
    if (z.empty())
    {
        uLongf bound = compressBound(S.items.raw.size()); z.resize(bound);
        if (compress2(z.data(), &bound, S.items.raw.data(), S.items.raw.size(), 9) == Z_OK) z.resize(bound); else z.clear();
        LOG("[items] compressed items.dat: %zu -> %zu bytes\n", S.items.raw.size(), z.size());
    }
    GamePacket h; h.type = 0x10;
    if (!z.empty()) { h.itemID = S.items.raw.size(); send_packet(p->peer, h, z); } else { h.itemID = 0; send_packet(p->peer, h, S.items.raw); }
    LOG("[items] sent items.dat to %s (%zu bytes)\n", p->name().c_str(), z.empty() ? S.items.raw.size() : z.size());
}

// ---------------------------------------------------------------- chat and commands
// ---------------------------------------------------------------- moderation (role >= 2): ghost, nuke, ban, kick, summon
static Player *find_player(const std::string &name) { for (Player *q : S.players) if (q->stage == 2 && q->acct && lower(q->name()) == lower(name)) return q; return nullptr; }

static void set_ghost(Player *p, bool on)
{
    if (p->ghost == on) return;
    World *w = S.world_of(p);
    if (on && w) broadcast(p->world, [&](Player *q) { call(q->peer, {"OnRemove", "netID|" + std::to_string(p->netid) + "\n", "pId|" + std::to_string(p->acct->user_id) + "\n"}); }, p);
    p->ghost = on;
    if (!on && w) for (Player *q : S.in_world(p->world)) if (q != p) { call(q->peer, {"OnSpawn", spawn_text(p, false)}); send_clothing(q, p); }
    send_clothing(p, p);
    console(p, on ? "`2Ghost mode ON.`` Nobody can see you in worlds, counts or friend lists." : "`4Ghost mode OFF.`` You are visible again.");
}

static void kick_to_menu(Player *q, const std::string &why)
{
    leave_world(q); console(q, why); send_world_menu(q);
}

// ---------------------------------------------------------------- world owner tools: pull, kick, world ban
static void world_action(Player *p, Player *t, const std::string &what)
{
    World *w = S.world_of(p); if (!w || t->world != p->world || t == p) return;
    if (!world_owner(*w, p)) { console(p, "`4Only the world's owner or a moderator can do that.``"); return; } // admins (access list) can't pull, kick or ban
    if (t->acct->role >= 2 || world_admin(*w, t)) { console(p, "`4You can't do that to `w" + t->name() + "``.``"); return; }
    if (what == "pull")
    {
        t->x = p->x; t->y = p->y;
        GamePacket st; st.type = 0x00; st.netID = t->netid; st.posX = p->x; st.posY = p->y; st.flags = S_UPDATE; // a server state packet with the snap flag moves the others' view
        broadcast(w->name, [&](Player *q) { if (q != t) send_packet(q->peer, st, {}, false); });
        call(t->peer, {"OnSetPos", Vec2{p->x, p->y}}, t->netid);
        broadcast(w->name, [&](Player *q) { console(q, p->name() + " `5pulls`` " + t->name() + "!"); sfx_at(q, t, "audio/object_spawn.wav"); });
    }
    else if (what == "kick")
    {
        broadcast(w->name, [&](Player *q) { console(q, p->name() + " `4kicks`` " + t->name() + "!"); call(q->peer, {"OnPlayPositioned", "audio/teleport.wav"}, t->netid, 2020); });
        respawn(t);
    }
    else if (what == "worldban")
    {
        w->banned[t->acct->user_id] = now_s() + 3600; w->dirty = true;
        sfx(p, "audio/repair.wav");
        broadcast(w->name, [&](Player *q) { console(q, p->name() + " `4world bans`` " + t->name() + " from `w" + w->name + "``!"); });
        kick_to_menu(t, "`4You were banned from `w" + w->name + "`` for one hour.``");
        console(p, "You've banned " + t->name() + " from `w" + w->name + "`` for one hour.  You can also type `#/uba`` to unban him/her early.");
    }
}

static void nuke(Player *p, std::string name, bool on)
{
    name = upper(clean_name(name));
    if (!valid_world_name(name)) { console(p, "`4Give a world name.``"); return; }
    World *w = load_world(name); w->nuked = on; w->dirty = true;
    if (on) for (Player *q : S.in_world(name)) if (q->acct->role < 2) kick_to_menu(q, "`4This world was nuked by a moderator.``");
    console(p, std::string(on ? "`2Nuked " : "`2Un-nuked ") + "`w" + name + "``.");
    LOG("[mod] %s %s %s\n", p->name().c_str(), on ? "nuked" : "un-nuked", name.c_str());
}

static void ban(Player *p, const std::string &who, const std::string &dur, const std::string &reason)
{
    std::lock_guard g(S.store.mu);
    Account *a = S.store.find(who);
    if (!a) { console(p, "`4No such account.``"); return; }
    if (a->role >= 2 && p->acct->role < 3) { console(p, "`4You can't ban a moderator.``"); return; }
    double hours = dur == "perm" || dur == "0" ? 0 : atof(dur.c_str());
    a->ban_until = hours <= 0 ? -1 : now_s() + (i64)(hours * 3600); a->ban_reason = reason;
    S.store.save(*a);
    if (Player *t = find_player(a->name)) { call(t->peer, {"OnConsoleMessage", ban_text(*a)}); enet_peer_disconnect_later(t->peer, 0); }
    console(p, "`2Banned `w" + a->name + "``" + (hours <= 0 ? " permanently." : " for " + dur + " hour(s)."));
    LOG("[mod] %s banned %s (%s) %s\n", p->name().c_str(), a->name.c_str(), dur.c_str(), reason.c_str());
}

static void command(Player *p, const std::string &line)
{
    std::istringstream in(line.substr(1)); std::string cmd; in >> cmd; cmd = lower(cmd);
    Account &a = *p->acct;
    if (cmd == "help") console(p, "Commands: `w/help /who /msg <name> <text> /warp <world> /me <text>`` World owners: `w/pull /kick /worldban /worldunban <name>``" + std::string(a.role >= 2 ? " `p(moderator)`` `w/ghost /noclip /nuke [world] /unnuke <world> /ban <name> <hours|perm> [reason] /unban <name> /disconnect <name> /summon <name> /warpto <name> /broadcast <text>``" : "") + std::string(a.role >= 3 ? " `4(admin)`` `w/give <id> [n] /giveto <name> <id> [n] /gems <n> /weather <id>``" : ""));
    else if (cmd == "msg" || cmd == "m") { std::string who, t; in >> who; std::getline(in, t); private_message(p, who, t.empty() ? t : t.substr(1)); }
    else if (a.role >= 2 && cmd == "ghost") set_ghost(p, !p->ghost);
    else if (a.role >= 2 && (cmd == "nuke" || cmd == "unnuke")) { std::string n; in >> n; nuke(p, n.empty() ? p->world : n, cmd == "nuke"); }
    else if (a.role >= 2 && cmd == "ban") { std::string who, dur, reason; in >> who >> dur; std::getline(in, reason); if (!reason.empty()) reason.erase(0, 1); ban(p, who, dur.empty() ? "0" : dur, reason); }
    else if (a.role >= 2 && cmd == "unban")
    {
        std::string who; in >> who; std::lock_guard g(S.store.mu); Account *t = S.store.find(who);
        if (t) { t->ban_until = 0; t->ban_reason.clear(); S.store.save(*t); console(p, "`2Unbanned `w" + t->name + "``."); } else console(p, "`4No such account.``");
    }
    else if (a.role >= 2 && cmd == "noclip")
    {
        p->state ^= 1;
        if (p->world.empty()) return;
        if (p->ghost) send_clothing(p, p); else broadcast(p->world, [&](Player *q) { send_clothing(q, p); });
        console(p, (p->state & 1) ? "`2Noclip ON.`` You are in ghost form and can walk through blocks (you can't punch or pick up while it's on)." : "`4Noclip OFF.``");
    }
    else if (cmd == "uba")
    {
        World *w = S.world_of(p);
        if (!w || !world_owner(*w, p)) { console(p, "`4Only the world's owner can do that.``"); return; }
        w->banned.clear(); w->dirty = true; console(p, "You've unbanned everybody from this world.");
    }
    else if (cmd == "trade") { std::string who; in >> who; Player *t = find_player(who); if (t && t->world == p->world && t != p) trade_request(p, t->netid); else console(p, "`4That player isn't in this world.``"); }
    else if (cmd == "fav")
    {
        if (p->world.empty()) return;
        auto &f = a.favs; auto it = std::find(f.begin(), f.end(), p->world);
        if (it != f.end()) { f.erase(it); console(p, "`oRemoved `w" + p->world + "`` from your favorite worlds.``"); }
        else if (f.size() >= 10) console(p, "`4You can only have 10 favorite worlds.``");
        else { f.push_back(p->world); console(p, "`oAdded `w" + p->world + "`` to your favorite worlds.``"); }
    }
    else if (cmd == "sethome")
    {
        World *w = S.world_of(p);
        if (!w || w->owner != a.user_id) console(p, "`4Only the owner of a locked world can make it their home world.``");
        else { a.home = p->world; console(p, "`oHome world set to `w" + p->world + "``."); }
    }
    else if (cmd == "home") { if (a.home.empty()) console(p, "`oYou haven't set a home world. Use `$/sethome`` in a world you own.``"); else join_world(p, a.home); }
    else if (cmd == "pull" || cmd == "kick" || cmd == "worldban" || cmd == "worldunban")
    {
        std::string who; in >> who; World *w = S.world_of(p); Player *t = find_player(who);
        if (cmd == "worldunban")
        {
            if (!w || !world_owner(*w, p)) { console(p, "`4Only the world's owner can do that.``"); return; }
            std::lock_guard g(S.store.mu); Account *ta = S.store.find(who);
            if (ta && w->banned.erase(ta->user_id)) { w->dirty = true; console(p, "`2Un-banned `w" + ta->name + "`` from this world."); } else console(p, "`4That player isn't banned here.``");
        }
        else if (!t || !w || t->world != p->world) console(p, "`4That player isn't in this world.``");
        else world_action(p, t, cmd);
    }
    else if (a.role >= 2 && cmd == "disconnect") { std::string who; in >> who; if (Player *t = find_player(who)) { if (t->acct->role >= 2 && a.role < 3) console(p, "`4You can't kick a moderator.``"); else { call(t->peer, {"OnConsoleMessage", "`4You were kicked by a moderator.``"}); enet_peer_disconnect_later(t->peer, 0); console(p, "`2Kicked.``"); } } else console(p, "`4That player isn't online.``"); }
    else if (a.role >= 2 && cmd == "summon") { std::string who; in >> who; Player *t = find_player(who); if (t && !p->world.empty()) { join_world(t, p->world); console(p, "`2Summoned `w" + t->name() + "``."); } else console(p, "`4That player isn't online.``"); }
    else if (a.role >= 2 && cmd == "warpto") { std::string who; in >> who; Player *t = find_player(who); if (t && !t->world.empty()) join_world(p, t->world); else console(p, "`4That player isn't online or in a world.``"); }
    else if (a.role >= 2 && cmd == "broadcast") { std::string t; std::getline(in, t); for (Player *q : S.players) if (q->stage == 2) { console(q, "`5** Announcement from `p@" + p->name() + "`5:`` " + t); sfx(q, "audio/gong.wav"); } }
    else if (cmd == "who") { std::string s = "In this world:"; for (Player *q : S.in_world(p->world)) if (!q->ghost || q == p) s += " `w" + q->name() + "``"; console(p, s); }
    else if (cmd == "warp") { std::string n; in >> n; join_world(p, n); }
    else if (cmd == "me") { std::string t; std::getline(in, t); broadcast(p->world, [&](Player *q) { console(q, "`6* " + p->name() + t + "``"); }); }
    else if (cmd == "give" && a.role >= 3) { int id = 0, n = 1; in >> id >> n; if (S.items.get(id) && id > 0) { inv_add(a, id, n); send_inventory(p); console(p, "Gave `w" + std::to_string(n) + " " + S.items.get(id)->name + "``."); } else console(p, "No such item."); }
    else if (cmd == "giveto" && a.role >= 3)
    {
        std::string who; int id = 0, n = 1; in >> who >> id >> n;
        for (Player *q : S.players) if (q->acct && lower(q->name()) == lower(who) && S.items.get(id) && id > 0) { inv_add(*q->acct, id, n); send_inventory(q); console(p, "Gave."); return; }
        console(p, "No such player or item.");
    }
    else if (cmd == "gems" && a.role >= 3) { long n = 0; in >> n; a.gems += n; send_gems(p); }
    else if (cmd == "weather" && a.role >= 3) { int id = 0; in >> id; if (World *w = S.world_of(p)) { w->weather = id; w->dirty = true; broadcast(p->world, [&](Player *q) { call(q->peer, {"OnSetCurrentWeather", U32{(u32)id}}); }); } }
    else console(p, "`4Unknown command.`` Enter `$/help`` for a list of valid commands.");
}

static void chat(Player *p, std::string text)
{
    while (!text.empty() && text.back() == '\n') text.pop_back();
    if (text.empty() || p->world.empty()) return;
    if (text[0] == '/') { command(p, text); return; }
    std::string name = p->name(), shown = base_name(S.world_of(p), p);
    broadcast(p->world, [&](Player *q) { call(q->peer, {"OnTalkBubble", U32{(u32)p->netid}, "CP:0_PL:0_OID:_player_chat=" + text, U32{0}}); });
    (void)name; (void)shown;
}

// ---------------------------------------------------------------- text messages
static void kick_duplicate(Player *p)
{
    for (Player *q : S.players) if (q != p && q->acct == p->acct) { enet_peer_disconnect_later(q->peer, 0); q->acct = nullptr; q->stage = -1; }
}

static void on_login_text(Player *p, const std::string &text)
{
    p->client_proto = atoi(field(text, "protocol").c_str());
    std::string ltoken = field(text, "ltoken");
    if (!ltoken.empty()) // step 3: login server, the token comes from the login page
    {
        std::lock_guard g(S.store.mu);
        Account *a = S.store.by_web_token(ltoken);
        if (!a) { send_action(p->peer, "logon_fail"); return; }
        if (is_banned(*a)) { send_action(p->peer, "log", "msg|" + ban_text(*a)); send_action(p->peer, "logon_fail"); return; }
        std::string uuid; i32 t = S.store.make_transfer(a->name, uuid);
        call(p->peer, {"OnSendToServer", S.cfg.public_port ? S.cfg.public_port : S.cfg.port, t, a->user_id, S.cfg.ip + "|0|" + uuid, 1, a->name});
        return;
    }
    std::string user = field(text, "user"), token = field(text, "token");
    if (!user.empty() && !token.empty()) // step 4: game server, the transfer token from OnSendToServer
    {
        std::lock_guard g(S.store.mu);
        Account *a = S.store.take_transfer(atoi(token.c_str()), field(text, "UUIDToken"));
        if (!a || a->user_id != atoi(user.c_str())) { send_action(p->peer, "logon_fail"); return; }
        if (is_banned(*a)) { send_action(p->peer, "log", "msg|" + ban_text(*a)); send_action(p->peer, "logon_fail"); return; }
        p->acct = a; p->stage = 2; p->joined_at = now_s(); kick_duplicate(p);
        LOG("[login] %s (user %d) from %s\n", a->name.c_str(), a->user_id, "client");
        send_login_preamble(p);
        call(p->peer, {"OnSuperMainStartAcceptLogonHrdxs47254722215a", U32{S.items.hash}, S.cfg.cdn_host, S.cfg.cdn_path,
                       "cc.cz.madkite.freedom org.aqua.gg idv.aqua.bulldog com.cih.gamecih2 com.cih.gamecih com.cih.game_cih cn.maocai.gamekiller com.gmd.speedtime org.dax.attack com.x0.strai.frep com.x0.strai.free org.cheatengine.cegui org.sbtools.gamehack com.skgames.traffikrider org.sbtoods.gamehaca com.skype.ralder org.cheatengine.cegui.xx.multi1458919170111 com.prohiro.macro me.autotouch.autotouch com.cygery.repetitouch.free com.cygery.repetitouch.pro com.proziro.zacro com.slash.gamebuster",
                       "proto=" + std::to_string(S.cfg.protocol) + "|choosemusic=audio/mp3/about_theme.mp3|active_holiday=0|wing_week_day=0|ubi_week_day=0|server_tick=" + std::to_string(now_ms() & 0x7fffffff) +
                       "|clash_active=0|drop_lavacheck_faster=1|isPayingUser=0|usingStoreNavigation=1|enableInventoryTab=1|bigBackpack=1|m_clientBits=" + std::to_string(a->cbits),
                       U32{0}});
        return;
    }
    send_action(p->peer, "logon_fail");
}

static void enter_game(Player *p)
{
    Account &a = *p->acct;
    int fon = 0; for (i32 uid : a.friends) if (online_user(uid)) ++fon;
    send_emoticons(p);
    console(p, "Welcome back, `w`w" + a.name + "````. " + (fon == 1 ? "`w1`` friend is online." : fon ? "`w" + std::to_string(fon) + "`` friends are online." : "No friends are online."));
    friend_alert(p, true);
    if (a.cbits & 0x400) console(p, "`5Personal Settings active:`` `#Punch Joystick enabled``");
    call(p->peer, {"OnPaw2018SkinColor1Changed", 0});
    call(p->peer, {"OnPaw2018SkinColor2Changed", 0});
    send_inventory(p);
    send_gems(p);
    call(p->peer, {"OnSetPearl", 0, 1});
    call(p->peer, {"OnSetVouchers", 0});
    call(p->peer, {"SetHasGrowID", U32{1}, a.name, ""});
    call(p->peer, {"SetHasAccountSecured", U32{1}});
    time_t tt = time(nullptr); tm lt; localtime_r(&tt, &lt);
    call(p->peer, {"OnTodaysDate", lt.tm_mon + 1, lt.tm_mday, U32{a.xp}, U32{a.level}});
    call(p->peer, {"SetShowChatOnlyFromFriends", U32{a.only_friends}});
    call(p->peer, {"OnRequestHoustonPricing"});
    send_world_menu(p);
    call(p->peer, {"OnClearItemTransforms"}, 0, 250);
    GamePacket ping; ping.type = 0x16; send_packet(p->peer, ping);
    call(p->peer, {"OnSetFeatureEnableFlags", FEATURE_FLAGS});
}

#include "features.hpp"
static std::string offer_text_fwd(const std::map<int, int> &o) { return offer_text(o); }

static void on_action(Player *p, const std::string &text)
{
    std::string action = field(text, "action");
    if (action == "enter_game") { enter_game(p); }
    else if (action == "refresh_item_data") { console(p, "One moment, updating item data..."); send_items(p); }
    else if (action == "join_request") join_world(p, field(text, "name"));
    else if (action == "quit_to_exit") { leave_world(p); send_world_menu(p); }
    else if (action == "quit") enet_peer_disconnect_later(p->peer, 0);
    else if (action == "dialog_return") dialog_return(p, text);
    else if (action == "trade_started") trade_refresh(p);
    else if (action == "friends") social_portal(p);
    else if (action == "store") store_tab(p, 0);
    else if (action == "killstore") {}
    else if (action == "storenavigate" || action == "buy") store_buy(p, field(text, "item"), field(text, "selection"));
    else if (action == "growid") { Dialog d; d.label_icon("`wYour GrowID``", 18).spacer().text("`oYou are logged in as `w" + p->name() + "``."); d.quick_exit(); dialog(p, d.end("growid_info", "", "OK")); }
    else if (action == "gohomeworld") { if (!p->acct->my_worlds.empty()) join_world(p, p->acct->my_worlds.front()); else { console(p, "`oYou don't own a world yet.``"); call(p->peer, {"OnFailedToEnterWorld"}); } }
    else if (action == "setFilter") { p->acct->cbits = (u32)strtoul(field(text, "cbits").c_str(), nullptr, 10); }
    else if (action == "setOnlyFriendsChat") { p->acct->only_friends = atoi(field(text, "show").c_str()) != 0; }
    else if (action == "setchatFilter" || action == "setGDPR" || action == "setDeath" || action == "setRespawn" ||
             action == "setRoleSkin" || action == "setRoleIcon" || action == "world_button" || action == "refresh_player_tribute_data" || action == "get_seed_diary_data" || action == "itemfavourite") {} // settings the client keeps itself
    else if (action == "mod_trade")
    {
        int id = atoi(field(text, "itemID").c_str()); const Item *it = S.items.get(id);
        if (p->trade_with && p->trade_open && it && inv_count(*p->acct, id) == 1) trade_add(p, id, 1);
        else if (p->trade_with && p->trade_open && it && inv_count(*p->acct, id) > 0)
        {
            Dialog d; d.label_icon("`w" + it->name + "``", id).text("`oHow many to offer? (you have " + std::to_string(inv_count(*p->acct, id)) + ")``")
             .input("count", "", "1", 5).hidden("itemID", std::to_string(id)).quick_exit();
            dialog(p, d.end("trade_add", "Cancel", "OK"));
        }
    }
    else if (action == "rem_trade") { if (p->trade_with && p->trade_open) { p->offer.erase(atoi(field(text, "itemID").c_str())); p->accepted = p->trade_with->accepted = false; trade_refresh(p); } }
    else if (action == "trade_accept") trade_accept(p, text);
    else if (action == "trade_cancel") trade_end(p, "`4" + p->name() + " cancelled the trade.``");
    else if (action == "trade_lock" || action == "trade_check_price") {}
    else if (action == "drop") drop_dialog(p, atoi(field(text, "itemID").c_str()), false);
    else if (action == "trash") drop_dialog(p, atoi(field(text, "itemID").c_str()), true);
    else if (action == "info") info_dialog(p, atoi(field(text, "itemID").c_str()));
    else if (action == "wrench") wrench_player(p, atoi(field(text, "netid").c_str()));
    else if (action == "respawn" || action == "respawn_spike") respawn(p);
    else if (action == "setSkin") { p->acct->skin = (u32)strtoull(field(text, "color").c_str(), nullptr, 10); if (!p->world.empty()) broadcast(p->world, [&](Player *q) { send_clothing(q, p); }); }
    else if (action == "input") { size_t k = text.find("|text|"); chat(p, k == std::string::npos ? "" : text.substr(k + 6)); }
    else if (features_action(p, action, text)) {}
    else LOG("[action] %s unhandled: %s\n", p->name().c_str(), action.c_str());
}

// ---------------------------------------------------------------- game packets
static void on_packet(Player *p, const u8 *d, size_t n)
{
    if (n < sizeof(GamePacket)) return;
    GamePacket pk; memcpy(&pk, d, sizeof pk);
    World *w = S.world_of(p);
    switch (pk.type)
    {
        case 0x00: // movement: stamp our netID and relay
        {
            if (!w) return;
            p->x = pk.posX; p->y = pk.posY; p->left = pk.flags & S_LEFT;
            pk.netID = p->netid;
            if (!p->ghost) broadcast(p->world, [&](Player *q) { send_packet(q->peer, pk, {}, false); }, p);
            break;
        }
        case 0x03: if (w && p->acct->clothes[5] == 5480) LOG("[punch] %s item %d tile %d,%d pos %.0f,%.0f flags %x\n", p->name().c_str(), pk.itemID, pk.tileX, pk.tileY, pk.posX, pk.posY, pk.flags);
                   if (w) { if (pk.itemID == 18) punch(p, *w, pk); else if (pk.itemID == 32) wrench_tile(p, *w, pk); else place(p, *w, pk); } break;
        case 0x07: if (w) enter_door(p, *w, pk); break;
        case 0x0a: if (!convert_locks(p, pk.itemID)) wear(p, pk.itemID); break;
        case 0x0b: if (w) pickup(p, *w, pk); break;
        case 0x12: if (w && !p->ghost) { pk.netID = p->netid; broadcast(p->world, [&](Player *q) { send_packet(q->peer, pk); }, p); } break; // the icon state (busy, typing...) goes to the others
        case 0x16: break;
        case 0x15: case 0x1a: case 0x18: break;
        default: LOG("[packet] %s sent unhandled game packet 0x%02x\n", p->name().c_str(), pk.type);
    }
}

static void on_receive(ENetEvent &ev)
{
    Player *p = S.player_of(ev.peer);
    if (!p || ev.packet->dataLength < 4) return;
    const u8 *d = ev.packet->data; size_t n = ev.packet->dataLength;
    u32 type; memcpy(&type, d, 4);
    if (type == GAME_PACKET) { if (p->stage == 2) on_packet(p, d + 4, n - 4); return; }
    if (type != TEXT && type != GAME_MSG) return;
    std::string text((const char *)d + 4, n - 4);
    while (!text.empty() && text.back() == '\0') text.pop_back();
    if (p->stage == 2) { if (!text.empty()) on_action(p, text); }
    else if (p->stage == 0) on_login_text(p, text);
}

// ---------------------------------------------------------------- operator console: lines in data/commands.txt are run once, then the file is removed
//   giveto <name> <id> [n]     gems <name> <n>     passwd <name> <new password>
static void run_command_file(const std::string &path)
{
    static i64 last = 0; if (now_ms() - last < 1000) return; last = now_ms();
    std::ifstream f(path); if (!f) return;
    std::vector<std::string> lines; for (std::string l; std::getline(f, l); ) lines.push_back(l);
    f.close(); std::filesystem::remove(path);
    for (auto &l : lines)
    {
        std::istringstream in(l); std::string cmd, who; in >> cmd >> who;
        if (cmd == "passwd")   // passwd <name> <new password>
        {
            std::string pw; in >> pw; std::lock_guard g(S.store.mu); Account *acc = S.store.find(who);
            if (!acc || pw.size() < 6) { LOG("[console] passwd: no such account or password under 6 characters\n"); continue; }
            acc->salt = random_hex(16); acc->hash = Store::pbkdf2(pw, acc->salt); S.store.save(*acc);
            LOG("[console] password changed for %s\n", acc->name.c_str()); continue;
        }
        Player *t = nullptr; for (Player *q : S.players) if (q->stage == 2 && q->acct && lower(q->name()) == lower(who)) t = q;
        if (!t) { LOG("[console] %s is not online: %s\n", who.c_str(), l.c_str()); continue; }
        if (cmd == "giveto") { int id = 0, n = 1; in >> id >> n; if (S.items.get(id) && id > 0) { inv_add(*t->acct, id, n); send_inventory(t); console(t, "`2You received `w" + std::to_string(n) + " " + S.items.get(id)->name + "``.``"); LOG("[console] gave %d x%d to %s\n", id, n, who.c_str()); } }
        else if (cmd == "gems") { long n = 0; in >> n; t->acct->gems += n; send_gems(t); LOG("[console] gems %ld to %s\n", n, who.c_str()); }
        else LOG("[console] unknown: %s\n", l.c_str());
    }
}

// ---------------------------------------------------------------- main
static void save_all()
{
    for (auto &[n, w] : S.worlds) if (w.dirty) { w.save(S.cfg.data_dir + "/worlds", S.items); w.dirty = false; }
    std::lock_guard g(S.store.mu);
    for (Player *p : S.players) if (p->acct) S.store.save(*p->acct);
}

int main(int argc, char **argv)
{
    setvbuf(stdout, nullptr, _IOLBF, 0);
    S.cfg.load(argc > 1 ? argv[1] : "gtps.conf");
    S.store.dir = S.cfg.data_dir; S.store.index_names();
    signal(SIGINT, [](int) { g_stop = 1; }); signal(SIGTERM, [](int) { g_stop = 1; });
    if (!S.items.load(S.cfg.items_path)) { fprintf(stderr, "can't read %s (items.dat v1..27)\n", S.cfg.items_path.c_str()); return 1; }
    load_store(S.cfg.store_path); load_punch(S.cfg.punch_path); load_achievements(); load_mods(S.cfg.res_dir + "/mods.txt");
    printf("items.dat v%d, %zu items, hash %u\n", S.items.version, S.items.list.size(), S.items.hash);

    enet_initialize();
    ENetAddress addr{.type = ENET_ADDRESS_TYPE_IPV4, .port = (enet_uint16)S.cfg.port};
    if (!S.cfg.bind_ip.empty()) enet_address_set_host_ip(&addr, S.cfg.bind_ip.c_str());
    S.host = enet_host_create(ENET_ADDRESS_TYPE_IPV4, &addr, 64, 2, 0, 0);
    if (!S.host) { fprintf(stderr, "can't listen on enet port %d\n", S.cfg.port); return 1; }
    S.host->usingNewPacketForServer = true; S.host->checksum = enet_crc32;
    enet_host_compress_with_range_coder(S.host);
    std::thread(https::listener, std::ref(S.cfg), std::ref(S.store)).detach();
    printf("enet listening on :%d, clients are sent to %s\n", S.cfg.port, S.cfg.ip.c_str());

    i64 last_save = now_s();
    ENetEvent ev;
    while (!g_stop)
    {
        while (enet_host_service(S.host, &ev, 100) > 0)
            switch (ev.type)
            {
                case ENET_EVENT_TYPE_CONNECT: { Player *p = new Player; p->peer = ev.peer; ev.peer->data = p; S.players.push_back(p); send_hello(ev.peer); break; }
                case ENET_EVENT_TYPE_RECEIVE: on_receive(ev); enet_packet_destroy(ev.packet); break;
                case ENET_EVENT_TYPE_DISCONNECT:
                {
                    Player *p = S.player_of(ev.peer); if (!p) break;
                    if (p->acct) { if (p->stage == 2) { friend_alert(p, false); p->acct->played += now_s() - p->joined_at; p->acct->last_seen = now_s(); } leave_world(p); std::lock_guard g(S.store.mu); S.store.save(*p->acct); LOG("[logout] %s\n", p->name().c_str()); }
                    S.players.erase(std::remove(S.players.begin(), S.players.end(), p), S.players.end());
                    delete p; ev.peer->data = nullptr; break;
                }
                default: break;
            }
        if (now_s() - last_save >= 60) { save_all(); last_save = now_s(); }
        run_command_file(S.cfg.data_dir + "/commands.txt");
    }
    for (Player *p : S.players) if (p->acct) leave_world(p);
    save_all();
    puts("bye");
}
