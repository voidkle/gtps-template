// gtcap: decodes a Wireshark/tcpdump capture of Growtopia's ENet traffic into readable packets.
//   gtcap <capture.pcap|.pcapng> [--port N] [--hex] [--raw-secrets] [--stats] [--only client|server]
// Reads pcap and pcapng (Ethernet, Linux cooked, raw IP, loopback). UDP only: the HTTPS login is encrypted and not shown.
// ENet as Growtopia uses it: client datagrams start with a 6-byte integrity field; every datagram has peerID+flags (u16 BE),
// sentTime (u16 BE), a CRC32 (u32), then a range-coded body of commands. See docs/guide/transport.md.
#include "proto.hpp"
#include <algorithm>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <map>
#include <set>
#include <unistd.h>

struct Dgram { double t; u32 sip, dip; u16 sport, dport; std::vector<u8> d; };

static u16 be16(const u8 *p) { return p[0] << 8 | p[1]; }
static u32 be32(const u8 *p) { return (u32)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }

// ---------------------------------------------------------------- capture files
static bool link_to_udp(int lt, const u8 *p, size_t n, Dgram &g)
{
    size_t o = 0; int eth = 0;
    switch (lt)
    {
        case 1: if (n < 14) return false; eth = be16(p + 12); o = 14; while (eth == 0x8100 && n >= o + 4) { eth = be16(p + o + 2); o += 4; } break;
        case 113: if (n < 16) return false; eth = be16(p + 14); o = 16; break;
        case 276: if (n < 20) return false; eth = be16(p); o = 20; break;
        case 0: case 108: if (n < 4) return false; eth = 0x0800; o = 4; break;
        case 101: case 228: eth = 0x0800; break;
        default: return false;
    }
    if (eth != 0x0800 || n < o + 20) return false; // IPv4 only; the game doesn't use IPv6
    const u8 *ip = p + o; size_t ihl = (ip[0] & 15) * 4;
    if ((ip[0] >> 4) != 4 || ip[9] != 17 || n < o + ihl + 8) return false;
    if (be16(ip + 6) & 0x1fff) return false; // fragments of larger datagrams: not expected, MTU is small
    const u8 *u = ip + ihl; size_t len = std::min<size_t>(be16(u + 4), n - o - ihl);
    if (len < 8) return false;
    g.sip = be32(ip + 12); g.dip = be32(ip + 16); g.sport = be16(u); g.dport = be16(u + 2); g.d.assign(u + 8, u + len);
    return true;
}

