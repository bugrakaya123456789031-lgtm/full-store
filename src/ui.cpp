// ui.cpp — ImGui + SDL2 frontend.
//
// Depends on vendored ImGui + imgui_impl_sdl2 + imgui_impl_sdlrenderer2
// (fetched by the SDK's port system or vendored under third_party/imgui).
#include "ui.h"
#include "config.h"
#include "download_manager.h"
#include "http_client.h"
#include "logger.h"
#include "resolver.h"
#include "scraper.h"
#include "settings.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <SDL2/SDL.h>
#include <SDL2/SDL_image.h>

#include "imgui.h"
#include "imgui_impl_sdl2.h"
#include "imgui_impl_sdlrenderer2.h"

namespace fs_ui {

namespace {

std::string fmt_bytes(int64_t b) {
    if (b < 0) return "?";
    const char* u[] = {"B","KB","MB","GB","TB"};
    double v = (double)b;
    int i = 0;
    while (v >= 1024.0 && i < 4) { v /= 1024.0; i++; }
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.2f %s", v, u[i]);
    return buf;
}

std::string fmt_speed(double bps) {
    return fmt_bytes((int64_t)bps) + "/s";
}

// --- Cover cache ---------------------------------------------------------

struct Cover {
    SDL_Texture* tex = nullptr;
    int w = 0, h = 0;
    bool loading = false;
    bool failed = false;
};

struct CoverCache {
    std::mutex mtx;
    std::unordered_map<std::string, std::shared_ptr<Cover>> map;

    std::shared_ptr<Cover> get_or_load(const std::string& url, SDL_Renderer* r) {
        std::shared_ptr<Cover> c;
        {
            std::lock_guard<std::mutex> lk(mtx);
            auto it = map.find(url);
            if (it != map.end()) return it->second;
            c = std::make_shared<Cover>();
            c->loading = true;
            map[url] = c;
        }
        std::thread([url, c, r]{
            auto resp = fs_net::get(url);
            if (!resp.ok() || resp.body.empty()) { c->failed = true; c->loading = false; return; }
            SDL_RWops* rw = SDL_RWFromConstMem(resp.body.data(), (int)resp.body.size());
            SDL_Surface* surf = IMG_Load_RW(rw, 1);
            if (!surf) { c->failed = true; c->loading = false; return; }
            // Texture creation must happen on main thread; stash the surface instead.
            // Simplest: use SDL_CreateTextureFromSurface here (SDL2 is tolerant with software renderer).
            c->tex = SDL_CreateTextureFromSurface(r, surf);
            c->w = surf->w; c->h = surf->h;
            SDL_FreeSurface(surf);
            c->loading = false;
        }).detach();
        return c;
    }

    void clear() {
        std::lock_guard<std::mutex> lk(mtx);
        for (auto& kv : map) {
            if (kv.second->tex) SDL_DestroyTexture(kv.second->tex);
        }
        map.clear();
    }
};

// --- Category fetch ------------------------------------------------------

struct Catalog {
    std::mutex mtx;
    std::vector<fs_scrape::GameEntry> entries;
    int page = 1;
    int last_page = 1;
    bool loading = false;
    bool loaded_once = false;

    void load_page(int p) {
        {
            std::lock_guard<std::mutex> lk(mtx);
            if (loading) return;
            loading = true;
        }
        std::thread([this, p]{
            auto cp = fs_scrape::fetch_category(p);
            std::lock_guard<std::mutex> lk(mtx);
            entries = cp.entries;
            page = cp.page;
            last_page = cp.last_page;
            loading = false;
            loaded_once = true;
        }).detach();
    }
};

struct DetailCache {
    std::mutex mtx;
    std::unordered_map<std::string, std::shared_ptr<fs_scrape::GameDetail>> map;
    std::unordered_map<std::string, bool> loading;

    std::shared_ptr<fs_scrape::GameDetail> get(const std::string& url) {
        std::lock_guard<std::mutex> lk(mtx);
        auto it = map.find(url);
        return it == map.end() ? nullptr : it->second;
    }

