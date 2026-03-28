#include "solver_engine.h"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <utility>

#include "url_query_utils.h"

namespace fs = std::filesystem;

void require_regular_file(const std::string& path, std::string_view label) {
    if (!fs::exists(path)) {
        throw std::runtime_error(std::string(label) + " nicht gefunden: " + path);
    }
    if (!fs::is_regular_file(path)) {
        throw std::runtime_error(std::string(label) + " ist keine reguläre Datei: " + path);
    }
}

std::string read_text_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("Konnte Datei nicht lesen: " + path);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

SolverEngine::SolverEngine(const SolverAssets& assets)
    : assets_(assets), rt_(JS_NewRuntime()), ctx_(JS_NewContext(rt_.get())) {
    if (!rt_) throw std::runtime_error("JS_NewRuntime fehlgeschlagen");
    if (!ctx_) throw std::runtime_error("JS_NewContext fehlgeschlagen");

    eval_script(assets_.meriyah_code, "meriyah.umd.js");
    eval_script(assets_.astring_code, "astring.min.js");
    eval_script(assets_.lib_code, "lib.min.js");
    eval_script(assets_.core_code, "core.min.js");
}

void SolverEngine::eval_script(const std::string& code, const char* fname) {
    JsValueOwner v(ctx_.get(),
                   JS_Eval(ctx_.get(), code.c_str(), code.size(), fname, JS_EVAL_TYPE_GLOBAL));
    if (JS_IsException(v.get())) throw std::runtime_error(get_quickjs_exception(ctx_.get()));
}

std::vector<SolverResponse> SolverEngine::solve(const std::vector<SolverRequest>& requests) {
    Json::Value req_root;
    req_root["type"] = "player";
    req_root["player"] = assets_.player_code;
    req_root["output_preprocessed"] = true;

    Json::Value req_arr(Json::arrayValue);
    for (const auto& r : requests) {
        Json::Value x;
        x["type"] = r.type;
        Json::Value challenges(Json::arrayValue);
        challenges.append(r.challenge);
        x["challenges"] = challenges;
        req_arr.append(x);
    }
    req_root["requests"] = req_arr;

    JsValueOwner global(ctx_.get(), JS_GetGlobalObject(ctx_.get()));
    JsValueOwner jsc_func(ctx_.get(), JS_GetPropertyStr(ctx_.get(), global.get(), "jsc"));
    if (!JS_IsFunction(ctx_.get(), jsc_func.get())) {
        throw std::runtime_error("jsc ist keine Funktion");
    }

    JsValueOwner arg(ctx_.get(), json_to_js_qjs(ctx_.get(), req_root));
    JSValueConst argv_local[] = {arg.get()};
    JsValueOwner ret(ctx_.get(), JS_Call(ctx_.get(), jsc_func.get(), JS_UNDEFINED, 1, argv_local));
    if (JS_IsException(ret.get())) throw std::runtime_error(get_quickjs_exception(ctx_.get()));

    const Json::Value ret_json = js_to_json_qjs(ctx_.get(), ret.get());
    const Json::Value& responses = ret_json["responses"];
    if (!responses.isArray()) {
        throw std::runtime_error("Solver-Antwort enthält kein responses-Array");
    }

    std::vector<SolverResponse> out;
    out.reserve(requests.size());

    for (Json::ArrayIndex i = 0; i < responses.size(); ++i) {
        SolverResponse sr;
        sr.type = (i < requests.size()) ? requests[i].type : "";

        const Json::Value& r = responses[i];
        if (r.get("type", "").asString() == "result") {
            sr.ok = true;
            const Json::Value& data = r["data"];
            if (data.isObject()) {
                for (auto it = data.begin(); it != data.end(); ++it) {
                    if (it->isString()) {
                        sr.data = it->asString();
                        break;
                    }
                }
            } else if (data.isString()) {
                sr.data = data.asString();
            }
        } else {
            sr.ok = false;
            sr.error = r.get("error", "unknown solver error").asString();
        }
        out.push_back(std::move(sr));
    }

    return out;
}

