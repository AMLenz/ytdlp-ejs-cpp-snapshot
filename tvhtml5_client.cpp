#include "tvhtml5_client.h"

#include <regex>
#include <sstream>
#include <stdexcept>
#include <string>

#include "http_utils.h"
#include "solver_engine.h"
#include "youtube_auth.h"
#include "youtube_page_utils.h"

Json::Value build_tvhtml5_player_request(const std::string& video_id,
                                         int sts,
                                         const AppConfig& cfg) {
    Json::Value root;
    root["videoId"] = video_id;
    root["contentCheckOk"] = true;
    root["racyCheckOk"] = true;
    root["context"]["client"]["clientName"] = cfg.player.tvhtml5_client_name;
    root["context"]["client"]["clientVersion"] = cfg.player.tvhtml5_client_version;
    root["playbackContext"]["contentPlaybackContext"]["signatureTimestamp"] = sts;
    return root;
}

std::string json_to_string(const Json::Value& root) {
    Json::StreamWriterBuilder builder;
    builder["indentation"] = "";
    return Json::writeString(builder, root);
}

std::string extract_effective_sts(const std::string& watch_html,
                                  const std::string& base_js,
                                  const AppConfig& cfg) {
    auto sts_watch = regex_first(watch_html, cfg.regex.sts_watch);
    auto sts_js    = regex_first(base_js, cfg.regex.sts_js);

    if (sts_js) return *sts_js;
    if (sts_watch) return *sts_watch;
    throw std::runtime_error("Keine STS gefunden");
}

Json::Value fetch_tvhtml5_player_response(const std::string& video_id,
                                          const std::string& cookie_file,
                                          const AppConfig& cfg) {
    auto cookies = load_netscape_cookies(cookie_file);
    std::string cookie_header = build_cookie_header(cookies);

    std::string sapisid;
    if (auto it = cookies.find("__Secure-3PAPISID"); it != cookies.end()) sapisid = it->second;
    else if (auto it = cookies.find("SAPISID"); it != cookies.end()) sapisid = it->second;
    else if (auto it = cookies.find("APISID"); it != cookies.end()) sapisid = it->second;

    std::string watch_headers;
    watch_headers += std::string("Accept-Language: ") + cfg.http.accept_language + "\r\n";
    watch_headers += std::string("Referer: ") + cfg.http.referer + "\r\n";
    if (!cookie_header.empty()) watch_headers += "Cookie: " + cookie_header + "\r\n";

    HttpResponse watch_resp = https_request(cfg.http.host, "GET", "/watch?v=" + video_id, watch_headers, "");
    if (watch_resp.status != 200) {
        throw std::runtime_error("Watch-Seite lieferte HTTP " + std::to_string(watch_resp.status));
    }

    auto api_key = regex_first(watch_resp.body, cfg.regex.api_key);
    if (!api_key) {
        api_key = regex_first(watch_resp.body, cfg.regex.api_key_alt);
    }
    if (!api_key) throw std::runtime_error("INNERTUBE_API_KEY nicht gefunden");

    auto js_url_rel = regex_first(watch_resp.body, cfg.regex.js_url);
    if (!js_url_rel) throw std::runtime_error("jsUrl nicht gefunden");

    std::string js_path = *js_url_rel;
    if (js_path.rfind("https://", 0) == 0) {
        std::smatch m;
        if (!std::regex_match(js_path, m, std::regex(R"(^https://[^/]+(/.*)$)"))) {
            throw std::runtime_error("jsUrl-Format unerwartet");
        }
        js_path = m[1].str();
    }

    HttpResponse js_resp = https_request(
        cfg.http.host,
        "GET",
        js_path,
        std::string("Referer: ") + cfg.http.referer + "\r\n",
        ""
    );
    if (js_resp.status != 200) {
        throw std::runtime_error("base.js lieferte HTTP " + std::to_string(js_resp.status));
    }

    int sts = std::stoi(extract_effective_sts(watch_resp.body, js_resp.body, cfg));

    Json::Value req_json = build_tvhtml5_player_request(video_id, sts, cfg);
    std::string req_body = json_to_string(req_json);

    std::string player_headers;
    player_headers += "Content-Type: application/json\r\n";
    player_headers += "X-YouTube-Client-Name: " + cfg.player.header_client_name + "\r\n";
    player_headers += "X-YouTube-Client-Version: " + cfg.player.header_client_version + "\r\n";
    player_headers += "X-Goog-AuthUser: " + cfg.player.auth_user + "\r\n";
    player_headers += std::string("X-Origin: ") + cfg.http.origin + "\r\n";
    player_headers += std::string("Origin: ") + cfg.http.origin + "\r\n";
    if (!cookie_header.empty()) player_headers += "Cookie: " + cookie_header + "\r\n";
    if (!sapisid.empty()) {
        player_headers += "Authorization: SAPISIDHASH " + generate_sapisid_hash(sapisid) + "\r\n";
    }

    HttpResponse player_resp = https_request(
        cfg.http.host,
        "POST",
        "/youtubei/v1/player?key=" + *api_key,
        player_headers,
        req_body
    );

    if (player_resp.status != 200) {
        throw std::runtime_error("Player-POST lieferte HTTP " + std::to_string(player_resp.status));
    }

    Json::Value player_json;
    std::string errs;
    if (!parse_json(player_resp.body, player_json, errs)) {
        throw std::runtime_error("Player-JSON parse fehlgeschlagen: " + errs);
    }

    return player_json;
}