    void load(const fs_scrape::GameEntry& e) {
        {
            std::lock_guard<std::mutex> lk(mtx);
            if (map.count(e.page_url)) return;
            if (loading[e.page_url]) return;
            loading[e.page_url] = true;
        }
        std::thread([this, e]{
            auto d = std::make_shared<fs_scrape::GameDetail>(fs_scrape::fetch_detail(e));
            std::lock_guard<std::mutex> lk(mtx);
            map[e.page_url] = d;
            loading[e.page_url] = false;
        }).detach();
    }
};

// --- Context -------------------------------------------------------------

struct UiCtx {
    SDL_Renderer* renderer = nullptr;
    CoverCache covers;
    Catalog    catalog;
    DetailCache details;
    char search_filter[256] = {0};
    fs_scrape::GameEntry selected;
    bool show_detail = false;
    int  active_tab  = 0;
    char tmp_cookie[1024] = {0};
    char tmp_ua[1024] = {0};
    char tmp_install[512] = {0};
    bool settings_loaded_fields = false;
};

// --- Tabs ----------------------------------------------------------------

void draw_grid(UiCtx* u) {
    auto& s = fs_set::current();
    int cols = std::max(1, s.ui_grid_cols);

    if (ImGui::Button("Refresh")) u->catalog.load_page(1);
    ImGui::SameLine();
    ImGui::InputTextWithHint("##search", "search title...",
                             u->search_filter, sizeof(u->search_filter));
    ImGui::SameLine();
    if (ImGui::Button("<")) {
        if (u->catalog.page > 1) u->catalog.load_page(u->catalog.page - 1);
    }
    ImGui::SameLine();
    ImGui::Text("page %d / %d", u->catalog.page, u->catalog.last_page);
    ImGui::SameLine();
    if (ImGui::Button(">")) {
        if (u->catalog.page < u->catalog.last_page)
            u->catalog.load_page(u->catalog.page + 1);
    }

    {
        std::lock_guard<std::mutex> lk(u->catalog.mtx);
        if (u->catalog.loading) ImGui::TextUnformatted("loading...");
        if (!u->catalog.loaded_once && !u->catalog.loading) {
            // Kick off first load.
            u->catalog.load_page(1);
        }
    }

    ImGui::Separator();

    ImGui::BeginChild("grid", ImVec2(0, 0), false, ImGuiWindowFlags_HorizontalScrollbar);
    std::vector<fs_scrape::GameEntry> entries_copy;
    {
        std::lock_guard<std::mutex> lk(u->catalog.mtx);
        entries_copy = u->catalog.entries;
    }

    float avail = ImGui::GetContentRegionAvail().x;
    float tile_w = (avail - (cols - 1) * 8.0f) / (float)cols;
    float tile_h = tile_w * 1.4f;

    int i = 0;
    for (auto& e : entries_copy) {
        if (u->search_filter[0] != '\0') {
            std::string t = e.title;
            std::transform(t.begin(), t.end(), t.begin(), ::tolower);
            std::string f = u->search_filter;
            std::transform(f.begin(), f.end(), f.begin(), ::tolower);
            if (t.find(f) == std::string::npos) continue;
        }
        if (i % cols != 0) ImGui::SameLine();

        ImGui::PushID(e.page_url.c_str());
        ImGui::BeginGroup();

        auto cov = e.cover_url.empty()
                   ? nullptr
                   : u->covers.get_or_load(e.cover_url, u->renderer).get();
        if (cov && cov->tex) {
            ImGui::Image((ImTextureID)cov->tex, ImVec2(tile_w, tile_h));
        } else {
            ImGui::Button("##empty", ImVec2(tile_w, tile_h));
        }
        ImGui::TextWrapped("%s", e.title.c_str());
        if (ImGui::Button("Open", ImVec2(tile_w, 0))) {
            u->selected = e;
            u->show_detail = true;
            u->details.load(e);
        }
        ImGui::EndGroup();
        ImGui::PopID();
        i++;
    }
    ImGui::EndChild();
}

void draw_detail_popup(UiCtx* u) {
    if (!u->show_detail) return;
    ImGui::OpenPopup("Game Detail");
    ImGui::SetNextWindowSize(ImVec2(1200, 800), ImGuiCond_Appearing);
    if (ImGui::BeginPopupModal("Game Detail", &u->show_detail,
                               ImGuiWindowFlags_NoCollapse)) {
        ImGui::Text("%s", u->selected.title.c_str());
        ImGui::Separator();

        auto d = u->details.get(u->selected.page_url);
        if (!d) {
            ImGui::TextUnformatted("loading details...");
            if (ImGui::Button("Close")) u->show_detail = false;
            ImGui::EndPopup();
            return;
        }

        ImGui::BeginChild("desc", ImVec2(0, 160), true);
        ImGui::TextWrapped("%s", d->description.c_str());
        ImGui::EndChild();

        ImGui::Separator();
        ImGui::Text("Downloads (%zu)", d->links.size());

        if (ImGui::BeginTable("dls", 6,
                              ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders)) {
            ImGui::TableSetupColumn("Fmt",    ImGuiTableColumnFlags_WidthFixed, 70);
            ImGui::TableSetupColumn("Host",   ImGuiTableColumnFlags_WidthFixed, 110);
            ImGui::TableSetupColumn("Region", ImGuiTableColumnFlags_WidthFixed, 70);
            ImGui::TableSetupColumn("Ver",    ImGuiTableColumnFlags_WidthFixed, 80);
            ImGui::TableSetupColumn("Label");
            ImGui::TableSetupColumn("",       ImGuiTableColumnFlags_WidthFixed, 180);
            ImGui::TableHeadersRow();

            for (auto& dl : d->links) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0); ImGui::TextUnformatted(fs_scrape::fmt_label(dl.fmt));
                ImGui::TableSetColumnIndex(1); ImGui::TextUnformatted(dl.host_label.c_str());
                ImGui::TableSetColumnIndex(2); ImGui::TextUnformatted(dl.region.c_str());
                ImGui::TableSetColumnIndex(3); ImGui::TextUnformatted(dl.version.c_str());
                ImGui::TableSetColumnIndex(4); ImGui::TextWrapped("%s", dl.label.c_str());
                ImGui::TableSetColumnIndex(5);
                ImGui::PushID(&dl);
                if (ImGui::Button("Download to etaHEN")) {
                    fs_dl::Manager::instance().enqueue(
                        u->selected.title,
                        u->selected.cover_url,
                        dl,
                        fs_set::current().install_path);
                    u->active_tab = 1;
                }
                ImGui::SameLine();
                if (ImGui::Button("USB")) {
                    fs_dl::Manager::instance().enqueue(
                        u->selected.title,
                        u->selected.cover_url,
                        dl,
                        fs_set::current().install_path_usb);
                    u->active_tab = 1;
                }
                ImGui::PopID();
            }
            ImGui::EndTable();
        }

