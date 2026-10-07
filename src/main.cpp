// main.cpp — PS5 payload entry.
//
// ps5-payload-sdk expects a standard main(). The SDK handles the payload
// bootstrap (ELF loader hands off to _start which calls main). We ignore argc/argv.
#include "config.h"
#include "download_manager.h"
#include "http_client.h"
#include "logger.h"
#include "settings.h"
#include "ui.h"

#include <SDL2/SDL.h>
#include <sys/stat.h>
#include <unistd.h>

static void ensure_dirs() {
    mkdir(fs_cfg::APP_DATA_DIR,   0755);
    mkdir(fs_cfg::APP_CACHE_DIR,  0755);
    mkdir(fs_cfg::APP_TEMP_DIR,   0755);
    mkdir(fs_cfg::APP_COVERS_DIR, 0755);
}

int main(int /*argc*/, char** /*argv*/) {
    ensure_dirs();
    fs_log::init(fs_cfg::APP_LOG_FILE);
    LOGI("%s %s starting", APP_NAME, APP_VERSION);

    fs_set::load();
    fs_net::global_init();

    fs_dl::Manager::instance().load_state();
    fs_dl::Manager::instance().start(fs_set::current().parallel_downloads);

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER) != 0) {
        LOGE("SDL_Init: %s", SDL_GetError());
        return 1;
    }

    // Open all connected controllers for DualSense navigation.
    for (int i = 0; i < SDL_NumJoysticks(); ++i) {
        if (SDL_IsGameController(i)) SDL_GameControllerOpen(i);
    }

    SDL_Window* win = SDL_CreateWindow(
        APP_NAME, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        fs_cfg::WINDOW_W, fs_cfg::WINDOW_H,
        SDL_WINDOW_SHOWN | SDL_WINDOW_FULLSCREEN_DESKTOP);
    if (!win) {
        LOGE("SDL_CreateWindow: %s", SDL_GetError());
        return 2;
    }

    SDL_Renderer* ren = SDL_CreateRenderer(win, -1,
        SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!ren) {
        LOGE("SDL_CreateRenderer: %s", SDL_GetError());
        return 3;
    }

    auto* ui = fs_ui::init(win, ren);

    bool running = true;
    while (running) {
        running = fs_ui::frame(ui, win, ren);
    }

    fs_ui::shutdown(ui);
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_Quit();

    fs_dl::Manager::instance().stop();
    fs_net::global_shutdown();
    fs_log::shutdown();
    return 0;
}