SolverAssets load_solver_assets_from_code(const std::string& base_js_code, const std::string& solver_dir) {
    require_regular_file("meriyah.umd.js", "meriyah.umd.js");
    require_regular_file("astring.min.js", "astring.min.js");
    require_regular_file(solver_dir + "/lib.min.js", "lib.min.js");
    require_regular_file(solver_dir + "/core.min.js", "core.min.js");

    SolverAssets assets;
    assets.meriyah_code = read_text_file("meriyah.umd.js");
    assets.astring_code = read_text_file("astring.min.js");
    assets.lib_code = read_text_file(solver_dir + "/lib.min.js");
    assets.core_code = read_text_file(solver_dir + "/core.min.js");
    assets.player_code = base_js_code;
    assets.cache = std::make_shared<SolverSharedCache>();
    return assets;
}

static SolverEngine& thread_local_solver_engine(const SolverAssets& assets) {
    thread_local std::unique_ptr<SolverEngine> engine;
    if (!engine) engine = std::make_unique<SolverEngine>(assets);
    return *engine;
}

SolvedUrlResult solve_format_url(const FormatInfo& fmt, const SolverAssets& assets) {
    SolvedUrlResult out;
    out.decoded_url = fmt.url;

    if (fmt.url.empty()) {
        out.solver_error = "Keine URL vorhanden";
        return out;
    }
    if (fmt.requests.empty()) return out;

    auto cache = assets.cache;
    if (!cache) {
        out.solver_error = "Solver-Cache fehlt";
        return out;
    }

    std::vector<std::pair<std::string, std::string>> ready;
    std::vector<SolverRequest> missing;

    {
        std::lock_guard<std::mutex> lock(cache->mutex);
        for (const auto& req : fmt.requests) {
            if (req.type == "n") {
                auto it = cache->n_values.find(req.challenge);
                if (it != cache->n_values.end()) {
                    ready.push_back(std::make_pair(req.type, it->second));
                    continue;
                }
                auto eit = cache->n_errors.find(req.challenge);
                if (eit != cache->n_errors.end()) {
                    if (!out.solver_error.empty()) out.solver_error += " | ";
                    out.solver_error += eit->second;
                    continue;
                }
                missing.push_back(req);
            } else if (req.type == "sig") {
                auto it = cache->sig_values.find(req.challenge);
                if (it != cache->sig_values.end()) {
                    ready.push_back(std::make_pair(req.type, it->second));
                    continue;
                }
                auto eit = cache->sig_errors.find(req.challenge);
                if (eit != cache->sig_errors.end()) {
                    if (!out.solver_error.empty()) out.solver_error += " | ";
                    out.solver_error += eit->second;
                    continue;
                }
                missing.push_back(req);
            } else {
                missing.push_back(req);
            }
        }
    }

    if (!missing.empty()) {
        try {
            auto& engine = thread_local_solver_engine(assets);
            const auto responses = engine.solve(missing);

            std::lock_guard<std::mutex> lock(cache->mutex);
            for (size_t i = 0; i < missing.size() && i < responses.size(); ++i) {
                const auto& req = missing[i];
                const auto& resp = responses[i];
                if (resp.ok) {
                    if (req.type == "n") cache->n_values[req.challenge] = resp.data;
                    else if (req.type == "sig") cache->sig_values[req.challenge] = resp.data;
                    ready.push_back(std::make_pair(req.type, resp.data));
                } else {
                    if (req.type == "n") cache->n_errors[req.challenge] = resp.error;
                    else if (req.type == "sig") cache->sig_errors[req.challenge] = resp.error;
                    if (!out.solver_error.empty()) out.solver_error += " | ";
                    out.solver_error += resp.error;
                }
            }
        } catch (const std::exception& e) {
            out.solver_error = e.what();
            return out;
        }
    }

    std::string current = fmt.url;
    for (const auto& kv : ready) {
        if (kv.first == "n") {
            current = set_or_replace_query_param(current, "n", kv.second);
        } else if (kv.first == "sig") {
            const std::string sp = fmt.sp.empty() ? "signature" : fmt.sp;
            current = set_or_replace_query_param(current, sp, kv.second);
        }
    }

    out.decoded_url = current;
    return out;
}
