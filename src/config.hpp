#pragma once
#include "proto.hpp"
#include <fstream>
#include <map>

// gtps.conf: "key value" per line, '#' comments
struct Config
{
    std::string ip = "127.0.0.1";      // address the client connects to (server_data.php `server`)
    int port = 17091;                  // ENet
    std::string bind_ip;               // listen only on this address (empty = all); lets a capture proxy sit on another loopback address
    int public_port = 0;               // port clients are told to use if it differs (port forwarding, a capture proxy); 0 = same as port
    int https_port = 443;
    std::string loginurl = "127.0.0.1";// host of the login site; must match the TLS certificate
    std::string meta = "gtps";
    std::string maint;                 // non-empty = maintenance message
    int type = 1, type2 = 1;
    bool register_ok = true;           // the login page may create new GrowIDs
    int protocol = 227;                // `proto` we tell the client (see docs: OnSuperMainStartAcceptLogon)
    std::string cdn_host = "ubistatic-a.akamaihd.net", cdn_path = "0098/024920264/cache/";
    std::string cert = "resources/ctx/server.crt", key = "resources/ctx/server.key";
    std::string data_dir = "data", items_path = "items.dat", store_path = "resources/store.txt", punch_path = "resources/punch.txt", res_dir = "resources";
    int max_players = 50;

    void load(const std::string &path)
    {
        std::ifstream f(path);
        for (std::string line; std::getline(f, line); )
        {
            if (line.empty() || line[0] == '#') continue;
            size_t sp = line.find(' '); if (sp == std::string::npos) continue;
            std::string k = line.substr(0, sp), v = line.substr(sp + 1);
            if (size_t hash = v.find(" #"); hash != std::string::npos) v.erase(hash);          // a trailing "  # comment"
            while (!v.empty() && (v.back() == ' ' || v.back() == '\t' || v.back() == '\r')) v.pop_back();
            if (k == "ip") ip = v; else if (k == "port") port = std::stoi(v); else if (k == "public_port") public_port = std::stoi(v); else if (k == "bind_ip") bind_ip = v; else if (k == "https_port") https_port = std::stoi(v);
            else if (k == "loginurl") loginurl = v; else if (k == "meta") meta = v; else if (k == "maint") maint = v;
            else if (k == "register") register_ok = v != "0"; else if (k == "protocol") protocol = std::stoi(v);
            else if (k == "cdn_host") cdn_host = v; else if (k == "cdn_path") cdn_path = v;
            else if (k == "cert") cert = v; else if (k == "key") key = v;
            else if (k == "data_dir") data_dir = v; else if (k == "items") items_path = v; else if (k == "store") store_path = v; else if (k == "punch") punch_path = v; else if (k == "resources") res_dir = v;
            else if (k == "max_players") max_players = std::stoi(v);
        }
    }

    std::string server_data() const
    {
        return "server|" + ip + "\nport|" + std::to_string(public_port ? public_port : port) + "\ntype|" + std::to_string(type) + "\ntype2|" + std::to_string(type2) +
               "\n" + (maint.empty() ? "#maint|" : "maint|") + maint + "\nloginurl|" + loginurl + "\nmeta|" + meta + "\nRTENDMARKERBS1001";
    }
};
