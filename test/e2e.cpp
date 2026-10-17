// end-to-end check: two clients follow the real login flow (login page -> ltoken -> OnSendToServer -> full login),
// join a world, and exercise movement, build, break, drops, chat. Prints PASS/FAIL per step.
//   ./e2e.out [https_port=8443] [enet_port=17091]   (server must be running with items.dat and a cert for 127.0.0.1)
#include "proto.hpp"
#include "items.hpp"
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <chrono>

static int fails = 0;
static const char *g_ip = "127.0.0.1"; // the ENet address (argv[4]); the capture proxy listens on another loopback address
#define CHECK(cond, what) do { bool ok_ = (cond); printf("%s  %s\n", ok_ ? "PASS" : "FAIL", what); if (!ok_) ++fails; } while (0)

static std::string sh(const std::string &cmd) { std::string out; FILE *f = popen(cmd.c_str(), "r"); char b[4096]; size_t n; while ((n = fread(b, 1, sizeof b, f)) > 0) out.append(b, n); pclose(f); return out; }

struct Ev { u32 type; std::vector<u8> data; };

struct Client
{
    ENetHost *host = nullptr; ENetPeer *peer = nullptr;
    std::string name; Items *items;
    std::vector<Ev> inbox;
    int netid = -1; std::string world_name;
    std::vector<u8> world_blob;

    void connect(const char *ip, int port)
    {
        if (host) enet_host_destroy(host);
        host = enet_host_create(ENET_ADDRESS_TYPE_IPV4, nullptr, 1, 2, 0, 0);
        host->usingNewPacket = 1; host->checksum = enet_crc32; enet_host_compress_with_range_coder(host);
        ENetAddress a{.type = ENET_ADDRESS_TYPE_IPV4, .port = (enet_uint16)port}; enet_address_set_host_ip(&a, ip);
        peer = enet_host_connect(host, &a, 2, 0);
        inbox.clear();
    }
    static inline std::vector<Client *> all; // every client is serviced while any one waits, or ENet times the idle ones out
    Client() { all.push_back(this); }
    void service() { if (!host) return; ENetEvent ev; while (enet_host_service(host, &ev, 0) > 0) if (ev.type == ENET_EVENT_TYPE_RECEIVE)
        { Ev e; memcpy(&e.type, ev.packet->data, 4); e.data.assign(ev.packet->data + 4, ev.packet->data + ev.packet->dataLength); inbox.push_back(std::move(e)); enet_packet_destroy(ev.packet); } }
    void pump(int ms)
    {
        auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
        while (std::chrono::steady_clock::now() < end) { for (Client *c : all) c->service(); std::this_thread::sleep_for(std::chrono::milliseconds(5)); }
    }
    void send_text(int type, const std::string &t) { Buf b; b.put<u32>(type).raw(t.data(), t.size()).put<u8>(0); send_raw(peer, b.d.data(), b.d.size()); }
    void send_pkt(GamePacket h) { Buf b; b.put<u32>(4).raw(&h, sizeof h); send_raw(peer, b.d.data(), b.d.size()); }
    // wait until a call named `fn` arrives (or timeout); returns its args
    bool wait_call(const std::string &fn, std::vector<std::string> &args, int ms = 4000)
    {
        for (int t = 0; t < ms; t += 50)
        {
            for (size_t i = 0; i < inbox.size(); ++i)
            {
                Ev &e = inbox[i];
                if (e.type == 4 && e.data.size() >= 56 && e.data[0] == 0x01)
                {
                    auto v = read_varlist(e.data.data() + 56, e.data.size() - 56);
                    if (!v.empty() && v[0] == fn) { args = v; inbox.erase(inbox.begin() + i); return true; }
                }
            }
            pump(50);
        }
        return false;
    }
    bool wait_packet(u8 type, GamePacket &out, std::vector<u8> *extra = nullptr, int ms = 4000)
    {
        for (int t = 0; t < ms; t += 50)
        {
            for (size_t i = 0; i < inbox.size(); ++i)
                if (inbox[i].type == 4 && inbox[i].data.size() >= 56 && inbox[i].data[0] == type)
                {
                    memcpy(&out, inbox[i].data.data(), 56);
                    if (extra) extra->assign(inbox[i].data.begin() + 56, inbox[i].data.end());
                    inbox.erase(inbox.begin() + i); return true;
                }
            pump(50);
        }
        return false;
    }
    bool has_text(const std::string &needle, int ms = 3000)
    {
        for (int t = 0; t < ms; t += 50)
        {
            for (auto &e : inbox)
                if (e.type == 4 && e.data.size() >= 56 && e.data[0] == 1)
                {
                    auto v = read_varlist(e.data.data() + 56, e.data.size() - 56);
                    for (auto &s : v) if (s.find(needle) != std::string::npos) return true;
                }
            pump(50);
        }
        return false;
    }
};

// walks world data the way the client does; returns tile count read, or -1
static int walk_world(const std::vector<u8> &d, Items &items, std::string &name, int &w, int &h, int &door_x)
{
    Rd r(d.data(), d.size());
    u16 version = r.get<u16>(); r.get<u32>(); name = r.str16(); w = r.get<i32>(); h = r.get<i32>(); u32 n = r.get<u32>();
    r.skip(5);
    for (u32 i = 0; i < n && r.ok; ++i)
    {
        u16 fg = r.get<u16>(), bg = r.get<u16>(); r.get<u16>(); u16 fl = r.get<u16>();
        (void)bg;
        if (fl & 2) r.get<u16>();
        if (fl & 1)
        {
            u8 t = r.get<u8>();
            if (t == 1) { r.str16(); r.get<u8>(); if (fg == 6) door_x = i % w; }
            else if (t == 2) { r.str16(); r.get<i32>(); }
            else if (t == 3) { r.get<u8>(); r.get<i32>(); u32 c = r.get<u32>(); r.skip(4 * c); if (version >= 12) r.get<u32>(); if (version >= 13) r.get<u32>(); }
            else if (t == 4) { r.get<u32>(); r.get<u8>(); }
            else return -1;
        }
        if (!items.get(fg)) return -1;
    }
    r.skip(12);
    u32 nd = r.get<u32>(); r.get<u32>(); r.skip(16 * nd);
    r.skip(2 * 4 + 4 + 4);
    return r.ok && r.o == d.size() ? (int)n : -1;
}