static bool read_capture(const std::string &path, std::vector<Dgram> &out)
{
    std::ifstream f(path, std::ios::binary); if (!f) return false;
    std::vector<u8> d((std::istreambuf_iterator<char>(f)), {});
    if (d.size() < 24) return false;
    u32 magic; memcpy(&magic, d.data(), 4);
    double t0 = -1;
    auto push = [&](double t, int lt, const u8 *p, size_t n) { Dgram g; g.t = t; if (link_to_udp(lt, p, n, g)) { out.push_back(std::move(g)); } };
    if (magic == 0xa1b2c3d4 || magic == 0xa1b23c4d || magic == 0xd4c3b2a1 || magic == 0x4d3cb2a1)
    {
        bool swap = magic == 0xd4c3b2a1 || magic == 0x4d3cb2a1, nano = magic == 0xa1b23c4d || magic == 0x4d3cb2a1;
        auto rd = [&](size_t o) { u32 v; memcpy(&v, d.data() + o, 4); return swap ? __builtin_bswap32(v) : v; };
        int lt = rd(20); size_t o = 24;
        while (o + 16 <= d.size())
        {
            double t = rd(o) + rd(o + 4) / (nano ? 1e9 : 1e6); u32 cl = rd(o + 8); o += 16;
            if (o + cl > d.size()) break;
            push(t, lt, d.data() + o, cl); o += cl; (void)t0;
        }
        return true;
    }
    if (magic == 0x0A0D0D0A) // pcapng
    {
        std::vector<int> lts; std::vector<double> res; size_t o = 0; bool le = true;
        auto r32 = [&](size_t p) { u32 v; memcpy(&v, d.data() + p, 4); return le ? v : __builtin_bswap32(v); };
        auto r16 = [&](size_t p) { u16 v; memcpy(&v, d.data() + p, 2); return le ? v : __builtin_bswap16(v); };
        while (o + 12 <= d.size())
        {
            u32 type = r32(o), len = r32(o + 4);
            if (type == 0x0A0D0D0A) { u32 bom; memcpy(&bom, d.data() + o + 8, 4); le = bom == 0x1A2B3C4D; len = r32(o + 4); lts.clear(); res.clear(); }
            if (len < 12 || o + len > d.size()) break;
            if (type == 1) // interface description: link type + if_tsresol option
            {
                lts.push_back(r16(o + 8)); double r = 1e-6;
                for (size_t p = o + 16; p + 4 <= o + len - 4; ) { u16 code = r16(p), l = r16(p + 2); if (code == 0) break; if (code == 9 && l >= 1) { u8 v = d[p + 4]; r = (v & 0x80) ? 1.0 / (1u << (v & 0x7f)) : 1.0; for (int i = 0; i < (v & 0x7f) && !(v & 0x80); ++i) r /= 10; } p += 4 + ((l + 3) & ~3); }
                res.push_back(r);
            }
            else if (type == 6 && o + 28 <= d.size()) // enhanced packet block
            {
                u32 itf = r32(o + 8), cl = r32(o + 20); u64 ts = (u64)r32(o + 12) << 32 | r32(o + 16);
                if (itf < lts.size() && o + 28 + cl <= o + len) push(ts * res[itf], lts[itf], d.data() + o + 28, cl);
            }
            o += (len + 3) & ~3u;
        }
        return true;
    }
    return false;
}

// ---------------------------------------------------------------- ENet
extern "C" void *enet_range_coder_create(void);
extern "C" size_t enet_range_coder_decompress(void *, const unsigned char *, size_t, unsigned char *, size_t);

struct Cmd { int cmd, channel; u16 seq; std::vector<u8> data; u32 fcount = 0, fnum = 0, ftotal = 0, foff = 0; u16 fstart = 0; };

static const int CMD_SIZE[13] = {0, 8, 48, 44, 8, 4, 6, 8, 24, 8, 12, 16, 24};

// parses the commands of one decompressed body; false if it doesn't fit the format exactly
static bool parse_commands(const u8 *p, size_t n, std::vector<Cmd> &out)
{
    size_t o = 0;
    while (o < n)
    {
        if (o + 4 > n) return false;
        int c = p[o] & 15; if (c == 0 || c >= 13 || o + CMD_SIZE[c] > n) return false;
        Cmd m; m.cmd = c; m.channel = p[o + 1]; m.seq = be16(p + o + 2);
        const u8 *h = p + o; size_t len = 0, hdr = CMD_SIZE[c];
        if (c == 6) len = be16(h + 4);
        else if (c == 7 || c == 9) len = be16(h + 6);
        else if (c == 8 || c == 12) { len = be16(h + 6); m.fstart = be16(h + 4); m.fcount = be32(h + 8); m.fnum = be32(h + 12); m.ftotal = be32(h + 16); m.foff = be32(h + 20); }
        if (o + hdr + len > n) return false;
        m.data.assign(p + o + hdr, p + o + hdr + len);
        if (c == 2 || c == 3) m.data.assign(h, h + hdr); // keep the raw handshake for printing
        out.push_back(std::move(m)); o += hdr + len;
    }
    return o == n;
}

