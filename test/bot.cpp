// a player that logs in through the real flow, joins a world and stays there; it accepts friend and trade requests so the
// features can be tried with a second account.   ./bot.out <growid> <password> [world=START] [https_port=443] [enet_port=17091] [ip=127.0.0.1]
#include "proto.hpp"
#include <cstdio>
#include <cstdlib>
#include <map>
#include <thread>
#include <chrono>
static std::string sh(const std::string &cmd) { std::string out; FILE *f = popen(cmd.c_str(), "r"); char b[4096]; size_t n; while ((n = fread(b, 1, sizeof b, f)) > 0) out.append(b, n); pclose(f); return out; }
static ENetHost *host; static ENetPeer *peer;
static void connect_to(const char *ip, int port)
{
    if (host) enet_host_destroy(host);
    host = enet_host_create(ENET_ADDRESS_TYPE_IPV4, nullptr, 1, 2, 0, 0); host->usingNewPacket = 1; host->checksum = enet_crc32; enet_host_compress_with_range_coder(host);
    ENetAddress a{.type = ENET_ADDRESS_TYPE_IPV4, .port = (enet_uint16)port}; enet_address_set_host_ip(&a, ip); peer = enet_host_connect(host, &a, 2, 0);
}
static void text(int type, const std::string &t) { Buf b; b.put<u32>(type).raw(t.data(), t.size()).put<u8>(0); send_raw(peer, b.d.data(), b.d.size()); }
int main(int argc, char **argv)
{
    if (argc < 3) { puts("usage: bot.out <growid> <password> [world] [https_port] [enet_port] [ip]"); return 1; }
    std::string name = argv[1], pw = argv[2], world = argc > 3 ? argv[3] : "START"; int hp = argc > 4 ? atoi(argv[4]) : 443, ep = argc > 5 ? atoi(argv[5]) : 17091; const char *ip = argc > 6 ? argv[6] : "127.0.0.1";
    setvbuf(stdout, nullptr, _IOLBF, 0); enet_initialize();
    std::string json = sh("curl -skL https://" + std::string(ip) + ":" + std::to_string(hp) + "/player/growid/login/validate --data-urlencode 'growId=" + name + "' --data-urlencode 'password=" + pw + "'");
    size_t k = json.find("\"token\":\""); if (k == std::string::npos) { printf("login page: %s\n", json.c_str()); return 1; }
    std::string token = json.substr(k + 9, json.find('"', k + 9) - k - 9), uuid; int stage = 0, netid = -1, other = -1; std::map<std::string, int> seen;
    connect_to(ip, ep);
    for (;;)
    {
        ENetEvent ev;
        while (enet_host_service(host, &ev, 100) > 0)
        {
            if (ev.type == ENET_EVENT_TYPE_DISCONNECT) { if (stage == 1) continue; puts("disconnected"); return 0; }
            if (ev.type != ENET_EVENT_TYPE_RECEIVE) continue;
            const u8 *d = ev.packet->data; size_t n = ev.packet->dataLength; u32 type; memcpy(&type, d, 4);
            if (type == 1) { if (stage == 0) text(2, "protocol|227\nltoken|" + token + "\nplatformID|0\n"); }
            else if (type == 4 && n >= 60 && d[4] == 0x01)
            {
                auto v = read_varlist(d + 60, n - 60); if (v.empty()) continue;
                const std::string &fn = v[0];
                if (fn == "OnSendToServer")
                {
                    uuid = v[4].substr(v[4].rfind('|') + 1); stage = 1; connect_to(ip, atoi(v[1].c_str())); ev.packet = nullptr;
                    // the new connection's HELLO triggers the full login below
                    stage = 2; std::string full = "tankIDName|" + name + "\nprotocol|227\ngame_version|5.6\nuser|" + v[3] + "\ntoken|" + v[2] + "\nUUIDToken|" + uuid + "\ndoorID|0\nlmode|1\n";
                    for (int t = 0; t < 50; ++t) { ENetEvent e2; if (enet_host_service(host, &e2, 100) > 0 && e2.type == ENET_EVENT_TYPE_RECEIVE) { enet_packet_destroy(e2.packet); break; } }
                    text(2, full); continue;
                }
                if (fn.rfind("OnSuperMainStartAcceptLogon", 0) == 0) { text(2, "action|enter_game\n"); }
                else if (fn == "OnRequestWorldSelectMenu" && netid < 0) { text(3, "action|join_request\nname|" + world + "\ninvitedWorld|0\n"); }
                else if (fn == "OnSpawn" && v[1].find("type|local") == std::string::npos)
                {
                    size_t q = v[1].find("netID|"), nq = v[1].find("name|"); if (q == std::string::npos || nq == std::string::npos) continue;
                    std::string nm = v[1].substr(nq + 5, v[1].find('\n', nq) - nq - 5), clean; for (size_t i = 0; i < nm.size(); ++i) { if (nm[i] == '`') { ++i; continue; } clean += nm[i]; }
                    if (!clean.empty() && clean[0] == '@') clean.erase(0, 1);
                    seen[clean] = atoi(v[1].c_str() + q + 6);
                }
                else if (fn == "OnSpawn" && v[1].find("type|local") != std::string::npos) { size_t q = v[1].find("netID|"); netid = atoi(v[1].c_str() + q + 6); printf("in %s as netID %d\n", world.c_str(), netid);
                    if (const char *walk = getenv("BOT_WALK")) // BOT_WALK=<pixels>: step that far to the right once in
                    {
                        float px = 0, py = 0; size_t xy = v[1].find("posXY|"); if (xy != std::string::npos) sscanf(v[1].c_str() + xy + 6, "%f|%f", &px, &py);
                        GamePacket h; h.type = 0x00; h.netID = netid; h.posX = px + atof(walk); h.posY = py; h.flags = 0x20; send_packet(peer, h); printf("walked to %.0f,%.0f\n", h.posX, h.posY);
                        if (const char *dn = getenv("BOT_DOWN")) { py += atof(dn); h.posY = py; send_packet(peer, h); printf("moved down to %.0f\n", py); } // BOT_DOWN=<pixels>
                        if (const char *brk = getenv("BOT_BREAK")) // BOT_BREAK=<n>: punch the n tiles below until foreground and background are gone
                            for (int dy = 1; dy <= atoi(brk); ++dy)
                                for (int hit = 0; hit < 16; ++hit)
                                {
                                    GamePacket pu; pu.type = 0x03; pu.netID = netid; pu.itemID = 18; pu.tileX = (int)h.posX / 32; pu.tileY = (int)py / 32 + dy; pu.posX = h.posX; pu.posY = h.posY;
                                    send_packet(peer, pu);
                                    auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(600); // service returns early whenever the server sends something, so wait out the full interval
                                    while (std::chrono::steady_clock::now() < until) { ENetEvent e3; if (enet_host_service(host, &e3, 50) > 0 && e3.type == ENET_EVENT_TYPE_RECEIVE) enet_packet_destroy(e3.packet); }
                                }
                    } }
                else if (fn == "OnConsoleMessage")
                {
                    printf("console: %s\n", v[1].c_str());
                    for (auto &[nm, id] : seen)
                    {
                        std::string c = v[1]; std::string clean; for (size_t i = 0; i < c.size(); ++i) { if (c[i] == '`') { ++i; continue; } clean += c[i]; }
                        if (clean.rfind(nm + " wants to be your friend", 0) == 0) text(2, "action|dialog_return\ndialog_name|popup\nnetID|" + std::to_string(id) + "|\nbuttonClicked|friend_add\n");
                        if (clean.rfind(nm + " wants to trade", 0) == 0) text(2, "action|dialog_return\ndialog_name|popup\nnetID|" + std::to_string(id) + "|\nbuttonClicked|trade\n");
                    }
                    // "<name> wants to ..." -> answer by wrenching them; the dialog's buttons are pressed below
                }
                else if (fn == "OnStartTrade") { other = atoi(v[2].c_str()); text(2, "action|trade_started\nnetid|" + v[2] + "\n"); }
                else if (fn == "OnTradeStatus") { if (atoi(v[1].c_str()) == other && v.size() > 4) { std::string items; size_t pos = 0; while ((pos = v[4].find("add_slot|", pos)) != std::string::npos) { int id, c; if (sscanf(v[4].c_str() + pos, "add_slot|%d|%d", &id, &c) == 2) items += "item|" + std::to_string(id) + "|" + std::to_string(c) + "\n"; pos += 9; } text(2, "action|trade_accept\nstatus|1\n" + items); } }
                else if (fn == "OnSpawn") {}
            }
            enet_packet_destroy(ev.packet);
        }
    }
}