        ImGui::Separator();
        if (ImGui::Button("Close")) u->show_detail = false;
        ImGui::EndPopup();
    }
}

void draw_downloads(UiCtx* /*u*/) {
    auto& mgr = fs_dl::Manager::instance();
    auto active = mgr.active_snapshot();
    auto done   = mgr.completed_snapshot();

    ImGui::Text("Active (%zu)", active.size());
    if (ImGui::BeginTable("active", 7,
                          ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders)) {
        ImGui::TableSetupColumn("#",       ImGuiTableColumnFlags_WidthFixed, 40);
        ImGui::TableSetupColumn("Title");
        ImGui::TableSetupColumn("Fmt",     ImGuiTableColumnFlags_WidthFixed, 70);
        ImGui::TableSetupColumn("Host",    ImGuiTableColumnFlags_WidthFixed, 110);
        ImGui::TableSetupColumn("Stage",   ImGuiTableColumnFlags_WidthFixed, 180);
        ImGui::TableSetupColumn("Progress",ImGuiTableColumnFlags_WidthFixed, 260);
        ImGui::TableSetupColumn("",        ImGuiTableColumnFlags_WidthFixed, 100);
        ImGui::TableHeadersRow();

        for (auto& j : active) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0); ImGui::Text("%llu", (unsigned long long)j->id);
            ImGui::TableSetColumnIndex(1); ImGui::TextWrapped("%s", j->game_title.c_str());
            ImGui::TableSetColumnIndex(2); ImGui::TextUnformatted(fs_scrape::fmt_label(j->link.fmt));
            ImGui::TableSetColumnIndex(3); ImGui::TextUnformatted(j->link.host_label.c_str());
            ImGui::TableSetColumnIndex(4);
            ImGui::Text("%s %s", fs_dl::status_label(j->status), j->stage_detail.c_str());
            ImGui::TableSetColumnIndex(5);
            ImGui::ProgressBar((float)j->progress, ImVec2(240, 0));
            ImGui::Text("%s / %s  (%s)",
                        fmt_bytes(j->done_bytes).c_str(),
                        fmt_bytes(j->total_bytes).c_str(),
                        fmt_speed(j->speed_bps).c_str());
            ImGui::TableSetColumnIndex(6);
            ImGui::PushID((int)j->id);
            if (ImGui::Button("Cancel")) fs_dl::Manager::instance().cancel(j->id);
            ImGui::PopID();
        }
        ImGui::EndTable();
    }

    ImGui::Separator();
    ImGui::Text("Completed / Failed (%zu)", done.size());
    if (ImGui::BeginTable("done", 6,
                          ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders)) {
        ImGui::TableSetupColumn("#",      ImGuiTableColumnFlags_WidthFixed, 40);
        ImGui::TableSetupColumn("Title");
        ImGui::TableSetupColumn("Fmt",    ImGuiTableColumnFlags_WidthFixed, 70);
        ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthFixed, 110);
        ImGui::TableSetupColumn("Path / Error");
        ImGui::TableSetupColumn("",       ImGuiTableColumnFlags_WidthFixed, 160);
        ImGui::TableHeadersRow();

        for (auto& j : done) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0); ImGui::Text("%llu", (unsigned long long)j->id);
            ImGui::TableSetColumnIndex(1); ImGui::TextWrapped("%s", j->game_title.c_str());
            ImGui::TableSetColumnIndex(2); ImGui::TextUnformatted(fs_scrape::fmt_label(j->link.fmt));
            ImGui::TableSetColumnIndex(3); ImGui::TextUnformatted(fs_dl::status_label(j->status));
            ImGui::TableSetColumnIndex(4);
            if (!j->error_msg.empty()) ImGui::TextWrapped("ERR: %s", j->error_msg.c_str());
            else                       ImGui::TextWrapped("%s", j->final_install_path.c_str());
            ImGui::TableSetColumnIndex(5);
            ImGui::PushID((int)(1000000 + j->id));
            if (j->status == fs_dl::Status::FAILED || j->status == fs_dl::Status::CANCELLED) {
                if (ImGui::Button("Retry")) fs_dl::Manager::instance().retry(j->id);
                ImGui::SameLine();
            }
            if (ImGui::Button("Remove")) fs_dl::Manager::instance().remove(j->id);
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
}