// tries both header layouts (client datagrams carry a 6-byte integrity prefix); returns commands and whether it was a client datagram
static bool decode_datagram(const std::vector<u8> &d, std::vector<Cmd> &cmds, bool &from_client)
{
    static void *coder = enet_range_coder_create();
    for (int pre : {6, 0})
    {
        size_t o = pre; if (d.size() < o + 8) continue;
        u16 flags = be16(&d[o]); o += 2;
        if (flags & 0x8000) o += 2; // sentTime
        o += 4;                     // CRC32
        if (o > d.size()) continue;
        std::vector<u8> body(d.begin() + o, d.end());
        if (flags & 0x4000)
        {
            std::vector<u8> out(1 << 17);
            size_t n = enet_range_coder_decompress(coder, body.data(), body.size(), out.data(), out.size());
            if (!n) continue;
            out.resize(n);
            body = std::move(out);
        }
        std::vector<Cmd> tmp;
        if (parse_commands(body.data(), body.size(), tmp)) { cmds = std::move(tmp); from_client = pre == 6; return true; }
    }
    return false;
}

// ---------------------------------------------------------------- application layer printing
static const char *PKT_NAME[] = {"STATE", "CALL_FUNCTION", "UPDATE_STATUS", "TILE_CHANGE_REQUEST", "SEND_MAP_DATA", "SEND_TILE_UPDATE_DATA", "SEND_TILE_UPDATE_DATA_MULTIPLE",
    "TILE_ACTIVATE_REQUEST", "TILE_APPLY_DAMAGE", "SEND_INVENTORY_STATE", "ITEM_ACTIVATE_REQUEST", "ITEM_ACTIVATE_OBJECT_REQUEST", "SEND_TILE_TREE_STATE", "MODIFY_ITEM_INVENTORY",
    "ITEM_CHANGE_OBJECT", "SEND_LOCK", "SEND_ITEM_DATABASE_DATA", "SEND_PARTICLE_EFFECT", "SET_ICON_STATE", "ITEM_EFFECT", "SET_CHARACTER_STATE", "PING_REPLY", "PING_REQUEST",
    "GOT_PUNCHED", "APP_CHECK_RESPONSE", "APP_INTEGRITY_FAIL", "DISCONNECT", "BATTLE_JOIN", "BATTLE_EVENT", "USE_DOOR", "SEND_PARENTAL", "GONE_FISHIN", "STEAM", "PET_BATTLE", "NPC",
    "SPECIAL", "SEND_PARTICLE_EFFECT_V2", "ACTIVE_ARROW_TO_ITEM", "SELECT_TILE_INDEX", "SEND_PLAYER_TRIBUTE_DATA", "FTUE_SET_ITEM_TO_QUICK_INVENTORY", "PVE_NPC", "PVPCARD_BATTLE",
    "PVE_APPLY_PLAYER_DAMAGE", "PVE_NPC_POSITION_UPDATE", "SET_EXTRA_MODS", "ON_STEP_ON_TILE_MOD"};

static bool g_hex = false, g_secrets = false;

static std::string hexs(const u8 *p, size_t n, size_t max = 96)
{
    static const char *d = "0123456789abcdef"; std::string s;
    for (size_t i = 0; i < n && i < max; ++i) { s += d[p[i] >> 4]; s += d[p[i] & 15]; if (i + 1 < n) s += ' '; }
    if (n > max) s += " ...(" + std::to_string(n) + " bytes)";
    return s;
}

static std::string quote(const std::string &s)
{
    std::string o = "\""; for (char c : s) { if (c == '\n') o += "\\n"; else if (c == '"') o += "\\\""; else if ((u8)c < 32) o += "\\x" + hexs((const u8 *)&c, 1); else o += c; } return o + "\"";
}

static std::string render_text(std::string t) // drops the secrets unless asked: passwords, tokens and device ids
{
    if (g_secrets) return t;
    static const std::set<std::string> secret = {"tankIDPass", "ltoken", "token", "UUIDToken", "rid", "mac", "wk", "aid", "gid", "vid", "hash", "hash2", "fhash", "klv", "fz", "FCMToken", "meta", "tr"};
    std::string out; size_t pos = 0;
    while (pos < t.size())
    {
        size_t e = t.find('\n', pos); if (e == std::string::npos) e = t.size();
        std::string line = t.substr(pos, e - pos); size_t bar = line.find('|');
        std::string key = bar == std::string::npos ? line : line.substr(0, bar); if (!key.empty() && key[0] == '|') key.erase(0, 1);
        out += (secret.count(key) ? key + "|<redacted>" : line) + "\n"; pos = e + 1;
    }
    return out;
}