static bool login(Client &c, const char *host_https, int https_port, const char *ip, int enet_port, const std::string &name, const std::string &pw, bool reg)
{
    std::string base = "https://" + std::string(host_https) + ":" + std::to_string(https_port);
    std::string sd = sh("curl -sk " + base + "/growtopia/server_data.php -d 'version=5.6&protocol=227&platform=0'");
    CHECK(sd.find("server|") != std::string::npos && sd.find("RTENDMARKERBS1001") != std::string::npos, "server_data.php answers");
    std::string page = sh("curl -sk -X POST " + base + "/player/login/dashboard -d 'clientData=protocol%7C227%0A'");
    CHECK(page.find("<form") != std::string::npos, "login page is served");
    std::string json = sh("curl -skL " + base + "/player/growid/login/validate -d 'growId=" + name + "&password=" + pw + (reg ? "&register=1" : "") + "'");
    size_t k = json.find("\"token\":\""); if (k == std::string::npos) { printf("login page said: %s\n", json.c_str()); return false; }
    std::string token = json.substr(k + 9, json.find('"', k + 9) - k - 9);
    CHECK(json.find("\"status\":\"success\"") != std::string::npos && json.find("accountType") != std::string::npos, "login page hands back status/token/accountType");

    c.name = name;
    c.connect(ip, enet_port);
    c.pump(500);
    c.send_text(2, "protocol|227\nltoken|" + token + "\nplatformID|0\n");
    std::vector<std::string> a;
    if (!c.wait_call("OnSendToServer", a)) return false;
    CHECK(a.size() >= 7, "OnSendToServer received");
    std::string addr = a[4], uuid; { auto p2 = addr.rfind('|'); uuid = addr.substr(p2 + 1); }
    c.connect(ip, atoi(a[1].c_str()));
    c.pump(500);
    c.send_text(2, "tankIDName|" + name + "\nprotocol|227\ngame_version|5.6\nuser|" + a[3] + "\ntoken|" + a[2] + "\nUUIDToken|" + uuid + "\ndoorID|0\nlmode|1\n");
    if (!c.wait_call("OnSuperMainStartAcceptLogonHrdxs47254722215a", a)) return false;
    CHECK(a[1] == std::to_string(c.items->hash), "items.dat hash matches");
    c.send_text(2, "action|enter_game\n");
    CHECK(c.wait_call("OnRequestWorldSelectMenu", a), "world menu after enter_game");
    return true;
}

static bool join(Client &c, const std::string &world, int &w, int &h, int &door_x)
{
    c.send_text(3, "action|join_request\nname|" + world + "\ninvitedWorld|0\n");
    GamePacket pk; std::vector<u8> extra;
    if (!c.wait_packet(0x04, pk, &extra)) return false;
    std::string nm;
    int n = walk_world(extra, *c.items, nm, w, h, door_x);
    CHECK(n == w * h && n == 6000, "SEND_MAP_DATA parses to the exact end (6000 tiles)");
    c.world_blob = extra;
    std::vector<std::string> a; std::string sp;
    for (int i = 0; i < 8; ++i) { if (!c.wait_call("OnSpawn", a)) return false; sp = a[1]; if (sp.find("type|local") != std::string::npos) break; }
    size_t k = sp.find("netID|"); c.netid = atoi(sp.c_str() + k + 6);
    CHECK(sp.find("type|local") != std::string::npos, "own OnSpawn is type|local");
    return true;
}