TvHtml5Context init_tvhtml5_context(const std::string& cookie_file,
                                    const std::string& seed_video_id,
                                    const std::string& solver_dir,
                                    const AppConfig& cfg) {
    TvHtml5Context ctx;

    ctx.cookies = load_netscape_cookies(cookie_file);
    ctx.cookie_header = build_cookie_header(ctx.cookies);

    if (auto it = ctx.cookies.find("__Secure-3PAPISID"); it != ctx.cookies.end()) ctx.sapisid = it->second;
else if (auto it = ctx.cookies.find("SAPISID"); it != ctx.cookies.end()) ctx.sapisid = it->second;
else if (auto it = ctx.cookies.find("APISID"); it != ctx.cookies.end()) ctx.sapisid = it->second;

    std::string watch_headers;
    watch_headers += std::string("Accept-Language: ") + cfg.http.accept_language + "\r\n";
    watch_headers += std::string("Referer: ") + cfg.http.referer + "\r\n";
    if (!ctx.cookie_header.empty()) watch_headers += "Cookie: " + ctx.cookie_header + "\r\n";

    HttpResponse watch_resp = https_request(cfg.http.host, "GET", "/watch?v=" + seed_video_id, watch_headers, "");
    if (watch_resp.status != 200) {
        throw std::runtime_error("Watch-Seite lieferte HTTP " + std::to_string(watch_resp.status));
    }

    auto api_key = regex_first(watch_resp.body, cfg.regex.api_key);
    if (!api_key) {
        api_key = regex_first(watch_resp.body, cfg.regex.api_key_alt);
    }
    if (!api_key) throw std::runtime_error("INNERTUBE_API_KEY nicht gefunden");
    ctx.api_key = *api_key;

    auto js_url_rel = regex_first(watch_resp.body, cfg.regex.js_url);
    if (!js_url_rel) throw std::runtime_error("jsUrl nicht gefunden");

    ctx.js_url = *js_url_rel;
    if (ctx.js_url.rfind("https://", 0) == 0) {
        std::smatch m;
        if (!std::regex_match(ctx.js_url, m, std::regex(R"(^https://[^/]+(/.*)$)"))) {
            throw std::runtime_error("jsUrl-Format unerwartet");
        }
        ctx.js_url = m[1].str();
    }

    HttpResponse js_resp = https_request(
        cfg.http.host,
        "GET",
        ctx.js_url,
        std::string("Referer: ") + cfg.http.referer + "\r\n",
        ""
    );
    if (js_resp.status != 200) {
        throw std::runtime_error("base.js lieferte HTTP " + std::to_string(js_resp.status));
    }

    ctx.base_js = js_resp.body;
    ctx.sts = std::stoi(extract_effective_sts(watch_resp.body, js_resp.body, cfg));
    ctx.solver = load_solver_assets_from_code(ctx.base_js, solver_dir);
    return ctx;
}

Json::Value fetch_tvhtml5_player_response_fast(const TvHtml5Context& ctx,
                                               const std::string& video_id,
                                               const AppConfig& cfg) {
    Json::Value req_json = build_tvhtml5_player_request(video_id, ctx.sts, cfg);
    std::string req_body = json_to_string(req_json);

    std::string player_headers;
    player_headers += "Content-Type: application/json\r\n";
    player_headers += "X-YouTube-Client-Name: " + cfg.player.header_client_name + "\r\n";
    player_headers += "X-YouTube-Client-Version: " + cfg.player.header_client_version + "\r\n";
    player_headers += "X-Goog-AuthUser: " + cfg.player.auth_user + "\r\n";
    player_headers += std::string("X-Origin: ") + cfg.http.origin + "\r\n";
    player_headers += std::string("Origin: ") + cfg.http.origin + "\r\n";
    if (!ctx.cookie_header.empty()) player_headers += "Cookie: " + ctx.cookie_header + "\r\n";
    if (!ctx.sapisid.empty()) {
        player_headers += "Authorization: SAPISIDHASH " + generate_sapisid_hash(ctx.sapisid) + "\r\n";
    }

    HttpResponse player_resp = https_request(
        cfg.http.host,
        "POST",
        "/youtubei/v1/player?key=" + ctx.api_key,
        player_headers,
        req_body
    );

    if (player_resp.status != 200) {
        throw std::runtime_error("Player-POST lieferte HTTP " + std::to_string(player_resp.status));
    }

    Json::Value player_json;
    std::string errs;
    if (!parse_json(player_resp.body, player_json, errs)) {
        throw std::runtime_error("Player-JSON parse fehlgeschlagen: " + errs);
    }

    return player_json;
}