static std::string render_varlist(const u8 *p, size_t n)
{
    Rd r(p, n); int count = r.get<u8>(); std::string s;
    for (int k = 0; k < count && r.ok; ++k)
    {
        int idx = r.get<u8>(), t = r.get<u8>(); char b[96]; std::string v;
        switch (t)
        {
            case 1: snprintf(b, sizeof b, "%gf", r.get<float>()); v = b; break;
            case 2: v = quote(r.str32()); break;
            case 3: { float x = r.get<float>(), y = r.get<float>(); snprintf(b, sizeof b, "vec2(%g, %g)", x, y); v = b; break; }
            case 4: { float x = r.get<float>(), y = r.get<float>(), z = r.get<float>(); snprintf(b, sizeof b, "vec3(%g, %g, %g)", x, y, z); v = b; break; }
            case 5: v = std::to_string(r.get<u32>()) + "u"; break;
            case 9: v = std::to_string(r.get<i32>()); break;
            case 10: v = std::to_string(r.get<int64_t>()) + "L"; break;
            default: v = "?type" + std::to_string(t); r.ok = false;
        }
        if (idx == 0 && t == 2) s = v.substr(1, v.size() - 2) + "("; else s += (s.empty() || s.back() == '(' ? "" : ", ") + v;
    }
    return s + (r.ok ? ")" : " ...)");
}

static std::string describe_packet(const u8 *d, size_t n)
{
    if (n < 56) return "short game packet (" + std::to_string(n) + " bytes) " + hexs(d, n);
    GamePacket h; memcpy(&h, d, 56); const u8 *ex = d + 56; size_t exn = n - 56;
    char b[512]; std::string name = h.type < sizeof PKT_NAME / sizeof *PKT_NAME ? PKT_NAME[h.type] : "UNKNOWN";
    snprintf(b, sizeof b, "0x%02x %s", h.type, name.c_str()); std::string s = b;
    auto add = [&](const char *k, long v) { if (v) { snprintf(b, sizeof b, " %s=%ld", k, v); s += b; } };
    auto addf = [&](const char *k, float v) { if (v != 0) { snprintf(b, sizeof b, " %s=%g", k, v); s += b; } };
    if (h.type == 0x01)
    {
        snprintf(b, sizeof b, " netID=%d delay=%d  ", h.netID, h.itemID); s = "CALL" + std::string(b) + render_varlist(ex, exn); return s;
    }
    add("objectType", h.objectType); add("jumpCount", h.jumpCount); add("buildRange", h.buildRange);
    add("netID", h.netID); add("targetNetID", h.targetNetID);
    if (h.flags) { snprintf(b, sizeof b, " flags=0x%x", h.flags); s += b; }
    addf("float", h.floatVar); add("itemID", h.itemID); addf("posX", h.posX); addf("posY", h.posY); addf("speedX", h.speedX); addf("speedY", h.speedY); addf("particle", h.particleID);
    if (h.tileX || h.tileY) { snprintf(b, sizeof b, " tile=(%d,%d)", h.tileX, h.tileY); s += b; }
    if (exn)
    {
        s += " extra=" + std::to_string(exn) + "B";
        if (h.type == 0x04 && exn > 12) { Rd r(ex, exn); u16 ver = r.get<u16>(); u32 fl = r.get<u32>(); std::string nm = r.str16(); i32 w = r.get<i32>(), hh = r.get<i32>(); snprintf(b, sizeof b, " [world v%u flags %u \"%s\" %dx%d]", ver, fl, nm.c_str(), w, hh); s += b; }
        else if (h.type == 0x09 && exn >= 8) { Rd r(ex, exn); u8 ver = r.get<u8>(); u32 slots = r.get<u32>(); u32 cnt = ver ? r.get<u16>() : r.get<u8>(); snprintf(b, sizeof b, " [inventory v%u %u slots, %u items]", ver, slots, cnt); s += b; }
        else if (h.type == 0x0f) s += " [" + std::to_string(h.targetNetID) + " tile indices]";
        else if (g_hex) s += "  " + hexs(ex, exn, 64);
    }
    return s;
}