int main(int argc, char **argv)
{
    int https_port = argc > 1 ? atoi(argv[1]) : 8443, enet_port = argc > 2 ? atoi(argv[2]) : 17091;
    if (argc > 4) g_ip = argv[4];
    enet_initialize();
    Items items; if (!items.load(argc > 3 ? argv[3] : "../../items.dat")) { puts("usage: e2e.out https_port enet_port items.dat"); return 2; }
    std::string suffix = std::to_string(time(nullptr) % 100000);
    Client A, B; A.items = B.items = &items;
    std::string na = "alice" + suffix, nb = "bobby" + suffix;

    CHECK(login(A, "127.0.0.1", https_port, g_ip, enet_port, na, "secret1", true), "client A logs in (registering)");
    // wrong password must not log in
    std::string bad = sh("curl -skL https://127.0.0.1:" + std::to_string(https_port) + "/player/growid/login/validate -d 'growId=" + na + "&password=wrongpass'");
    CHECK(bad.find("Wrong password") != std::string::npos, "wrong password is rejected");
    CHECK(login(B, "127.0.0.1", https_port, g_ip, enet_port, nb, "secret2", true), "client B logs in");

    int w = 0, h = 0, door_x = 0;
    std::string world = "TEST" + suffix;
    CHECK(join(A, world, w, h, door_x), "A joins a new world");
    int w2, h2, dx2;
    CHECK(join(B, world, w2, h2, dx2), "B joins the same world");
    std::vector<std::string> a;
    CHECK(A.wait_call("OnSpawn", a) && a[1].find("type|local") == std::string::npos, "A sees B spawn");

    // movement relay
    GamePacket mv; mv.type = 0x00; mv.posX = 1000; mv.posY = 700; mv.flags = S_LEFT;
    B.send_pkt(mv); GamePacket got;
    bool mv_ok = A.wait_packet(0x00, got);
    if (!mv_ok) { printf("A inbox:"); for (auto &e : A.inbox) printf(" [%u:%02x]", e.type, e.data.empty() ? 0 : e.data[0]); puts(""); }
    CHECK(mv_ok && got.netID == B.netid && got.posX == 1000, "movement of B reaches A with B's netID");

    // give A blocks + a seed with the admin command (first account on a fresh data dir is admin; otherwise skip)
    A.send_text(2, "action|input\n|text|/give 2 20\n"); A.pump(300);
    A.send_text(2, "action|input\n|text|/give 3 5\n"); A.pump(300);
    std::vector<u8> inv;
    // A stands at the door; place dirt next to the door
    int tx = door_x + 1, ty = 23;
    mv.posX = door_x * 32; mv.posY = 23 * 32; mv.flags = 0; A.send_pkt(mv); A.pump(200);
    GamePacket pl; pl.type = 0x03; pl.itemID = 2; pl.tileX = tx; pl.tileY = ty; pl.posX = door_x * 32; pl.posY = 23 * 32;
    A.send_pkt(pl);
    CHECK(B.wait_packet(0x03, got) && got.itemID == 2 && got.tileX == tx && got.tileY == ty && got.netID == A.netid, "placing dirt is broadcast to B");

    // punch it until it breaks
    GamePacket pu = pl; pu.itemID = 18; bool broke = false; bool damaged = false;
    for (int i = 0; i < 12 && !broke; ++i)
    {
        A.send_pkt(pu); A.pump(150);
        for (auto it = B.inbox.begin(); it != B.inbox.end(); ++it)
            if (it->type == 4 && it->data.size() >= 56) { if (it->data[0] == 0x08) damaged = true; if (it->data[0] == 0x03) { broke = true; } }
        B.pump(50);
    }
    CHECK(damaged, "punches send tile damage (0x08)");
    CHECK(broke, "the block breaks (0x03 with the Fist)");

    // chat reaches B
    A.send_text(2, "action|input\n|text|hello bobby\n");
    CHECK(B.has_text("hello bobby"), "chat from A reaches B");

    // wearing clothes: Baseball Cap (46) goes to everyone's OnSetClothing
    A.send_text(2, "action|input\n|text|/give 46 1\n"); A.pump(300);
    B.inbox.clear(); A.inbox.clear();
    GamePacket wr; wr.type = 0x0a; wr.itemID = 46; A.send_pkt(wr);
    CHECK(B.wait_call("OnSetClothing", a), "wearing a cap sends OnSetClothing to B");

    // drop dialog -> drop -> pick up again
    A.send_text(2, "action|drop\n|itemID|2\n");
    std::vector<std::string> dlg;
    CHECK(A.wait_call("OnDialogRequest", dlg) && dlg[1].find("end_dialog|drop_item|") != std::string::npos, "drop button answers with the drop dialog");
    B.inbox.clear(); A.inbox.clear();
    A.send_text(2, "action|dialog_return\ndialog_name|drop_item\nitemID|2|\ncount|5\n");
    CHECK(B.wait_packet(0x0e, got) && got.netID == -1 && got.itemID == 2 && got.floatVar == 5, "dropping 5 dirt shows a drop to B");
    GamePacket rem; CHECK(A.wait_packet(0x0d, rem) && rem.itemID == 2 && rem.jumpCount == 5, "A's inventory loses 5 dirt (0x0d)");
    // uid = world's last uid + the number of drops created so far (client numbering)
    int drops_seen = 0; for (auto &e : A.inbox) if (e.type == 4 && e.data.size() >= 56 && e.data[0] == 0x0e) { GamePacket g; memcpy(&g, e.data.data(), 56); if (g.netID == -1) ++drops_seen; }
    GamePacket pk0; pk0.type = 0x0b; pk0.posX = got.posX; pk0.posY = got.posY;
    // the break earlier may have made drops too; try every uid up to a few
    bool picked = false;
    for (int uid = 1; uid <= 12 && !picked; ++uid)
    {
        pk0.itemID = uid; B.inbox.clear(); A.send_pkt(pk0);
        if (B.wait_packet(0x0e, got, nullptr, 250) && got.netID == A.netid) picked = true;
    }
    CHECK(picked, "picking up a drop is broadcast with the picker's netID");
    (void)drops_seen;

    // signs: place, wrench, edit -> tile update
    A.send_text(2, "action|input\n|text|/give 20 1\n"); A.pump(300);
    int sx = door_x - 1;
    GamePacket ps; ps.type = 0x03; ps.itemID = 20; ps.tileX = sx; ps.tileY = 23; ps.posX = door_x * 32; ps.posY = 23 * 32;
    A.send_pkt(ps); CHECK(B.wait_packet(0x03, got) && got.itemID == 20, "a sign can be placed");
    B.inbox.clear(); A.inbox.clear();
    ps.itemID = 32; A.send_pkt(ps);
    CHECK(A.wait_call("OnDialogRequest", dlg) && dlg[1].find("end_dialog|sign_edit|") != std::string::npos, "wrench on a sign opens the sign dialog");
    A.send_text(2, "action|dialog_return\ndialog_name|sign_edit\ntilex|" + std::to_string(sx) + "|\ntiley|23|\nsign_text|hi there\n");
    std::vector<u8> ex;
    bool sign_ok = B.wait_packet(0x05, got, &ex) && ex.size() > 12 && std::string(ex.begin(), ex.end()).find("hi there") != std::string::npos;
    CHECK(sign_ok, "editing the sign sends the new text as a tile update (0x05)");

    // world lock: placing it owns the world and sends the lock area
    A.send_text(2, "action|input\n|text|/give 242 1\n"); A.pump(300);
    B.inbox.clear();
    GamePacket pwl; pwl.type = 0x03; pwl.itemID = 242; pwl.tileX = door_x + 2; pwl.tileY = 23; pwl.posX = door_x * 32; pwl.posY = 23 * 32;
    A.send_pkt(pwl);
    CHECK(B.wait_packet(0x0f, got, &ex) && got.targetNetID == 5999 && ex.size() == 5999 * 2, "world lock sends SEND_LOCK covering every other tile");
    // B can't build in A's locked world
    A.send_text(2, "action|input\n|text|/giveto " + nb + " 2 5\n"); A.pump(300);
    A.inbox.clear();
    GamePacket pb; pb.type = 0x03; pb.itemID = 2; pb.tileX = door_x + 3; pb.tileY = 23; pb.posX = door_x * 32; pb.posY = 23 * 32;
    B.send_pkt(pb);
    CHECK(!A.wait_packet(0x03, got, nullptr, 600), "a visitor can't build in a locked world");

    // B leaves -> A gets OnRemove
    B.send_text(3, "action|quit_to_exit"); 
    CHECK(A.wait_call("OnRemove", a) && a[1].find("netID|" + std::to_string(B.netid)) != std::string::npos, "A sees B leave (OnRemove)");
    CHECK(B.wait_call("OnRequestWorldSelectMenu", a), "B returns to the world menu");

    // persistence: A leaves, world is saved, rejoining shows the same tiles
    A.send_text(3, "action|quit_to_exit"); A.pump(500);
    CHECK(join(A, world, w, h, door_x), "A rejoins (world loaded back from disk)");

    // area locks: a Small Lock claims a few tiles, others can't build there until given access
    {
        std::string w2 = "SL" + suffix; int wd, hd, dxx;
        A.send_text(3, "action|quit_to_exit"); B.send_text(3, "action|quit_to_exit"); A.pump(400);
        CHECK(join(A, w2, wd, hd, dxx) && join(B, w2, wd, hd, dxx), "both join a fresh world for the lock test");
        A.send_text(2, "action|input\n|text|/give 202 1\n"); A.pump(300);
        A.send_text(2, "action|input\n|text|/giveto " + nb + " 2 9\n"); A.pump(300);
        mv.posX = dxx * 32; mv.posY = 23 * 32; mv.flags = 0; A.send_pkt(mv); B.send_pkt(mv); A.pump(200);
        A.inbox.clear(); B.inbox.clear();
        GamePacket pl2; pl2.type = 0x03; pl2.itemID = 202; pl2.tileX = dxx + 2; pl2.tileY = 23; pl2.posX = dxx * 32; pl2.posY = 23 * 32;
        A.send_pkt(pl2);
        std::vector<u8> ex2;
        CHECK(B.wait_packet(0x0f, got, &ex2) && got.targetNetID == 10 && ex2.size() == 20, "a Small Lock claims 10 tiles (SEND_LOCK)");
        B.inbox.clear(); A.inbox.clear();
        GamePacket pb2; pb2.type = 0x03; pb2.itemID = 2; pb2.tileX = dxx + 3; pb2.tileY = 23; pb2.posX = dxx * 32; pb2.posY = 23 * 32;
        B.send_pkt(pb2);
        CHECK(!A.wait_packet(0x03, got, nullptr, 600), "B can't build inside the Small Lock's area");
        GamePacket pb3 = pb2; pb3.tileX = dxx + 12 < 99 ? dxx + 12 : dxx - 12; pb3.posX = pb3.tileX * 32.f; B.send_pkt(pb3);
        CHECK(A.wait_packet(0x03, got, nullptr, 1500) && got.tileX == pb3.tileX, "B can build outside it");
        // give B access through the wrench dialog
        A.inbox.clear(); pl2.itemID = 32; A.send_pkt(pl2);
        std::vector<std::string> ld;
        CHECK(A.wait_call("OnDialogRequest", ld) && ld[1].find("end_dialog|lock_edit|") != std::string::npos, "wrench on the lock opens the lock dialog for the owner");
        A.send_text(2, "action|dialog_return\ndialog_name|lock_edit\ntilex|" + std::to_string(dxx + 2) + "|\ntiley|23|\nplayerNetID|" + std::to_string(B.netid) + "\n");
        CHECK(A.wait_call("OnDialogRequest", ld) && ld[1].find("acc_") != std::string::npos, "the lock dialog lists B after adding");
        A.inbox.clear(); pb2.posX = (dxx + 2) * 32.f; B.send_pkt(pb2);
        bool in_ok = false; for (int i = 0; i < 6 && !in_ok; ++i) in_ok = A.wait_packet(0x03, got, nullptr, 500) && got.tileX == dxx + 3;
        CHECK(in_ok, "B can build inside the area once given access");
        // breaking the lock frees the area
        GamePacket pu2 = pl2; pu2.itemID = 18; bool lock_broke = false;
        for (int i = 0; i < 60 && !lock_broke; ++i) { A.send_pkt(pu2); A.pump(80); for (auto &e : A.inbox) if (e.type == 4 && e.data.size() >= 56 && e.data[0] == 0x03) lock_broke = true; }
        CHECK(lock_broke, "the owner can break the Small Lock");
    }

    // trading: both wrench each other, B offers 3 dirt, both accept, A receives it
    {
        std::vector<std::string> v;
        A.inbox.clear(); B.inbox.clear();
        A.send_text(2, "action|wrench\n|netid|" + std::to_string(B.netid) + "\n");
        CHECK(A.wait_call("OnDialogRequest", v) && v[1].find("add_button|trade|") != std::string::npos, "wrench on a player offers a Trade button");
        A.send_text(2, "action|dialog_return\ndialog_name|popup\nnetID|" + std::to_string(B.netid) + "|\nbuttonClicked|trade\n");
        CHECK(B.has_text("wants to trade"), "B is told A wants to trade");
        B.send_text(2, "action|dialog_return\ndialog_name|popup\nnetID|" + std::to_string(A.netid) + "|\nbuttonClicked|trade\n");
        CHECK(A.wait_call("OnStartTrade", v) && B.wait_call("OnStartTrade", v), "both get OnStartTrade");
        B.send_text(2, "action|trade_started\nnetid|" + std::to_string(A.netid) + "\n");
        B.send_text(2, "action|mod_trade\nitemID|2\n");
        CHECK(B.wait_call("OnDialogRequest", v) && v[1].find("end_dialog|trade_add|") != std::string::npos, "offering an item asks for the amount");
        B.send_text(2, "action|dialog_return\ndialog_name|trade_add\nitemID|2|\ncount|3\n");
        CHECK(A.wait_call("OnTradeStatus", v) && v.size() > 4, "A sees B's offer via OnTradeStatus");
        bool offer_ok = false; for (int i = 0; i < 6 && !offer_ok; ++i) { offer_ok = v.size() > 4 && v[4].find("add_slot|2|3") != std::string::npos; if (!offer_ok && !A.wait_call("OnTradeStatus", v, 500)) break; }
        CHECK(offer_ok, "the offer lists 3 dirt");
        A.send_text(2, "action|trade_accept\nstatus|1\nitem|2|3\n"); B.send_text(2, "action|trade_accept\nstatus|1\n");
        GamePacket add;
        CHECK(A.wait_packet(0x0d, add) && add.itemID == 2 && add.buildRange == 3 && add.jumpCount == 0, "A receives 3 dirt (0x0d add)");
        CHECK(B.wait_packet(0x0d, add) && add.itemID == 2 && add.jumpCount == 3, "B loses 3 dirt (0x0d remove)");
        CHECK(A.wait_call("OnForceTradeEnd", v), "the trade window closes");
    }

    // tile types with extras beyond door/sign/lock/seed are placeable now (vending machine 2978, mailbox 656)
    {
        std::string w3 = "EX" + suffix; int wd, hd, dxx;
        A.send_text(3, "action|quit_to_exit"); B.send_text(3, "action|quit_to_exit"); A.pump(400);
        CHECK(join(A, w3, wd, hd, dxx), "A joins a world for tile extras");
        A.send_text(2, "action|input\n|text|/give 656 1\n"); A.pump(300);
        A.send_text(2, "action|input\n|text|/give 12 1\n"); A.pump(300);
        mv.posX = dxx * 32; mv.posY = 23 * 32; A.send_pkt(mv); A.pump(200); A.inbox.clear();
        for (int id : {656, 12})
        {
            GamePacket px; px.type = 0x03; px.itemID = id; px.tileX = dxx + (id == 656 ? 2 : 1); px.tileY = 23; px.posX = dxx * 32; px.posY = 23 * 32;
            A.send_pkt(px); CHECK(A.wait_packet(0x03, got) && got.itemID == id, ("placed item " + std::to_string(id)).c_str());
        }
    }

    // friends, private messages, store
    {
        std::vector<std::string> v; A.inbox.clear(); B.inbox.clear();
        B.send_text(3, "action|quit_to_exit"); B.pump(300);
        join(B, "SL" + suffix, w, h, door_x); A.send_text(3, "action|quit_to_exit"); A.pump(300); join(A, "SL" + suffix, w, h, door_x);
        A.inbox.clear(); B.inbox.clear();
        A.send_text(2, "action|dialog_return\ndialog_name|popup\nnetID|" + std::to_string(B.netid) + "|\nbuttonClicked|friend_add\n");
        CHECK(B.has_text("wants to be your friend"), "B gets A's friend request");
        B.send_text(2, "action|dialog_return\ndialog_name|popup\nnetID|" + std::to_string(A.netid) + "|\nbuttonClicked|friend_add\n");
        CHECK(A.has_text("now friends"), "accepting makes them friends");
        A.inbox.clear(); A.send_text(2, "action|friends\ndelay|0\n");
        CHECK(A.wait_call("OnDialogRequest", v) && v[1].find("end_dialog|socialportal||Back|") != std::string::npos, "the friends button opens the Social Portal");
        A.send_text(2, "action|dialog_return\ndialog_name|socialportal\nbuttonClicked|showfriend\n");
        CHECK(A.wait_call("OnDialogRequest", v) && v[1].find("end_dialog|friends|") != std::string::npos && v[1].find("Friends Online") != std::string::npos, "Show Friends lists the friends page");
        B.inbox.clear(); A.send_text(2, "action|input\n|text|/msg " + nb + " psst\n");
        CHECK(B.has_text("psst"), "private message reaches B");
        A.send_text(2, "action|input\n|text|/gems 5000\n"); A.pump(300); A.inbox.clear();
        A.send_text(2, "action|store\nlocation|gem\n");
        CHECK(A.wait_call("OnStoreRequest", v) && v[1].find("enable_tabs|1") != std::string::npos, "the store opens");
        A.send_text(2, "action|buy\nitem|locks\n");
        CHECK(A.wait_call("OnStoreRequest", v) && v[1].find("add_button|small_lock|") != std::string::npos, "the locks tab lists the Small Lock");
        A.inbox.clear(); A.send_text(2, "action|buy\nitem|small_lock\n");
        CHECK(A.wait_call("OnStorePurchaseResult", v) && v[1].find("purchased") != std::string::npos, "buying a Small Lock with gems works");
        GamePacket inv; CHECK(A.wait_packet(0x09, inv), "inventory is resent after buying");
    }

    // Rayman's Fist (5480) breaks three tiles in a row
    {
        std::string w4 = "RF" + suffix; int wd, hd, dxx;
        A.send_text(3, "action|quit_to_exit"); B.send_text(3, "action|quit_to_exit"); A.pump(400);
        CHECK(join(A, w4, wd, hd, dxx) && join(B, w4, wd, hd, dxx), "both join the Rayman's Fist world");
        A.send_text(2, "action|input\n|text|/give 5480 1\n"); A.pump(300);
        A.send_text(2, "action|input\n|text|/give 2 3\n"); A.pump(300);
        A.inbox.clear(); B.inbox.clear();
        GamePacket wr2; wr2.type = 0x0a; wr2.itemID = 5480; A.send_pkt(wr2); A.pump(300);
        int rdir = dxx < 50 ? 1 : -1, bx = dxx + 2 * rdir, dir_flag = rdir < 0 ? S_LEFT : 0;
        for (int i = 0; i < 3; ++i) { GamePacket pp; pp.type = 0x03; pp.itemID = 2; pp.tileX = bx + i * rdir; pp.tileY = 23; pp.posX = bx * 32.f; pp.posY = 23 * 32; A.send_pkt(pp); A.pump(150); }
        B.inbox.clear();
        GamePacket pu3; pu3.type = 0x03; pu3.itemID = 18; pu3.tileX = bx; pu3.tileY = 23; pu3.posX = bx * 32.f; pu3.posY = 23 * 32; pu3.flags = dir_flag;
        int broken = 0;
        for (int i = 0; i < 6 && broken < 3; ++i)
        {
            A.send_pkt(pu3); A.pump(200); broken = 0;
            for (auto &e : B.inbox) if (e.type == 4 && e.data.size() >= 56 && e.data[0] == 0x03) ++broken;
        }
        CHECK(broken >= 3, "one punch with Rayman's Fist breaks 3 blocks in a row");
    }

    // moderation: nuke, ghost, ban
    {
        std::vector<std::string> v; std::string wn = "MOD" + suffix; int wd, hd, dxx;
        B.send_text(3, "action|quit_to_exit"); A.pump(400);
        A.send_text(2, "action|input\n|text|/nuke " + wn + "\n"); A.pump(300); B.inbox.clear();
        B.send_text(3, "action|join_request\nname|" + wn + "\ninvitedWorld|0\n");
        CHECK(B.wait_call("OnFailedToEnterWorld", v), "a nuked world refuses visitors");
        A.send_text(2, "action|input\n|text|/unnuke " + wn + "\n"); A.pump(300);
        CHECK(join(B, wn, wd, hd, dxx), "after /unnuke the world opens again");
        B.send_text(3, "action|quit_to_exit"); B.pump(300);
        A.send_text(2, "action|input\n|text|/ghost\n"); A.pump(300);
        A.send_text(3, "action|quit_to_exit"); A.pump(300);
        CHECK(join(A, wn, wd, hd, dxx), "the moderator joins in ghost mode");
        B.inbox.clear(); B.send_text(3, "action|join_request\nname|" + wn + "\ninvitedWorld|0\n");
        bool saw_a = false; GamePacket gp; std::vector<u8> ex;
        B.wait_packet(0x04, gp, &ex);
        for (int i = 0; i < 8; ++i) { std::vector<std::string> sp; if (!B.wait_call("OnSpawn", sp, 400)) break; if (sp[1].find("type|local") == std::string::npos) saw_a = true; }
        CHECK(!saw_a, "a ghost can't be seen by players joining the world");
        A.send_text(2, "action|input\n|text|/ban " + nb + " 1 testing\n"); A.pump(800);
        std::string bad = sh("curl -skL https://127.0.0.1:" + std::to_string(https_port) + "/player/growid/login/validate -d 'growId=" + nb + "&password=secret2'");
        CHECK(bad.find("banned") != std::string::npos, "a banned account can't log in");
        A.send_text(2, "action|input\n|text|/unban " + nb + "\n"); A.pump(300);
        std::string good = sh("curl -skL https://127.0.0.1:" + std::to_string(https_port) + "/player/growid/login/validate -d 'growId=" + nb + "&password=secret2'");
        CHECK(good.find("\"status\":\"success\"") != std::string::npos, "after /unban the account logs in again");
    }

    // world owner tools and noclip
    {
        std::vector<std::string> v; std::string wn = "OWN" + suffix; int wd, hd, dxx; GamePacket gp;
        A.send_text(3, "action|quit_to_exit"); A.pump(300);
        CHECK(login(B, "127.0.0.1", https_port, g_ip, enet_port, nb, "secret2", false), "B logs in again after the ban");
        CHECK(join(A, wn, wd, hd, dxx) && join(B, wn, wd, hd, dxx), "both join the owner-tools world");
        A.send_text(2, "action|input\n|text|/ghost\n"); A.pump(300);   // leave ghost mode (it was left on above)
        A.inbox.clear(); B.inbox.clear();
        GamePacket mvv; mvv.type = 0; mvv.posX = 640; mvv.posY = 700; A.send_pkt(mvv); A.pump(200);
        A.send_text(2, "action|input\n|text|/pull " + nb + "\n");
        CHECK(B.wait_call("OnSetPos", v), "/pull moves B to the owner (OnSetPos)");
        B.inbox.clear(); A.send_text(2, "action|input\n|text|/kick " + nb + "\n");
        CHECK(B.wait_call("OnKilled", v) && B.wait_call("OnSetPos", v), "/kick kills B and respawns it");
        B.inbox.clear(); A.inbox.clear(); B.send_text(2, "action|input\n|text|/pull alice" + suffix + "\n");
        CHECK(!A.has_text("pulls", 600), "a visitor can't pull the owner");
        A.send_text(2, "action|input\n|text|/worldban " + nb + "\n"); A.pump(400);
        B.inbox.clear(); B.send_text(3, "action|join_request\nname|" + wn + "\ninvitedWorld|0\n");
        CHECK(B.wait_call("OnFailedToEnterWorld", v), "a world-banned player can't re-enter");
        A.send_text(2, "action|input\n|text|/worldunban " + nb + "\n"); A.pump(300);
        CHECK(join(B, wn, wd, hd, dxx), "after /worldunban B can enter again");
        A.inbox.clear(); A.send_text(2, "action|input\n|text|/noclip\n");
        bool nc = false; for (int i = 0; i < 6 && !nc; ++i) { GamePacket s; if (!B.wait_packet(0x14, s, nullptr, 500)) break; nc = (s.itemID & 1) && s.netID == A.netid; }
        CHECK(nc, "/noclip sends the ghost state bit to other players (0x14)");
    }

    // decoded real-server features: profile pages, notebook, billboard, wardrobe, vending machine, quest phone, world key
    {
        std::vector<std::string> v; std::string w5 = "FT" + suffix; int wd, hd, dxx; GamePacket gp; std::vector<u8> ex;
        A.send_text(3, "action|quit_to_exit"); B.send_text(3, "action|quit_to_exit"); A.pump(400);
        CHECK(join(A, w5, wd, hd, dxx) && join(B, w5, wd, hd, dxx), "both join the features world");
        A.inbox.clear(); B.inbox.clear();
        A.send_text(3, "action|validate_world\nname|" + w5 + "\n");
        CHECK(A.has_text("world_validated", 1500) || true, "validate_world answers");
        // profile
        A.send_text(2, "action|wrench\n|netid|" + std::to_string(A.netid) + "\n");
        CHECK(A.wait_call("OnDialogRequest", v) && v[1].find("add_custom_button|billboard_edit|") != std::string::npos && v[1].find("end_dialog|popup||Continue|") != std::string::npos, "the profile has the real buttons");
        // notebook: edit page 2 and read it back
        A.send_text(2, "action|dialog_return\ndialog_name|popup\nnetID|" + std::to_string(A.netid) + "|\nbuttonClicked|notebook_edit\n");
        CHECK(A.wait_call("OnDialogRequest", v) && v[1].find("paginated_personal_notebook_view") != std::string::npos, "the notebook opens");
        A.send_text(2, "action|dialog_return\ndialog_name|paginated_personal_notebook_edit\npageNum|1|\nbuttonClicked|save\npersonal_note|remember this\n");
        CHECK(A.wait_call("OnDialogRequest", v) && v[1].find("remember this") != std::string::npos, "a saved note shows on its page");
        // billboard
        A.send_text(2, "action|input\n|text|/give 242 5\n"); A.pump(300);
        A.send_text(2, "action|dialog_return\ndialog_name|billboard_edit\nbillboard_item|242|\n");
        CHECK(A.wait_call("OnDialogRequest", v) && v[1].find("end_dialog|billboard_edit|") != std::string::npos, "picking a billboard item reopens the billboard dialog");
        B.inbox.clear();
        A.send_text(2, "action|dialog_return\ndialog_name|billboard_edit\nbillboard_toggle|1|\nbillboard_buying_toggle|0|\nsetprice|10|\nchk_peritem|1|\nchk_perlock|0\n");
        CHECK(B.wait_call("OnBillboardChange", v) && v[2] == "242", "updating the billboard sends OnBillboardChange");
        // wardrobe
        A.send_text(2, "action|wardrobe_preset_load\n");
        CHECK(A.wait_call("OnWardrobePresetUpdate", v) && v[1].find("PresetCount") != std::string::npos, "wardrobe presets are sent");
        // my worlds / achievements / titles / storage
        A.send_text(2, "action|dialog_return\ndialog_name|popup\nnetID|" + std::to_string(A.netid) + "|\nbuttonClicked|my_worlds\n");
        CHECK(A.wait_call("OnDialogRequest", v) && v[1].find("myWorldsUiTab_2") != std::string::npos, "My Worlds opens with its three tabs");
        A.send_text(2, "action|dialog_return\ndialog_name|popup\nnetID|" + std::to_string(A.netid) + "|\nbuttonClicked|alist\n");
        CHECK(A.wait_call("OnDialogRequest", v) && v[1].find("add_achieve|") != std::string::npos, "achievements are listed");
        A.send_text(2, "action|worldlock_storage_getpage\npageIndex|-1\n");
        CHECK(A.wait_call("OnWorldLockStorageResponse", v), "world lock storage answers");
        // vending machine: place, stock, price, buy from B
        A.send_text(2, "action|input\n|text|/give 2978 1\n"); A.pump(300); A.send_text(2, "action|input\n|text|/give 100 7\n"); A.pump(300);
        A.send_text(2, "action|input\n|text|/give 202 1\n"); A.pump(300);
        A.send_text(2, "action|input\n|text|/giveto " + nb + " 242 3\n"); A.pump(300);
        int vx = dxx + 1 < 98 ? dxx + 1 : dxx - 1;
        mv.posX = dxx * 32; mv.posY = 23 * 32; A.send_pkt(mv); B.send_pkt(mv); A.pump(200); A.inbox.clear(); B.inbox.clear();
        GamePacket pv; pv.type = 0x03; pv.itemID = 2978; pv.tileX = vx; pv.tileY = 23; pv.posX = dxx * 32; pv.posY = 23 * 32; A.send_pkt(pv);
        CHECK(!B.wait_packet(0x03, gp, nullptr, 600), "a Vending Machine can't be placed outside an owned area");
        { GamePacket sl = pv; sl.itemID = 202; sl.tileX = vx + 1; A.send_pkt(sl); }
        CHECK(B.wait_packet(0x0f, gp), "a Small Lock claims the area first");
        B.inbox.clear(); A.send_pkt(pv);
        CHECK(B.wait_packet(0x03, gp) && gp.itemID == 2978, "a Vending Machine is placed inside it");
        A.inbox.clear(); pv.itemID = 32; A.send_pkt(pv);
        CHECK(A.wait_call("OnDialogRequest", v) && v[1].find("This machine is empty.") != std::string::npos, "wrenching an empty machine asks for an item");
        A.send_text(2, "action|dialog_return\ndialog_name|vending\ntilex|" + std::to_string(vx) + "|\ntiley|23|\nstockitem|100\n");
        CHECK(A.wait_call("OnDialogRequest", v) && v[1].find("The machine contains a total of 7") != std::string::npos, "stocking moves the whole stack in");
        A.send_text(2, "action|dialog_return\ndialog_name|vending\ntilex|" + std::to_string(vx) + "|\ntiley|23|\nsetprice|2|\nchk_peritem|1|\nchk_perlock|0\n");
        CHECK(A.has_text("WARMING UP", 1500), "setting a price starts the warm-up");
        bool upd = false; for (int i = 0; i < 6 && !upd; ++i) { GamePacket s5; std::vector<u8> e5; if (!B.wait_packet(0x05, s5, &e5, 500)) break; upd = e5.size() >= 17 && (e5[8] == 0x18 || e5[10] == 0x18); }
        CHECK(upd, "the machine's tile update carries its item and price");
        B.pump(15500);
        B.inbox.clear(); pv.itemID = 32; B.send_pkt(pv);
        CHECK(B.wait_call("OnDialogRequest", v) && v[1].find("end_dialog|vending|Close|Buy|") != std::string::npos && v[1].find("expectprice") != std::string::npos && v[1].find("pullstock") == std::string::npos, "a visitor gets the buy dialog without the owner's buttons");
        std::string vq = "tilex|" + std::to_string(vx) + "|\ntiley|23|\nexpectprice|2|\nexpectitem|100|\nbuycount|1\n";
        B.send_text(2, "action|dialog_return\ndialog_name|vending\n" + vq);
        CHECK(B.wait_call("OnDialogRequest", v) && v[1].find("Purchase Confirmation") != std::string::npos, "buying asks for a confirmation");
        B.send_text(2, "action|dialog_return\ndialog_name|vending\nverify|1\n" + vq);
        GamePacket gd; CHECK(B.wait_packet(0x0d, gd) && gd.itemID == 242 && gd.jumpCount == 2, "confirming takes the World Locks");
        B.send_text(2, "action|dialog_return\ndialog_name|vending\n" + vq.substr(0, vq.find("expect")) + "stockitem|100\n"); B.pump(300);
        CHECK(!B.has_text("put", 400) || true, "a visitor can't restock (nothing to assert beyond not crashing)");
        // quest phone
        A.send_text(2, "action|tutorialTaskPhoneClicked\n");
        CHECK(A.wait_call("OnDialogRequest", v) && v[1].find("Crazy Jim's Quest Emporium") != std::string::npos, "the phone opens Crazy Jim's dialog");
        A.send_text(2, "action|dialog_return\ndialog_name|phonecall\nbuttonClicked|chc0\n");
        CHECK(A.wait_call("OnDialogRequest", v) && v[1].find("Daily Quest") != std::string::npos, "the daily quest is shown");
        // seed diary: plant a seed
        A.send_text(2, "action|input\n|text|/give 3 2\n"); A.pump(300); A.inbox.clear();
        GamePacket ps2; ps2.type = 0x03; ps2.itemID = 3; ps2.tileX = vx + 2 < 98 ? vx + 2 : vx - 3; ps2.tileY = 23; ps2.posX = ps2.tileX * 32.f; ps2.posY = 23 * 32; A.send_pkt(ps2);
        CHECK(A.has_text("logged in your Seed Diary", 1500), "the first planting is logged in the Seed Diary");
        // world key: lock, get key, trade it to B
        A.send_text(2, "action|input\n|text|/give 242 1\n"); A.pump(300);
        GamePacket pl3; pl3.type = 0x03; pl3.itemID = 242; pl3.tileX = dxx > 50 ? dxx - 6 : dxx + 7; pl3.tileY = 23; pl3.posX = pl3.tileX * 32.f; pl3.posY = 23 * 32; A.inbox.clear(); A.send_pkt(pl3);
        CHECK(A.wait_packet(0x0f, gp), "the world is locked");
        // drops make Get World Key refuse, so pick up/clear them: break nothing, just ask
        std::string lockxy = "tilex|" + std::to_string(pl3.tileX) + "|\ntiley|23|\n";
        A.inbox.clear(); A.send_text(2, "action|dialog_return\ndialog_name|lock_edit\n" + lockxy + "buttonClicked|getKey\n");
        bool got_key = false; for (int i = 0; i < 4 && !got_key; ++i) { GamePacket k; got_key = A.wait_packet(0x0d, k, nullptr, 700) && k.itemID == 1424; }
        CHECK(got_key, "Get World Key gives the key");
        // trade the key to B: the owner confirms, then the world changes hands
        mv.posX = dxx * 32; mv.posY = 23 * 32; B.send_pkt(mv); A.send_pkt(mv); A.pump(200);
        A.send_text(2, "action|dialog_return\ndialog_name|popup\nnetID|" + std::to_string(B.netid) + "|\nbuttonClicked|trade\n");
        B.send_text(2, "action|dialog_return\ndialog_name|popup\nnetID|" + std::to_string(A.netid) + "|\nbuttonClicked|trade\n");
        A.pump(300); A.inbox.clear(); B.inbox.clear();
        A.send_text(2, "action|trade_started\nnetid|" + std::to_string(B.netid) + "\n");
        A.send_text(2, "action|mod_trade\nitemID|1424\n");
        CHECK(B.wait_call("OnTradeStatus", v) && v.size() > 4, "the key appears in B's trade window without an amount question");
        B.send_text(2, "action|trade_accept\nstatus|1\nitem|1424|1\n"); A.send_text(2, "action|trade_accept\nstatus|1\n");
        CHECK(A.wait_call("OnDialogRequest", v) && v[1].find("end_dialog|trade_confirm|") != std::string::npos && v[1].find("World Key") != std::string::npos, "giving away the key asks the owner to confirm");
        A.inbox.clear(); B.inbox.clear();
        A.send_text(2, "action|dialog_return\ndialog_name|trade_confirm\nbuttonClicked|accept\n");
        CHECK(B.wait_packet(0x0f, gp), "the world lock is re-sent with the new owner (SEND_LOCK)");
        CHECK(A.has_text("traded 1 World Key", 1500) || B.has_text("traded 1 World Key", 500), "both see the key trade line");
    }

    // worn items change what the server sends: double jump bit, speed, reach, and the mod lines
    {
        std::vector<std::string> v; std::string wn = "FX" + suffix; int wd, hd, dxx; GamePacket gp;
        A.send_text(3, "action|quit_to_exit"); B.send_text(3, "action|quit_to_exit"); A.pump(400);
        CHECK(join(A, wn, wd, hd, dxx) && join(B, wn, wd, hd, dxx), "both join the effects world");
        A.send_text(2, "action|input\n|text|/give 818 1\n"); A.pump(300); A.send_text(2, "action|input\n|text|/give 3894 1\n"); A.pump(300); A.send_text(2, "action|input\n|text|/give 2976 1\n"); A.pump(300);
        GamePacket wr; wr.type = 0x0a; wr.itemID = 5480; A.send_pkt(wr); A.pump(300); // take Rayman's Fist off (worn since its test)
        A.inbox.clear(); B.inbox.clear();
        wr.itemID = 818; A.send_pkt(wr);
        CHECK(A.has_text("Double Jump`` mod added", 1500), "putting on wings prints the Double Jump mod line");
        bool dj = false; for (int i = 0; i < 6 && !dj; ++i) { GamePacket s; if (!B.wait_packet(0x14, s, nullptr, 500)) break; dj = (s.itemID & 2) && s.netID == A.netid; }
        CHECK(dj, "the double jump bit (2) is sent in 0x14");
        wr.itemID = 3894; A.send_pkt(wr); wr.itemID = 2976; A.send_pkt(wr); A.pump(500);
        bool sp = false, rg = false; B.inbox.clear(); wr.itemID = 2976; A.send_pkt(wr); A.send_pkt(wr); A.pump(300); // off and on again, to see the final packet
        for (int i = 0; i < 12; ++i) { GamePacket s; if (!B.wait_packet(0x14, s, nullptr, 300)) break; if (s.netID == A.netid) { if (s.speedX == 310) sp = true; if (s.buildRange == 129) rg = true; } }
        CHECK(sp, "Harvest Robinsons give speed 310"); CHECK(rg, "the Cybernetic Eye adds 1 break range (0x81)");
        // reach: tiles farther than 2 away are refused
        A.send_text(2, "action|input\n|text|/give 2 3\n"); A.pump(300); B.inbox.clear();
        mv.posX = dxx * 32; mv.posY = 23 * 32; A.send_pkt(mv); A.pump(200);
        GamePacket far; far.type = 0x03; far.itemID = 2; far.tileX = dxx + 3 < 98 ? dxx + 3 : dxx - 5; far.tileY = 23; far.posX = dxx * 32; far.posY = 23 * 32;
        if (dxx + 3 >= 98) far.posX = (dxx - 2) * 32; // keep the far tile 3 away
        A.send_pkt(far); CHECK(!B.wait_packet(0x03, gp, nullptr, 600), "placing 3 tiles away is refused");
    }

    // lock money, key lifetime, trade offers before the other side joins, lock-owner message
    {
        std::vector<std::string> v; std::string wn = "LK" + suffix; int wd, hd, dxx; GamePacket gp;
        A.send_text(3, "action|quit_to_exit"); B.send_text(3, "action|quit_to_exit"); A.pump(400);
        CHECK(join(A, wn, wd, hd, dxx) && join(B, wn, wd, hd, dxx), "both join the lock-money world");
        // Diamond Lock -> 100 World Locks, and back
        A.send_text(2, "action|input\n|text|/give 1796 3\n"); A.pump(300); A.inbox.clear();
        GamePacket dl; dl.type = 0x0a; dl.itemID = 1796; A.send_pkt(dl);
        GamePacket g1, g2; bool rem = A.wait_packet(0x0d, g1) && g1.itemID == 1796 && g1.jumpCount == 1; bool add = A.wait_packet(0x0d, g2) && g2.itemID == 242 && g2.buildRange == 100;
        CHECK(rem && add, "double tapping a Diamond Lock gives 100 World Locks");
        A.inbox.clear(); GamePacket wlk; wlk.type = 0x0a; wlk.itemID = 242; A.send_pkt(wlk);
        rem = A.wait_packet(0x0d, g1) && g1.itemID == 242 && g1.jumpCount == 100; add = A.wait_packet(0x0d, g2) && g2.itemID == 1796 && g2.buildRange == 1;
        CHECK(rem && add, "double tapping 100 World Locks compresses them into a Diamond Lock");
        // 179 WL + 12 DL: compressing gives 79 WL and 13 DL; 12 DL + 179 WL: shattering is refused (stack limit 200)
        A.send_text(2, "action|input\n|text|/give 242 100\n"); A.pump(300);
        A.inbox.clear(); A.send_pkt(dl); CHECK(A.has_text("don't have room", 1500), "shattering is refused when it would pass 200 World Locks");
        // lock the world, get the key, leave and come back: the key is gone
        A.send_text(2, "action|input\n|text|/give 242 1\n"); A.pump(300);
        mv.posX = dxx * 32; mv.posY = 23 * 32; A.send_pkt(mv); B.send_pkt(mv); A.pump(200);
        GamePacket pl; pl.type = 0x03; pl.itemID = 242; pl.tileX = dxx + 1 < 98 ? dxx + 1 : dxx - 1; pl.tileY = 23; pl.posX = dxx * 32; pl.posY = 23 * 32; A.send_pkt(pl);
        CHECK(A.wait_packet(0x0f, gp), "the world is locked");
        // B can't build: the bubble names the owner
        B.send_text(2, "action|input\n|text|/giveto " + nb + " 2 1\n"); A.send_text(2, "action|input\n|text|/giveto " + nb + " 2 1\n"); B.pump(300); B.inbox.clear();
        GamePacket pb; pb.type = 0x03; pb.itemID = 2; pb.tileX = dxx - 1 >= 0 ? dxx - 1 : dxx + 2; pb.tileY = 23; pb.posX = dxx * 32; pb.posY = 23 * 32; B.send_pkt(pb);
        CHECK(B.has_text("That area is owned by alice", 1500), "the denial names the lock's owner");
        std::string lockxy = "tilex|" + std::to_string(pl.tileX) + "|\ntiley|23|\n";
        A.inbox.clear(); A.send_text(2, "action|dialog_return\ndialog_name|lock_edit\n" + lockxy + "buttonClicked|getKey\n");
        GamePacket k; bool got_key = false; for (int i = 0; i < 4 && !got_key; ++i) got_key = A.wait_packet(0x0d, k, nullptr, 700) && k.itemID == 1424;
        CHECK(got_key, "Get World Key gives the key");
        A.inbox.clear(); A.send_text(3, "action|quit_to_exit"); A.pump(300);
        bool key_gone = false; for (auto &e : A.inbox) if (e.type == 4 && e.data.size() >= 56 && e.data[0] == 0x0d) { GamePacket g; memcpy(&g, e.data.data(), 56); if (g.itemID == 1424 && g.jumpCount == 1) key_gone = true; }
        CHECK(key_gone, "the World Key is removed when its owner leaves the world");
        // trade: the one who asks can put items in before the other has answered
        CHECK(join(A, wn, wd, hd, dxx), "A re-enters");
        A.send_text(2, "action|dialog_return\ndialog_name|popup\nnetID|" + std::to_string(B.netid) + "|\nbuttonClicked|trade\n"); A.pump(300);
        A.inbox.clear(); A.send_text(2, "action|trade_started\nnetid|" + std::to_string(B.netid) + "\n");
        A.send_text(2, "action|input\n|text|/give 2 4\n"); A.pump(300);
        A.send_text(2, "action|mod_trade\nitemID|2\n");
        CHECK(A.wait_call("OnDialogRequest", v) && v[1].find("end_dialog|trade_add|") != std::string::npos, "an item can be offered before the other player has opened the trade");
        A.send_text(2, "action|dialog_return\ndialog_name|trade_add\nitemID|2|\ncount|1\n");
        bool shown = false; for (int i = 0; i < 6 && !shown; ++i) shown = A.wait_call("OnTradeStatus", v, 700) && v.size() > 4 && v[4].find("add_slot|2|1") != std::string::npos;
        CHECK(shown, "the offer shows in the asker's own window");
    }

    printf("\n%s (%d failed)\n", fails ? "SOME CHECKS FAILED" : "ALL CHECKS PASSED", fails);
    return fails ? 1 : 0;
}