void draw_settings(UiCtx* u) {
    auto& s = fs_set::current();

    if (!u->settings_loaded_fields) {
        std::snprintf(u->tmp_install, sizeof(u->tmp_install), "%s", s.install_path.c_str());
        std::snprintf(u->tmp_cookie,  sizeof(u->tmp_cookie),  "%s", s.cookie_header.c_str());
        std::snprintf(u->tmp_ua,      sizeof(u->tmp_ua),      "%s", s.ua_override.c_str());
        u->settings_loaded_fields = true;
    }

    ImGui::TextUnformatted("Install target");
    ImGui::InputText("##install_path", u->tmp_install, sizeof(u->tmp_install));

    if (ImGui::Button("etaHEN games"))
        std::snprintf(u->tmp_install, sizeof(u->tmp_install), "%s", fs_cfg::ETAHEN_FPKG_PATH);
    ImGui::SameLine();
    if (ImGui::Button("etaHEN FPFSC"))
        std::snprintf(u->tmp_install, sizeof(u->tmp_install), "%s", fs_cfg::ETAHEN_FPFSC_PATH);
    ImGui::SameLine();
    if (ImGui::Button("USB0"))
        std::snprintf(u->tmp_install, sizeof(u->tmp_install), "%s", fs_cfg::USB_INSTALL_PATH);

    ImGui::Separator();

    ImGui::SliderInt("Parallel downloads", &s.parallel_downloads, 1, 8);
    ImGui::SliderInt("Chunks per file",    &s.chunks_per_file,    1, 32);
    ImGui::SliderInt("Grid columns",       &s.ui_grid_cols,       3, 10);

    int tmo = (int)s.connect_timeout_sec;  ImGui::SliderInt("Connect timeout (s)", &tmo, 5, 120); s.connect_timeout_sec = tmo;
    int lsl = (int)s.low_speed_limit_bps;  ImGui::SliderInt("Low-speed cutoff B/s", &lsl, 0, 65536); s.low_speed_limit_bps = lsl;
    int lst = (int)s.low_speed_time_sec;   ImGui::SliderInt("Low-speed window (s)", &lst, 10, 300); s.low_speed_time_sec = lst;

    ImGui::Checkbox("Auto-extract archives",      &s.auto_extract);
    ImGui::Checkbox("Delete archive after install",&s.delete_archive_after);
    ImGui::Checkbox("Notify etaHEN on install",   &s.notify_etahen);

    ImGui::Separator();
    ImGui::TextUnformatted("Cookie header (for sites behind Cloudflare / premium)");
    ImGui::InputTextMultiline("##cookie", u->tmp_cookie, sizeof(u->tmp_cookie),
                              ImVec2(-1, 60));
    ImGui::TextUnformatted("User-Agent override (leave blank for default PS5)");
    ImGui::InputText("##ua", u->tmp_ua, sizeof(u->tmp_ua));

    ImGui::Separator();
    if (ImGui::Button("Save settings")) {
        s.install_path  = u->tmp_install;
        s.cookie_header = u->tmp_cookie;
        s.ua_override   = u->tmp_ua;
        fs_set::save();
    }
    ImGui::SameLine();
    if (ImGui::Button("Reset to defaults")) {
        s = fs_set::Settings::defaults();
        u->settings_loaded_fields = false;
        fs_set::save();
    }
}

} // anon

