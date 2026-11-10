#include "process_runner.h"
#include <SDL3/SDL.h>
#include <SDL3/SDL_dialog.h>
#include <SDL3/SDL_opengl.h>
#include <imgui.h>
#include <imgui_impl_opengl3.h>
#include <imgui_impl_sdl3.h>
#include <nlohmann/json.hpp>
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <stb_image_write.h>
#include <thread>
using json = nlohmann::json;
namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;
static std::string utf(const fs::path &p) { return p.u8string(); }
static ImU32 color(int r, int g, int b, int a = 255) {
  return IM_COL32(r, g, b, a);
}
static ImVec4 rgba(int r, int g, int b) {
  return ImVec4(r / 255.f, g / 255.f, b / 255.f, 1);
}
static std::string duration(double v) {
  int s = (int)v;
  char b[32];
  std::snprintf(b, sizeof(b), "%02d:%02d:%02d", s / 3600, s / 60 % 60, s % 60);
  return b;
}
struct Host {
  fs::path base, data, cache, python, bridge;
  json settings = json::object(), games = json::array();
  std::vector<std::string> files;
  std::map<int, std::string> art;
  std::map<std::string, GLuint> textures;
  std::deque<json> events;
  std::deque<std::string> logs;
  std::mutex mutex;
  std::thread worker, art_worker;
  workbench::ProcessControl control, art_control;
  bool busy = false, building = false, advanced_preview = false,
       multiple = false, settings_page = false, show_logs = false;
  int selected = 0, current_index = -1;
  std::string status = "Choose your game files", error;
  double total = 0, current = 0;
  Clock::time_point last = Clock::now();
  void request(std::string action) {
    if (busy)
      return;
    if (worker.joinable())
      worker.join();
    json request = {{"action", action},
                    {"data_root", utf(data)},
                    {"cache_root", utf(cache)},
                    {"files", files},
                    {"multiple", multiple}};
    if (action == "save")
      request["settings"] = settings;
    fs::create_directories(data);
    fs::path input = data / "native-request.json";
    std::ofstream(input) << request.dump(2);
    busy = true;
    building = action == "build";
    control.cancel_requested = false;
    if (action == "selection") {
      games = json::array();
      total = 0;
      current_index = -1;
      error.clear();
      status = "Reading selection";
    }
    if (building) {
      fs::remove(data / "cancel-build");
      total = 0;
      current = 0;
      last = Clock::now();
      status = "Preparing build";
      for (auto &g : games) {
        g["state"] = "queued";
        g["elapsed_seconds"] = 0;
      }
    }
    worker = std::thread([this, input, action] {
      try {
        std::string pending;
        auto result = workbench::run_process(
            {utf(python), "-u", "-B", utf(bridge), utf(input)}, base, &control,
            [&](std::string_view text) {
              pending.append(text);
              size_t end;
              while ((end = pending.find('\n')) != std::string::npos) {
                std::string line = pending.substr(0, end);
                pending.erase(0, end + 1);
                json event = json::parse(line, nullptr, false);
                if (event.is_discarded())
                  event = {{"event", "log"}, {"text", line}};
                std::lock_guard<std::mutex> lock(mutex);
                events.push_back(event);
              }
            });
        if (action == "selection") {
          if (art_worker.joinable()) {
            art_control.request_cancel();
            art_worker.join();
          }
          art_control.cancel_requested = false;
          json art_request;
          std::ifstream(input) >> art_request;
          art_request["action"] = "artwork";
          auto art_input = data / "art-request.json";
          std::ofstream(art_input) << art_request.dump();
          art_worker = std::thread([this, art_input] {
            auto art_result = workbench::run_process(
                {utf(python), "-u", "-B", utf(bridge), utf(art_input)}, base,
                &art_control);
            std::istringstream lines(art_result.output);
            std::string line;
            while (std::getline(lines, line)) {
              auto event = json::parse(line, nullptr, false);
              if (!event.is_discarded()) {
                std::lock_guard<std::mutex> lock(mutex);
                events.push_back(event);
              }
            }
          });
        }
        std::lock_guard<std::mutex> lock(mutex);
        events.push_back({{"event", "process_end"},
                          {"action", action},
                          {"code", result.exit_code},
                          {"cancelled", result.canceled}});
      } catch (const std::exception &error) {
        std::lock_guard<std::mutex> lock(mutex);
        events.push_back({{"event", "error"}, {"text", error.what()}});
        events.push_back(
            {{"event", "process_end"}, {"action", action}, {"code", 1}});
      }
    });
  }
  void poll() {
    auto now = Clock::now();
    double delta = std::chrono::duration<double>(now - last).count();
    last = now;
    if (building) {
      total += delta;
      for (auto &g : games)
        if (g.value("state", "") == "active")
          g["elapsed_seconds"] = g.value("elapsed_seconds", 0.0) + delta;
    }
    std::deque<json> local;
    {
      std::lock_guard<std::mutex> lock(mutex);
      local.swap(events);
    }
    for (auto &e : local) {
      auto kind = e.value("event", "");
      if (kind == "settings")
        settings = e["settings"];
      else if (kind == "selection") {
        games = e["games"];
        art.clear();
        selected = 0;
        error.clear();
        status = std::to_string(games.size()) + " game" +
                 (games.size() == 1 ? "" : "s") + " selected";
      } else if (kind == "log") {
        auto line = e.value("text", "");
        logs.push_back(line);
        if (logs.size() > 5000)
          logs.pop_front();
        if (line.rfind("WORKBENCH_EVENT ", 0) == 0) {
          auto j = json::parse(line.substr(16), nullptr, false);
          if (j.is_object())
            status = j.value("label", j.value("stage", status));
        }
      } else if (kind == "artwork") {
        art[e.value("index", 0)] = e.value("path", "");
      } else if (kind == "error") {
        error = e.value("text", "");
        status = "Needs attention";
      } else if (kind == "batch_started" || kind == "game_started" ||
                 kind == "game_finished" || kind == "batch_finished") {
        games = e["games"];
        total = e.value("total_seconds", total);
        if (kind == "game_started") {
          selected = e.value("index", 0);
          current_index = selected;
          status = "Building " + games[selected].value("name", "");
        }
        if (kind == "batch_finished") {
          building = false;
          status = e.value("state", "") == "completed" ? "Build complete"
                   : e.value("state", "") == "cancelled"
                       ? "Build cancelled"
                       : "Batch finished with errors";
        }
      } else if (kind == "process_end") {
        busy = false;
        if (building) {
          building = false;
          status = e.value("cancelled", false) ? "Build cancelled"
                   : e.value("code", 0) == 0   ? "Build complete"
                                               : "Build failed";
        }
      }
    }
  }
};
struct Picker {
  Host *host;
  std::string field;
};
static void SDLCALL picked(void *user, const char *const *paths, int) {
  std::unique_ptr<Picker> p((Picker *)user);
  json e = {{"event", "picked"}, {"field", p->field}, {"paths", json::array()}};
  if (paths)
    for (int i = 0; paths[i]; i++)
      e["paths"].push_back(paths[i]);
  std::lock_guard<std::mutex> lock(p->host->mutex);
  p->host->events.push_back(e);
}
static void picker(Host &h, SDL_Window *w, std::string field,
                   bool folder = false) {
  auto *data = new Picker{&h, field};
  static SDL_DialogFileFilter game[] = {
      {"PlayStation games and SBI", "cue;iso;chd;m3u;sbi"}};
  static SDL_DialogFileFilter bios[] = {{"BIOS files", "bin;rom"}};
  static SDL_DialogFileFilter pins[] = {{"Pin record", "json"}};
  if (folder)
    SDL_ShowOpenFolderDialog(picked, data, w, nullptr, false);
  else
    SDL_ShowOpenFileDialog(picked, data, w,
                           field == "files"        ? game
                           : field == "bios_files" ? bios
                                                   : pins,
                           1, nullptr,
                           field == "files" || field == "bios_files");
}
static GLuint texture(const fs::path &path) {
  int w, h, n;
  auto *p = stbi_load(utf(path).c_str(), &w, &h, &n, 4);
  if (!p)
    return 0;
  GLuint t;
  glGenTextures(1, &t);
  glBindTexture(GL_TEXTURE_2D, t);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE,
               p);
  stbi_image_free(p);
  return t;
}
static bool button(const char *label, ImVec2 size, ImVec4 fill) {
  ImGui::PushStyleColor(ImGuiCol_Button, fill);
  bool v = ImGui::Button(label, size);
  ImGui::PopStyleColor();
  return v;
}
static void caption(const char *text) {
  ImGui::PushStyleColor(ImGuiCol_Text, rgba(133, 151, 178));
  ImGui::TextUnformatted(text);
  ImGui::PopStyleColor();
}
static void field(Host &h, const char *label, const char *key) {
  char buffer[2048];
  std::snprintf(buffer, sizeof(buffer), "%s",
                h.settings.value(key, "").c_str());
  ImGui::TextUnformatted(label);
  ImGui::SetNextItemWidth(-1);
  if (ImGui::InputText((std::string("##") + key).c_str(), buffer,
                       sizeof(buffer)))
    h.settings[key] = buffer;
}
static void style(bool light = false) {
  auto &s = ImGui::GetStyle();
  s.WindowRounding = 0;
  s.ChildRounding = 16;
  s.FrameRounding = 9;
  s.GrabRounding = 9;
  s.PopupRounding = 12;
  s.WindowPadding = {28, 24};
  s.FramePadding = {16, 11};
  s.ItemSpacing = {12, 12};
  s.ScrollbarSize = 8;
  s.FrameBorderSize = 1;
  s.Colors[ImGuiCol_WindowBg] = light ? rgba(235, 240, 248) : rgba(10, 16, 27);
  s.Colors[ImGuiCol_ChildBg] = light ? rgba(249, 251, 255) : rgba(17, 26, 42);
  s.Colors[ImGuiCol_Text] = light ? rgba(25, 38, 57) : rgba(228, 235, 246);
  s.Colors[ImGuiCol_TextDisabled] = rgba(127, 145, 173);
  s.Colors[ImGuiCol_Border] = light ? rgba(205, 215, 230) : rgba(37, 52, 74);
  s.Colors[ImGuiCol_FrameBg] = light ? rgba(234, 239, 248) : rgba(12, 20, 34);
  s.Colors[ImGuiCol_Button] = rgba(30, 44, 65);
  s.Colors[ImGuiCol_ButtonHovered] = rgba(43, 65, 94);
  s.Colors[ImGuiCol_ButtonActive] = rgba(42, 82, 125);
  s.Colors[ImGuiCol_Header] = rgba(35, 64, 99);
  s.Colors[ImGuiCol_HeaderHovered] = rgba(40, 65, 94);
  s.Colors[ImGuiCol_CheckMark] = rgba(100, 177, 255);
}
int main(int argc, char **argv) {
  Host h;
  h.base = fs::absolute(fs::u8path(argv[0])).parent_path();
  h.data = h.base / "public-studio-data";
  h.bridge = h.base / "native_bridge.py";
  h.python = h.base / "runtime/python.exe";
  fs::path screenshot, fixture;
  for (int i = 1; i < argc; i++) {
    std::string a = argv[i];
    if (a == "--data-root" && i + 1 < argc)
      h.data = fs::u8path(argv[++i]);
    else if (a == "--cache-root" && i + 1 < argc)
      h.cache = fs::u8path(argv[++i]);
    else if (a == "--preview" && i + 1 < argc)
      screenshot = fs::u8path(argv[++i]);
    else if (a == "--preview-state" && i + 1 < argc)
      fixture = fs::u8path(argv[++i]);
    else if (a == "--python" && i + 1 < argc)
      h.python = fs::u8path(argv[++i]);
    else if (a == "--bridge" && i + 1 < argc)
      h.bridge = fs::u8path(argv[++i]);
  }
  if (h.cache.empty())
    h.cache = h.data / "cache";
  if (!SDL_Init(SDL_INIT_VIDEO))
    return 2;
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 2);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
  auto *w =
      SDL_CreateWindow("Workbench Studio | Retro Porting Toolkit", 1180, 790,
                       SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE |
                           (screenshot.empty() ? 0 : SDL_WINDOW_HIDDEN));
  if (!w)
    return 3;
  SDL_SetWindowMinimumSize(w, 960, 720);
  auto gl = SDL_GL_CreateContext(w);
  if (!gl)
    return 4;
  SDL_GL_MakeCurrent(w, gl);
  SDL_GL_SetSwapInterval(1);
  ImGui::CreateContext();
  ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
  ImGui::GetIO().IniFilename = nullptr;
  auto font = h.base / "assets/LatoLatin-Regular.ttf";
  if (fs::exists(font))
    ImGui::GetIO().Fonts->AddFontFromFileTTF(utf(font).c_str(), 18);
  auto bold_path = h.base / "assets/LatoLatin-Bold.ttf";
  ImFont *heading =
      fs::exists(bold_path)
          ? ImGui::GetIO().Fonts->AddFontFromFileTTF(utf(bold_path).c_str(), 24)
          : nullptr;
  ImGui_ImplSDL3_InitForOpenGL(w, gl);
  ImGui_ImplOpenGL3_Init("#version 150");
  GLuint logo = texture(h.base / "assets/retro-porting-toolkit.png");
  h.request("settings");
  bool done = false;
  int frames = 0;
  while (!done) {
    SDL_Event e;
    bool dropped = false;
    while (SDL_PollEvent(&e)) {
      ImGui_ImplSDL3_ProcessEvent(&e);
      if (e.type == SDL_EVENT_QUIT)
        done = true;
      if (e.type == SDL_EVENT_DROP_BEGIN && !h.busy)
        h.files.clear();
      if (e.type == SDL_EVENT_DROP_FILE && !h.busy) {
        h.files.push_back(e.drop.data);
        dropped = true;
      }
    }
    if (dropped)
      h.request("selection");
    // File dialog callbacks enter through the UI thread, keeping the active
    // batch immutable.
    {
      std::lock_guard<std::mutex> lock(h.mutex);
      for (auto it = h.events.begin(); it != h.events.end();) {
        if (it->value("event", "") != "picked") {
          ++it;
          continue;
        }
        auto field = it->value("field", "");
        if (field == "files")
          h.files = it->at("paths").get<std::vector<std::string>>();
        else if (field == "bios_files")
          h.settings[field] = (*it)["paths"];
        else if (!(*it)["paths"].empty())
          h.settings[field] = (*it)["paths"][0];
        it = h.events.erase(it);
        if (field == "files")
          h.status = "Selection changed";
      }
    }
    h.poll();
    if (!fixture.empty() && !h.busy) {
      json f;
      std::ifstream(fixture) >> f;
      h.games = f.value("games", json::array());
      if (f.contains("artwork"))
        for (auto it = f["artwork"].begin(); it != f["artwork"].end(); ++it)
          h.art[std::stoi(it.key())] = it.value();
      h.settings_page = f.value("settings_page", false);
      h.advanced_preview = f.value("advanced", false);
      h.total = f.value("total_seconds", 0.0);
      h.current_index = f.value("current_index", -1);
      h.status = f.value("status", h.status);
      fixture.clear();
    }
    if (h.status == "Selection changed" && !h.busy)
      h.request("selection");
    style(h.settings.value("theme", "dark") == "light");
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();
    auto size = ImGui::GetIO().DisplaySize;
    ImGui::SetNextWindowPos({0, 0});
    ImGui::SetNextWindowSize(size);
    ImGui::Begin("Studio", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoSavedSettings);
    if (logo) {
      ImGui::Image((ImTextureID)(intptr_t)logo, {44, 44});
      ImGui::SameLine();
    }
    ImGui::BeginGroup();
    if (heading)
      ImGui::PushFont(heading);
    ImGui::TextUnformatted("Workbench Studio");
    if (heading)
      ImGui::PopFont();
    caption("RETRO PORTING TOOLKIT  /  PS1");
    ImGui::EndGroup();
    auto *header_draw = ImGui::GetWindowDrawList();
    header_draw->AddRectFilled({size.x - 268, 28}, {size.x - 180, 72},
                               color(24, 46, 73), 10);
    header_draw->AddText({size.x - 239, 40}, color(143, 190, 240), "PS1");
    ImGui::SameLine(size.x - 158);
    if (ImGui::Button(h.settings_page ? "Back to games" : "Settings",
                      {130, 44}) &&
        !h.busy) {
      if (h.settings_page)
        h.request("save");
      h.settings_page = !h.settings_page;
    }
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    if (h.settings_page) {
      ImGui::BeginChild("settings", {0, -52}, true);
      ImGui::TextUnformatted("Your workspace");
      caption("Set this up once. Then choose your games and build.");
      ImGui::Spacing();
      ImGui::TextUnformatted("BIOS files");
      if (h.settings.contains("bios_files"))
        for (auto &p : h.settings["bios_files"]) {
          auto path = p.get<std::string>();
          caption(fs::u8path(path).filename().u8string().c_str());
          auto regions = h.settings.value("bios_regions", json::object());
          std::string region = regions.value(path, "Auto");
          const char *names[] = {"Auto", "USA", "Europe", "Japan"};
          int selected = 0;
          for (int i = 0; i < 4; i++)
            if (region == names[i])
              selected = i;
          ImGui::PushID(path.c_str());
          ImGui::SetNextItemWidth(150);
          if (ImGui::Combo("Region", &selected, names, 4))
            h.settings["bios_regions"][path] = names[selected];
          ImGui::PopID();
        }
      if (ImGui::Button("Choose BIOS files"))
        picker(h, w, "bios_files");
      field(h, "Default output folder", "output_folder");
      if (ImGui::Button("Choose output folder"))
        picker(h, w, "output_folder", true);
      if (h.advanced_preview)
        ImGui::SetNextItemOpen(true, ImGuiCond_Once);
      if (ImGui::CollapsingHeader("Advanced")) {
        field(h, "Core PIN.json", "core_pin");
        if (ImGui::Button("Choose core pin"))
          picker(h, w, "core_pin");
        field(h, "UI.json", "ui_pin");
        if (ImGui::Button("Choose UI pin"))
          picker(h, w, "ui_pin");
        field(h, "Online core version", "core_ref");
        field(h, "Online UI version", "ui_ref");
        field(h, "Custom core source", "custom_core");
        field(h, "Custom UI source", "custom_ui");
        int optimization =
            h.settings.value("compiler_optimization", "optimized") == "fast"
                ? 0
                : 1;
        ImGui::TextUnformatted("Compiler optimization");
        if (ImGui::Combo("##compiler", &optimization,
                         "Fast build\0Optimized runtime\0"))
          h.settings["compiler_optimization"] =
              optimization == 0 ? "fast" : "optimized";
        caption("Fast build uses -O1. Optimized runtime uses -O3.");
        bool lto = h.settings.value("lto", true);
        if (ImGui::Checkbox("Link-time optimization (LTO)", &lto))
          h.settings["lto"] = lto;
        caption("LTO can improve runtime speed, but takes longer to link.");
        for (auto pair : {std::pair{"Portable", "portable"},
                          {"Skip BIOS", "skip_bios"},
                          {"PGO", "pgo"},
                          {"Diagnostics", "diagnostics"},
                          {"Startup checks", "startup_checks"}}) {
          bool value = h.settings.value(pair.second, false);
          if (ImGui::Checkbox(pair.first, &value))
            h.settings[pair.second] = value;
        }
      }
      bool light = h.settings.value("theme", "dark") == "light";
      if (ImGui::Checkbox("Light appearance", &light))
        h.settings["theme"] = light ? "light" : "dark";
      if (h.advanced_preview)
        ImGui::SetScrollY(ImGui::GetScrollMaxY());
      ImGui::EndChild();
    } else {
      ImGui::BeginDisabled(h.busy);
      if (button("One Game", {132, 42},
                 h.multiple ? rgba(22, 33, 50) : rgba(33, 72, 114))) {
        h.multiple = false;
        h.request("selection");
      }
      ImGui::SameLine();
      if (button("Multiple Games", {160, 42},
                 h.multiple ? rgba(33, 72, 114) : rgba(22, 33, 50))) {
        h.multiple = true;
        h.request("selection");
      }
      ImGui::EndDisabled();
      ImGui::SameLine();
      if (ImGui::Button("?", {42, 42}))
        ImGui::OpenPopup("Choosing games");
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("How to choose single-disc and multi-disc games");
      if (ImGui::BeginPopup("Choosing games")) {
        ImGui::PushTextWrapPos(470);
        ImGui::TextUnformatted(
            "One game: choose its disc image, or all of its discs together. A "
            ".m3u playlist also works.\n\nMultiple games: choose one image for "
            "each single-disc game. For each multi-disc game, choose its .m3u "
            "playlist instead. Each playlist stays one game.\n\nChoose a .cue "
            "file instead of its .bin tracks. Add matching .sbi files in the "
            "same selection. Each game gets its own output folder.");
        ImGui::PopTextWrapPos();
        ImGui::EndPopup();
      }
      ImGui::Spacing();
      float side = 300, bodyHeight = size.y - 340;
      ImGui::BeginChild("game tray", {size.x - side - 80, bodyHeight}, true);
      ImGui::TextUnformatted("Your games");
      ImGui::SameLine(ImGui::GetWindowWidth() - 130);
      ImGui::BeginDisabled(h.busy);
      if (ImGui::SmallButton("Choose files"))
        picker(h, w, "files");
      ImGui::EndDisabled();
      caption("CUE, ISO, CHD, playlists and matching SBI files");
      ImGui::Spacing();
      if (h.games.empty()) {
        ImGui::Dummy({0, 50});
        auto *draw = ImGui::GetWindowDrawList();
        auto pos = ImGui::GetCursorScreenPos();
        float width = ImGui::GetContentRegionAvail().x;
        draw->AddRect(pos, {pos.x + width, pos.y + 190}, color(55, 79, 111), 18,
                      0, 1.5f);
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 45);
        ImGui::Indent(34);
        ImGui::TextUnformatted("Drop your game files here");
        caption("or choose them from your library");
        ImGui::Spacing();
        ImGui::BeginDisabled(h.busy);
        if (button("Choose game files", {210, 46}, rgba(36, 65, 101)))
          picker(h, w, "files");
        ImGui::EndDisabled();
        ImGui::Unindent(34);
      } else
        for (int i = 0; i < (int)h.games.size(); i++) {
          auto &g = h.games[i];
          std::string state = g.value("state", "queued");
          ImGui::PushID(i);
          ImVec4 tint = state == "completed" ? rgba(43, 104, 171)
                        : state == "failed"  ? rgba(126, 43, 52)
                        : state == "active"  ? rgba(37, 66, 103)
                                             : rgba(24, 36, 54);
          ImGui::PushStyleColor(ImGuiCol_Header, tint);
          ImGui::PushStyleColor(ImGuiCol_HeaderHovered, tint);
          bool selected = i == h.selected;
          if (ImGui::Selectable("##game", false, 0, {0, 78}))
            h.selected = i;
          auto pos = ImGui::GetItemRectMin();
          auto *d = ImGui::GetWindowDrawList();
          if (state == "completed" || state == "failed" || state == "active")
            d->AddRectFilled(pos, ImGui::GetItemRectMax(),
                             ImGui::ColorConvertFloat4ToU32(tint), 11);
          d->AddRectFilled({pos.x + 15, pos.y + 18}, {pos.x + 55, pos.y + 58},
                           color(48, 66, 91), 9);
          d->AddCircle({pos.x + 35, pos.y + 38}, 12, color(151, 181, 216), 32,
                       1.3);
          d->AddCircle({pos.x + 35, pos.y + 38}, 3, color(151, 181, 216));
          d->AddText({pos.x + 72, pos.y + 15}, color(233, 240, 250),
                     g.value("name", "").c_str());
          std::string detail = state == "completed" ? "Completed"
                               : state == "failed"  ? "Failed"
                               : state == "active"  ? "Building"
                                                    : "Queued";
          detail += "   " + duration(g.value("elapsed_seconds", 0.0));
          d->AddText({pos.x + 72, pos.y + 43}, color(149, 173, 203),
                     detail.c_str());
          ImGui::PopStyleColor(2);
          ImGui::PopID();
        }
      ImGui::EndChild();
      ImGui::SameLine();
      ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(12, 8));
      ImGui::BeginChild("build panel", {0, bodyHeight}, true);
      caption("PLAYSTATION");
      ImGui::TextUnformatted("Your next port");
      ImGui::Spacing();
      auto pos = ImGui::GetCursorScreenPos();
      float cw = ImGui::GetContentRegionAvail().x;
      auto *d = ImGui::GetWindowDrawList();
      d->AddRectFilled(pos, {pos.x + cw, pos.y + 135}, color(12, 20, 33), 12);
      if (h.art.count(h.selected) && !h.art[h.selected].empty()) {
        auto path = h.art[h.selected];
        if (!h.textures.count(path))
          h.textures[path] = texture(fs::u8path(path));
        if (h.textures[path])
          d->AddImage((ImTextureID)(intptr_t)h.textures[path],
                      {pos.x + (cw - 135) / 2, pos.y},
                      {pos.x + (cw + 135) / 2, pos.y + 135});
      } else {
        d->AddCircle({pos.x + cw / 2, pos.y + 65}, 52, color(44, 62, 86), 80,
                     2);
        d->AddCircle({pos.x + cw / 2, pos.y + 65}, 18, color(62, 84, 112), 48,
                     1);
        d->AddCircle({pos.x + cw / 2, pos.y + 65}, 6, color(88, 113, 143), 32,
                     1);
      }
      ImGui::Dummy({cw, 135});
      caption(h.art.count(h.selected) && !h.art[h.selected].empty()
                  ? "Cover art"
                  : "Artwork unavailable");
      ImGui::Spacing();
      caption("Current game");
      double elapsed = 0;
      if (h.current_index >= 0 && h.current_index < (int)h.games.size())
        elapsed = h.games[h.current_index].value("elapsed_seconds", 0.0);
      ImGui::TextUnformatted(duration(elapsed).c_str());
      caption("Total time");
      ImGui::TextUnformatted(duration(h.total).c_str());
      ImGui::Spacing();
      ImGui::PushTextWrapPos(0);
      ImGui::TextUnformatted(h.status.c_str());
      ImGui::PopTextWrapPos();
      ImGui::EndChild();
      ImGui::PopStyleVar();
      ImGui::Spacing();
      bool bios_valid = h.settings.value("skip_bios", false);
      if (h.settings.contains("bios_files"))
        for (auto &path : h.settings["bios_files"])
          bios_valid = bios_valid ||
                       fs::is_regular_file(fs::u8path(path.get<std::string>()));
      bool valid = !h.games.empty() &&
                   !h.settings.value("output_folder", "").empty() && bios_valid;
      if (ImGui::Button(h.show_logs ? "Hide build log" : "Build log",
                        {130, 48}))
        h.show_logs = !h.show_logs;
      ImGui::SameLine();
      if (h.building) {
        if (ImGui::Button("Cancel", {100, 48})) {
          std::ofstream(h.data / "cancel-build") << "cancel";
          h.status = "Cancelling build";
        }
      } else if (!h.error.empty()) {
        ImGui::SameLine();
        ImGui::TextColored(rgba(244, 124, 135),
                           "Build needs attention - see log");
      }
      ImGui::SameLine(size.x - 168);
      ImGui::BeginDisabled(h.busy || !valid);
      ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 18);
      if (button("BUILD", {140, 80},
                 valid ? rgba(36, 139, 94) : rgba(44, 53, 66)))
        h.request("build");
      ImGui::PopStyleVar();
      ImGui::EndDisabled();
    }
    if (h.show_logs) {
      ImGui::SetNextWindowSize({850, 350}, ImGuiCond_FirstUseEver);
      if (ImGui::Begin("Build log", &h.show_logs)) {
        ImGui::BeginChild("lines", {0, 0}, false,
                          ImGuiWindowFlags_HorizontalScrollbar);
        for (auto &line : h.logs)
          ImGui::TextUnformatted(line.c_str());
        if (!h.error.empty())
          ImGui::TextWrapped("%s", h.error.c_str());
        ImGui::EndChild();
      }
      ImGui::End();
    }
    ImGui::SetCursorPos({28, size.y - 35});
    if (ImGui::SmallButton("GitHub"))
      SDL_OpenURL("https://github.com/RetroPortingToolKit");
    ImGui::SameLine();
    if (ImGui::SmallButton("Discord"))
      SDL_OpenURL("https://discord.gg/Jn6ZgtC85u");
    ImGui::SameLine();
    if (ImGui::SmallButton("Website"))
      SDL_OpenURL("https://retroportingtoolkit.com/");
    ImGui::End();
    ImGui::Render();
    int width, height;
    SDL_GetWindowSizeInPixels(w, &width, &height);
    glViewport(0, 0, width, height);
    glClearColor(.035, .055, .09, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    if (!screenshot.empty() && ++frames > 6 && !h.busy) {
      std::vector<unsigned char> pixels(width * height * 4);
      glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE,
                   pixels.data());
      stbi_flip_vertically_on_write(1);
      stbi_write_png(utf(screenshot).c_str(), width, height, 4, pixels.data(),
                     width * 4);
      done = true;
    }
    SDL_GL_SwapWindow(w);
    SDL_Delay(8);
  }
  if (h.busy && h.building) {
    std::ofstream(h.data / "cancel-build") << "cancel";
    auto until = Clock::now() + std::chrono::seconds(5);
    while (h.busy && Clock::now() < until) {
      h.poll();
      SDL_Delay(25);
    }
  }
  if (h.busy)
    h.control.request_cancel();
  if (h.worker.joinable())
    h.worker.join();
  if (h.art_worker.joinable()) {
    h.art_control.request_cancel();
    h.art_worker.join();
  }
  ImGui_ImplOpenGL3_Shutdown();
  ImGui_ImplSDL3_Shutdown();
  ImGui::DestroyContext();
  SDL_GL_DestroyContext(gl);
  SDL_DestroyWindow(w);
  SDL_Quit();
  return 0;
}