static void describe_message(double t, bool from_client, const std::vector<u8> &m, int *stats_msg, std::map<std::string, int> &stats)
{
    char head[64]; snprintf(head, sizeof head, "%9.3f %s ", t, from_client ? "C->S" : "S->C"); std::string line = head;
    if (m.size() < 4) { printf("%s(empty message)\n", head); return; }
    u32 type; memcpy(&type, m.data(), 4); const u8 *p = m.data() + 4; size_t n = m.size() - 4;
    if (type == 1) { printf("%sHELLO\n", head); return; }
    if (type == 2 || type == 3)
    {
        std::string t2((const char *)p, n); while (!t2.empty() && t2.back() == '\0') t2.pop_back();
        std::string a = field(t2, "action"); stats[(type == 2 ? "text2 " : "text3 ") + (a.empty() ? "(login)" : a)]++;
        std::string r = render_text(t2); while (!r.empty() && r.back() == '\n') r.pop_back();
        std::string flat; for (char c : r) flat += c == '\n' ? '|' : c;
        printf("%s%s %s\n", head, type == 2 ? "TEXT" : "ACTION", quote(flat).c_str()); return;
    }
    if (type == 4)
    {
        std::string d = describe_packet(p, n); printf("%s%s\n", head, d.c_str());
        if (n >= 56) { u8 pt = p[0]; if (pt == 1 && n > 56) { auto v = read_varlist(p + 56, n - 56); if (!v.empty()) stats["call " + v[0]]++; } else stats[std::string("packet ") + (pt < sizeof PKT_NAME / sizeof *PKT_NAME ? PKT_NAME[pt] : "UNKNOWN")]++; }
        return;
    }
    (void)stats_msg;
    printf("%smessage type %u, %zu bytes  %s\n", head, type, n, hexs(p, n).c_str());
}

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: gtcap <capture.pcap|pcapng> [--port N] [--hex] [--raw-secrets] [--stats] [--only client|server] [--handshake] [--ip PREFIX] [--show-undecodable]\n"); return 1; }
    std::string path = argv[1]; int sport = 0; bool stats_only = false, handshake = false, show_bad = false; std::string only, ipfilter;
    for (int i = 2; i < argc; ++i)
    {
        std::string a = argv[i];
        if (a == "--port" && i + 1 < argc) sport = atoi(argv[++i]); else if (a == "--hex") g_hex = true; else if (a == "--raw-secrets") g_secrets = true;
        else if (a == "--stats") stats_only = true; else if (a == "--only" && i + 1 < argc) only = argv[++i]; else if (a == "--handshake") handshake = true;
        else if (a == "--ip" && i + 1 < argc) ipfilter = argv[++i]; else if (a == "--show-undecodable") show_bad = true;
    }
    FILE *report = stdout; // with --stats the per-message lines go to /dev/null and only the counts are shown
    if (stats_only) { report = fdopen(dup(1), "w"); if (!freopen("/dev/null", "w", stdout)) return 1; }
    std::vector<Dgram> dg;
    if (!read_capture(path, dg)) { fprintf(stderr, "can't read %s as pcap or pcapng\n", path.c_str()); return 1; }
    // a flow = a pair of endpoints; with --port only datagrams to or from that UDP port are used
    if (sport) dg.erase(std::remove_if(dg.begin(), dg.end(), [&](const Dgram &g) { return g.sport != sport && g.dport != sport; }), dg.end());
    auto ipstr = [](u32 a) { char b[20]; snprintf(b, sizeof b, "%u.%u.%u.%u", a >> 24, a >> 16 & 255, a >> 8 & 255, a & 255); return std::string(b); };
    if (!ipfilter.empty()) dg.erase(std::remove_if(dg.begin(), dg.end(), [&](const Dgram &g) { return ipstr(g.sip).rfind(ipfilter, 0) != 0 && ipstr(g.dip).rfind(ipfilter, 0) != 0; }), dg.end()); // --ip 213.179.209. keeps traffic to or from that prefix
    if (dg.empty()) { fprintf(stderr, "no UDP datagrams%s found\n", sport ? " on that port" : ""); return 1; }
    double t0 = dg[0].t; int ok = 0, bad = 0;
    struct Key { u32 a; u16 ap; u32 b; u16 bp; bool operator<(const Key &o) const { return std::tie(a, ap, b, bp) < std::tie(o.a, o.ap, o.b, o.bp); } };
    std::map<std::tuple<Key, bool, int, u16>, std::map<u32, std::vector<u8>>> frags; // (flow, direction, channel, start seq) -> offset -> bytes
    std::map<std::tuple<Key, bool, int, u16>, u32> fragTotal;
    std::map<std::string, int> stats; std::set<std::tuple<Key, bool, int, u16>> seen_reliable; // retransmits of a reliable command are shown once
    for (const Dgram &g : dg)
    {
        std::vector<Cmd> cmds; bool from_client = false;
        if (!decode_datagram(g.d, cmds, from_client)) { ++bad; if (!stats_only && show_bad) printf("%9.3f ???? undecodable %zu-byte datagram %s\n", g.t - t0, g.d.size(), hexs(g.d.data(), g.d.size(), 24).c_str()); continue; }
        ++ok;
        if (!only.empty() && ((only == "client") != from_client)) continue;
        Key k{from_client ? g.sip : g.dip, from_client ? g.sport : g.dport, from_client ? g.dip : g.sip, from_client ? g.dport : g.sport};
        for (Cmd &c : cmds)
        {
            double t = g.t - t0;
            if (c.cmd == 2 && from_client) for (auto it = seen_reliable.begin(); it != seen_reliable.end(); ) it = std::get<0>(*it).ap == k.ap ? seen_reliable.erase(it) : std::next(it); // a new connection restarts the sequence numbers
            if ((c.cmd == 6 || c.cmd == 8 || c.cmd == 12) && !(c.cmd == 6 && false) && (c.channel < 255) && !seen_reliable.insert({k, from_client, c.channel, c.seq}).second && c.cmd != 7) continue;
            if (c.cmd == 2 || c.cmd == 3) { if (handshake && !stats_only) printf("%9.3f %s %s\n", t, from_client ? "C->S" : "S->C", c.cmd == 2 ? "ENET CONNECT" : "ENET VERIFY_CONNECT"); continue; }
            if (c.cmd == 4) { if (!stats_only) printf("%9.3f %s ENET DISCONNECT\n", t, from_client ? "C->S" : "S->C"); continue; }
            if (c.cmd == 6 || c.cmd == 7 || c.cmd == 9) { describe_message(t, from_client, c.data, nullptr, stats); continue; }
            if (c.cmd == 8 || c.cmd == 12)
            {
                auto key = std::make_tuple(k, from_client, c.channel, c.fstart);
                frags[key][c.foff] = c.data; fragTotal[key] = c.ftotal;
                size_t have = 0; for (auto &[o, v] : frags[key]) have += v.size();
                if (have >= c.ftotal)
                {
                    std::vector<u8> whole(c.ftotal); for (auto &[o, v] : frags[key]) if (o + v.size() <= whole.size()) memcpy(whole.data() + o, v.data(), v.size());
                    frags.erase(key); describe_message(t, from_client, whole, nullptr, stats);
                }
            }
        }
    }
    if (stats_only || true)
    {
        fprintf(report, "\n--- %d datagrams decoded, %d skipped as not Growtopia ENet (use --show-undecodable to list them)\n", ok, bad);
        if (stats_only) { std::vector<std::pair<int, std::string>> v; for (auto &[k, c] : stats) v.push_back({c, k}); std::sort(v.rbegin(), v.rend()); for (auto &[c, k] : v) fprintf(report, "%6d  %s\n", c, k.c_str()); }
        fflush(report);
    }
}