UiCtx* init(SDL_Window* window, SDL_Renderer* renderer) {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad; // DualSense navigation
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    ImGui::StyleColorsDark();
    ImGuiStyle& st = ImGui::GetStyle();
    st.WindowRounding = 6.0f;
    st.FrameRounding  = 4.0f;
    st.GrabRounding   = 4.0f;
    st.ScaleAllSizes(1.2f);

    ImGui_ImplSDL2_InitForSDLRenderer(window, renderer);
    ImGui_ImplSDLRenderer2_Init(renderer);

    IMG_Init(IMG_INIT_PNG | IMG_INIT_JPG);

    auto* u = new UiCtx();
    u->renderer = renderer;
    return u;
}

void shutdown(UiCtx* u) {
    if (!u) return;
    u->covers.clear();
    ImGui_ImplSDLRenderer2_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();
    IMG_Quit();
    delete u;
}

bool frame(UiCtx* u, SDL_Window* window, SDL_Renderer* renderer) {
    bool keep = true;

    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
        ImGui_ImplSDL2_ProcessEvent(&ev);
        if (ev.type == SDL_QUIT) keep = false;
        if (ev.type == SDL_WINDOWEVENT &&
            ev.window.event == SDL_WINDOWEVENT_CLOSE) keep = false;
        if (ev.type == SDL_CONTROLLERBUTTONDOWN &&
            ev.cbutton.button == SDL_CONTROLLER_BUTTON_GUIDE) keep = false;
    }

    ImGui_ImplSDLRenderer2_NewFrame();
    ImGui_ImplSDL2_NewFrame();
    ImGui::NewFrame();

    // Full-screen docking root.
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::Begin("##root", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                 ImGuiWindowFlags_NoBringToFrontOnFocus);

    ImGui::Text(APP_NAME "  v" APP_VERSION "  —  etaHEN target: %s",
                fs_set::current().install_path.c_str());
    ImGui::Separator();

    if (ImGui::BeginTabBar("tabs")) {
        if (ImGui::BeginTabItem("Store", nullptr,
                u->active_tab == 0 ? ImGuiTabItemFlags_SetSelected : 0)) {
            u->active_tab = 0;
            draw_grid(u);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Downloads", nullptr,
                u->active_tab == 1 ? ImGuiTabItemFlags_SetSelected : 0)) {
            u->active_tab = 1;
            draw_downloads(u);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Settings")) {
            u->active_tab = 2;
            draw_settings(u);
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }

    draw_detail_popup(u);

    ImGui::End();

    ImGui::Render();
    SDL_SetRenderDrawColor(renderer, 20, 22, 26, 255);
    SDL_RenderClear(renderer);
    ImGui_ImplSDLRenderer2_RenderDrawData(ImGui::GetDrawData(), renderer);
    SDL_RenderPresent(renderer);

    return keep;
}

} // namespace fs_ui
