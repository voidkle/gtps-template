// the HTTPS side of the login (docs: guide/official-login steps 1-2): server_data.php and the login site
#pragma once
#include "config.hpp"
#include "store.hpp"
#include <thread>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <csignal>
#include <openssl/err.h>
#include <openssl/ssl.h>

namespace https
{
inline std::string urldecode(const std::string &s)
{
    std::string o;
    for (size_t i = 0; i < s.size(); ++i)
        if (s[i] == '%' && i + 2 < s.size() && isxdigit((unsigned char)s[i + 1]) && isxdigit((unsigned char)s[i + 2])) { o += (char)std::stoi(s.substr(i + 1, 2), nullptr, 16); i += 2; }
        else o += s[i] == '+' ? ' ' : s[i];
    return o;
}
inline std::string urlencode(const std::string &s)
{
    std::string o; char b[4];
    for (unsigned char c : s) { if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') o += c; else { snprintf(b, 4, "%%%02X", c); o += b; } }
    return o;
}
inline std::string form_get(const std::string &body, const std::string &key)
{
    size_t pos = 0;
    while (pos <= body.size())
    {
        size_t e = body.find('&', pos); if (e == std::string::npos) e = body.size();
        std::string kv = body.substr(pos, e - pos); size_t eq = kv.find('=');
        if (eq != std::string::npos && urldecode(kv.substr(0, eq)) == key) return urldecode(kv.substr(eq + 1));
        pos = e + 1;
    }
    return {};
}
inline std::string html_escape(const std::string &s)
{
    std::string o; for (char c : s) { if (c == '<') o += "&lt;"; else if (c == '>') o += "&gt;"; else if (c == '&') o += "&amp;"; else if (c == '"') o += "&quot;"; else o += c; } return o;
}

inline std::string page(const std::string &client_data, const std::string &error, const std::string &name)
{
    return R"(<!doctype html><html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Growtopia</title><style>
body{margin:0;background:#1b2a3a;color:#fff;font-family:sans-serif;display:flex;justify-content:center;padding:24px}
form{width:100%;max-width:340px;background:#2c4258;border-radius:10px;padding:20px}
h1{font-size:22px;margin:0 0 14px}input[type=text],input[type=password]{width:100%;box-sizing:border-box;padding:10px;margin:6px 0 12px;border-radius:6px;border:0;font-size:16px}
button{width:100%;padding:12px;border:0;border-radius:6px;background:#4cae4c;color:#fff;font-size:16px}
.err{background:#a33;border-radius:6px;padding:8px;margin-bottom:12px}label.c{font-size:14px;display:block;margin-bottom:12px}</style></head><body>
<form method="POST" action="/player/growid/login/validate"><h1>Log on with your GrowID</h1>)" +
           (error.empty() ? "" : "<div class=\"err\">" + html_escape(error) + "</div>") +
           "<input type=\"hidden\" name=\"clientData\" value=\"" + html_escape(client_data) + "\">"
           "GrowID<input type=\"text\" name=\"growId\" value=\"" + html_escape(name) + "\" autocapitalize=\"none\" autocomplete=\"username\">"
           "Password<input type=\"password\" name=\"password\" autocomplete=\"current-password\">"
           "<label class=\"c\"><input type=\"checkbox\" name=\"register\" value=\"1\"> This is a new GrowID, create it</label>"
           "<button type=\"submit\">Log on</button></form></body></html>";
}

inline std::string json_ok(const std::string &token)
{
    return "{\"status\":\"success\",\"message\":\"Account Validated.\",\"token\":\"" + token + "\",\"url\":\"\",\"accountType\":\"growtopia\",\"accountAge\":25}";
}

struct Req { std::string method, path, query, body; };

inline std::string reply(int code, const std::string &type, const std::string &body, const std::string &extra = "")
{
    const char *msg = code == 200 ? "OK" : code == 302 ? "Found" : code == 404 ? "Not Found" : "Error";
    return "HTTP/1.1 " + std::to_string(code) + " " + msg + "\r\nContent-Type: " + type + "\r\nContent-Length: " + std::to_string(body.size()) +
           "\r\nConnection: close\r\n" + extra + "\r\n" + body;
}

inline std::string route(const Req &r, Config &cfg, Store &st)
{
    const std::string html = "text/html; charset=utf-8";
    if (r.path == "/growtopia/server_data.php") return reply(200, "text/plain", cfg.server_data());

    std::string client_data = form_get(r.body, "clientData"); if (client_data.empty()) client_data = form_get(r.query, "clientData");
    if (r.path == "/player/login/dashboard" || (r.path == "/player/growid/login/validate" && r.method != "POST"))
        return reply(200, html, page(client_data, "", form_get(client_data, "tankIDName")));

    if (r.path == "/player/growid/login/validate")
    {
        std::string token;
        std::string err = st.login_or_register(form_get(r.body, "growId"), form_get(r.body, "password"), token, cfg.register_ok && form_get(r.body, "register") == "1");
        if (!err.empty()) return reply(200, html, page(client_data, err, form_get(r.body, "growId")));
        return reply(302, html, "", "Location: /player/growid/validate/checktoken/" + urlencode(token) + "\r\n");
    }
    if (r.path == "/player/growid/checktoken") // a saved token: valid -> same hand-over page, else the login page
    {
        std::string tok = form_get(r.body, "refreshToken"); if (tok.empty()) tok = form_get(r.query, "refreshToken");
        std::lock_guard g(st.mu);
        if (st.by_web_token(tok)) return reply(302, html, "", "Location: /player/growid/validate/checktoken/" + urlencode(tok) + "\r\n");
        return reply(200, html, page(client_data, "", form_get(client_data, "tankIDName")));
    }
    const std::string pre = "/player/growid/validate/checktoken/";
    if (r.path.rfind(pre, 0) == 0) return reply(200, html, json_ok(urldecode(r.path.substr(pre.size()))));
    return reply(404, "text/plain", "not found");
}

inline void serve(SSL *ssl, Config &cfg, Store &st)
{
    std::string data; char buf[4096];
    size_t hend = std::string::npos, need = 0;
    while (data.size() < (1 << 20))
    {
        int n = SSL_read(ssl, buf, sizeof buf); if (n <= 0) break;
        data.append(buf, n);
        if (hend == std::string::npos && (hend = data.find("\r\n\r\n")) != std::string::npos)
        {
            std::string head = data.substr(0, hend); size_t cl = head.find("Content-Length:");
            if (cl == std::string::npos) cl = head.find("content-length:");
            need = hend + 4 + (cl == std::string::npos ? 0 : std::stoul(head.substr(cl + 15)));
        }
        if (hend != std::string::npos && data.size() >= need) break;
    }
    if (hend == std::string::npos) return;
    Req r; size_t sp1 = data.find(' '), sp2 = data.find(' ', sp1 + 1);
    r.method = data.substr(0, sp1); std::string target = data.substr(sp1 + 1, sp2 - sp1 - 1);
    size_t q = target.find('?'); r.path = target.substr(0, q); if (q != std::string::npos) r.query = target.substr(q + 1);
    r.body = data.substr(hend + 4);
    LOG("[https] %s %s\n", r.method.c_str(), r.path.c_str());
    std::string out = route(r, cfg, st);
    for (size_t off = 0; off < out.size(); ) { int n = SSL_write(ssl, out.data() + off, out.size() - off); if (n <= 0) break; off += n; }
}

inline void listener(Config &cfg, Store &st)
{
    signal(SIGPIPE, SIG_IGN);
    SSL_CTX *ctx = SSL_CTX_new(TLS_server_method());
    if (SSL_CTX_use_certificate_file(ctx, cfg.cert.c_str(), SSL_FILETYPE_PEM) != 1 || SSL_CTX_use_PrivateKey_file(ctx, cfg.key.c_str(), SSL_FILETYPE_PEM) != 1)
    { ERR_print_errors_fp(stderr); fprintf(stderr, "https: can't load %s / %s\n", cfg.cert.c_str(), cfg.key.c_str()); return; }
    int s = socket(AF_INET, SOCK_STREAM, 0), one = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    sockaddr_in a{}; a.sin_family = AF_INET; a.sin_addr.s_addr = INADDR_ANY; a.sin_port = htons(cfg.https_port);
    if (bind(s, (sockaddr *)&a, sizeof a) < 0 || listen(s, 64) < 0) { fprintf(stderr, "https: can't listen on port %d: %s (below 1024 needs setcap cap_net_bind_service)\n", cfg.https_port, strerror(errno)); return; }
    printf("https listening on :%d\n", cfg.https_port);
    for (;;)
    {
        int fd = accept(s, nullptr, nullptr); if (fd < 0) continue;
        std::thread([=, &cfg, &st] {
            timeval tv{10, 0}; setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
            SSL *ssl = SSL_new(ctx); SSL_set_fd(ssl, fd);
            if (SSL_accept(ssl) > 0) serve(ssl, cfg, st);
            else { char ip[64] = "?"; sockaddr_in pa{}; socklen_t pl = sizeof pa; if (getpeername(fd, (sockaddr *)&pa, &pl) == 0) inet_ntop(AF_INET, &pa.sin_addr, ip, sizeof ip); unsigned long e = ERR_peek_last_error(); LOG("[https] TLS handshake from %s failed: %s\n", ip, e ? ERR_reason_error_string(e) : "closed"); ERR_clear_error(); }
            SSL_shutdown(ssl); SSL_free(ssl); close(fd);
        }).detach();
    }
}
}
