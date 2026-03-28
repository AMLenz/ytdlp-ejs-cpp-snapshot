/* watch_ids_with_titles.cpp
//
// Compile:
g++ -std=c++17 -Os -Wall -Wextra -I. -Iyoube-files \
  -o yoube_get \
  yoube_get_7.cpp \
  yoube-files/app_config.cpp \
  yoube-files/http_utils.cpp \
  yoube-files/youtube_auth.cpp \
  yoube-files/youtube_page_utils.cpp \
  yoube-files/tvhtml5_client.cpp \
  yoube-files/common_menu.cpp \
  yoube-files/url_query_utils.cpp \
  yoube-files/quickjs_json_utils.cpp \
  yoube-files/solver_engine.cpp \
  yoube-files/format_utils.cpp \
  yoube-files/selection_output.cpp \
  shared_cli_json.cpp \
  -lssl -lcrypto `pkg-config --cflags --libs jsoncpp` \
  -I quickjs -L quickjs -lquickjs

./yoube_get "https://www.youtube.com/watch?v=IxuEtL7gxoM&list=PLcYXtqQvQHKFlKY3P1p2zO2av-xlYgo_P" --tvhtml5
*/

#include <iostream>
#include <string>
#include <vector>
#include <thread>
#include <future>

#include <json/json.h>

#include "shared_cli_json.h"
#include "yoube-files/app_config.h"
#include "yoube-files/yoube_model.h"
#include "yoube-files/youtube_page_utils.h"
#include "yoube-files/tvhtml5_client.h"
#include "yoube-files/common_menu.h"
#include "yoube-files/selection_output.h"

static void print_help(const char* prog) {
    std::cout
        << "Usage:\n"
        << "  " << prog << " <youtube-url-or-video-id>\n"
        << "  " << prog << " <youtube-url-or-video-id> --tvhtml5 [--loose-common|--medium-common|--strict-common]\n"
        << "  " << prog << " --tvhtml5-id <VIDEO_ID>\n"
        << "  " << prog << " --help\n"
        << "  " << prog << " -h\n\n"

        << "Modes:\n"
        << "  --tvhtml5         Use TVHTML5 player responses and open the interactive menu system.\n"
        << "  --loose-common    Common formats by same itag (default).\n"
        << "  --medium-common   Common formats by same type/container/quality/fps/audio-base,\n"
        << "                    while allowing differing pixel dimensions.\n"
        << "  --strict-common   Common formats only if properties match strictly.\n"
        << "  --tvhtml5-id ID   Fetch exactly one TVHTML5 player response by video id.\n\n"

        << "Interactive flow with --tvhtml5:\n"
        << "  1) Select one or more videos\n"
        << "  2) Choose mode:\n"
        << "       - common formats across selected videos\n"
        << "       - or per-video selection\n"
        << "  3) Choose format group:\n"
        << "       1 = muxed\n"
        << "       2 = adaptive video\n"
        << "       3 = audio-only\n"
        << "       4 = all\n"
        << "  4) Choose one or more formats\n"
        << "  5) For adaptive video selections, the best matching audio is added automatically\n\n"

        << "Selection syntax inside menus:\n"
        << "  1           single item\n"
        << "  1,3,5       multiple items\n"
        << "  2-6         range\n"
        << "  all         all displayed items\n\n";
}

int main(int argc, char** argv) {
    try {
        AppConfig cfg = load_app_config("yt_config.json");

        if (argc >= 2) {
            std::string arg1 = argv[1];
            if (arg1 == "--help" || arg1 == "-h") {
                print_help(argv[0]);
                return 0;
            }
        }

        if (argc >= 3 && std::string(argv[1]) == "--tvhtml5-id") {
            TvHtml5Context ctx = init_tvhtml5_context(
                cfg.paths.cookies,
                argv[2],
                cfg.paths.solver_dir,
                cfg
            );

            Json::Value player = fetch_tvhtml5_player_response_fast(ctx, argv[2], cfg);
            interactive_format_menu(player, argv[2], "", ctx);
            return 0;
        }

        if (argc < 2) {
            print_help(argv[0]);
            return 1;
        }

        std::string input = argv[1];
        bool use_tvhtml5 = false;
        CommonMatchMode common_mode = CommonMatchMode::LooseItag;

        for (int i = 2; i < argc; ++i) {
            std::string arg = argv[i];
            if (arg == "--tvhtml5") {
                use_tvhtml5 = true;
            } else if (arg == "--strict-common") {
                common_mode = CommonMatchMode::StrictProps;
            } else if (arg == "--medium-common") {
                common_mode = CommonMatchMode::MediumProps;
            } else if (arg == "--loose-common") {
                common_mode = CommonMatchMode::LooseItag;
            }
        }

        std::vector<VideoEntry> entries =
            is_channel_videos_url(input)
                ? extract_channel_entries(input, cfg)
                : extract_watch_entries(input, cfg);

        if (entries.empty()) {
            std::cerr << "No video entries found.\n";
            return 2;
        }

        std::cout << "Found " << entries.size() << " video entr"
                  << (entries.size() == 1 ? "y" : "ies") << ":\n";
        for (size_t i = 0; i < entries.size(); ++i) {
            std::cout << (i + 1) << ": " << entries[i].videoId;
            if (!entries[i].title.empty()) std::cout << " | " << entries[i].title;
            std::cout << "\n";
        }

        if (use_tvhtml5) {
            std::cout << "\n=== Initializing TVHTML5 context once ===\n";

            TvHtml5Context ctx = init_tvhtml5_context(
                cfg.paths.cookies,
                entries[0].videoId,
                cfg.paths.solver_dir,
                cfg
            );
            // std::cout << ctx.base_js << std::endl; // raw base.js für Decoder

            const size_t max_parallel =
                std::max<size_t>(
                    1,
                    std::min<size_t>(
                        8,
                        std::thread::hardware_concurrency()
                            ? std::thread::hardware_concurrency()
                            : 4
                    )
                );

            std::vector<TvResult> results(entries.size());
            std::vector<std::future<TvResult>> futures;

            auto launch_one = [&](size_t i) {
                return std::async(std::launch::async, [&, i]() -> TvResult {
                    TvResult r;
                    r.index = i;
                    r.videoId = entries[i].videoId;
                    r.title = entries[i].title;

                    try {
                        r.player = fetch_tvhtml5_player_response_fast(ctx, entries[i].videoId, cfg);
                    } catch (const std::exception& e) {
                        r.error = e.what();
                    } catch (...) {
                        r.error = "unknown error";
                    }

                    return r;
                });
            };

            size_t next = 0;
            while (next < entries.size() || !futures.empty()) {
                while (next < entries.size() && futures.size() < max_parallel) {
                    futures.push_back(launch_one(next));
                    ++next;
                }

                TvResult r = futures.front().get();
                futures.erase(futures.begin());
                results[r.index] = std::move(r);
            }

            Json::Value root = interactive_video_selection_menu(results, ctx, common_mode);
            print_json_pretty(root);

            if (save_json_file(root, cfg.paths.output)) {
                std::cout << "JSON gespeichert: " << cfg.paths.output << "\n";
            } else {
                std::cout << "Konnte " << cfg.paths.output << " nicht schreiben.\n";
            }

            return 0;
        }

        return 0;
    } catch (const std::exception& e) {
        std::cerr << "ERROR: " << e.what() << "\n";
        return 1;
    }
}
