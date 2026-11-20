// See companion.h.
// SPDX-License-Identifier: GPL-2.0-or-later
#include "companion.h"

#include "companion_input.h"

#ifndef MELEE_COMPANION

namespace companion {
bool available() { return false; }
void feed_touches(const Touch*, size_t, float) {}
void replay_saved(const std::string&) {}
void note_frame(const gx::Frame&) {}
float ui_scale(float density) { return density; }
Begin begin(bool) { return {}; }
void set_area(ImVec2, ImVec2) {}
Result page(gx::RenderOptions&) { return {}; }
void leave_settings() {}
}  // namespace companion

#else

#include "imgui_internal.h"
#include "gx_core.h"
#include "render_options.h"
#include "training_overlay.h"
#include "host.h"
#include "lcancel.h"
#include "net_trace.h"
#include "replay_control.h"
#include "native_practice.h"
#include "native_practice_model.h"
#include "practice_cpu.h"
#include "slippi_online.h"
#include "launcher_replay_data.h"
#include <nlohmann/json.hpp>
#ifdef __ANDROID__
#include "android_input.h"
#include "android_second_screen.h"
#endif
#include <algorithm>
#include <array>
#include <atomic>
#include <cfloat>
#include <cstdarg>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <mutex>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
namespace np = slippi::native_practice;

namespace companion {
namespace {

// ---- look
const ImU32 kAccent = IM_COL32(210, 40, 52, 255);
const ImU32 kAccentDim = IM_COL32(120, 30, 38, 255);
const ImU32 kGood = IM_COL32(96, 210, 128, 255);
const ImU32 kBad = IM_COL32(236, 96, 88, 255);
const ImU32 kMuted = IM_COL32(150, 156, 170, 255);
const ImU32 kPanel = IM_COL32(24, 27, 36, 255);
const ImU32 kPanelHi = IM_COL32(40, 44, 58, 255);
const ImVec4 kAccentV(210 / 255.f, 40 / 255.f, 52 / 255.f, 1.f);
const ImVec4 kMutedV(150 / 255.f, 156 / 255.f, 170 / 255.f, 1.f);

void heading(const char* text) {
  ImGui::Dummy(ImVec2(0, 4));
  ImGui::PushStyleColor(ImGuiCol_Text, kAccentV);
  ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 1.15f);
  ImGui::TextUnformatted(text);
  ImGui::PopFont();
  ImGui::PopStyleColor();
  ImGui::Separator();
}
void muted(const char* fmt, ...) IM_FMTARGS(1);
void muted(const char* fmt, ...) {
  va_list args;
  va_start(args, fmt);
  ImGui::PushStyleColor(ImGuiCol_Text, kMutedV);
  ImGui::TextWrappedV(fmt, args);
  ImGui::PopStyleColor();
  va_end(args);
}
// A button that reads well under a thumb: at least 40 units tall.
bool button(const char* label, float width = 0.0f, bool enabled = true, bool selected = false) {
  if (!enabled) ImGui::BeginDisabled();
  if (selected) {
    ImGui::PushStyleColor(ImGuiCol_Button, kAccentV);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, kAccentV);
  }
  const bool pressed = ImGui::Button(label, ImVec2(width, 40.0f));
  if (selected) ImGui::PopStyleColor(2);
  if (!enabled) ImGui::EndDisabled();
  return pressed && enabled;
}
// A toggle drawn as a wide button: on is the accent.
bool toggle(const char* label, bool* value, float width = 0.0f) {
  const bool pressed = button(label, width, true, *value);
  if (pressed) *value = !*value;
  return pressed;
}

std::string clock_text(double seconds) {
  if (seconds < 0) seconds = 0;
  char b[32];
  std::snprintf(b, sizeof b, "%d:%02d", (int)seconds / 60, (int)seconds % 60);
  return b;
}

// ---- a worker for the files (listing replays, reading their stats): never the render thread
class Worker {
 public:
  void post(std::function<void()> job) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      jobs_.push_back(std::move(job));
      if (!started_) {
        started_ = true;
        std::thread([this] { run(); }).detach();   // lives with the process
      }
    }
    cv_.notify_one();
  }
 private:
  void run() {
    for (;;) {
      std::function<void()> job;
      {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock, [this] { return !jobs_.empty(); });
        job = std::move(jobs_.front());
        jobs_.pop_front();
      }
      try { job(); } catch (const std::exception& e) { host::log("companion: %s", e.what()); } catch (...) {}
    }
  }
  std::mutex mutex_;
  std::condition_variable cv_;
  std::deque<std::function<void()>> jobs_;
  bool started_ = false;
};
Worker& worker() {
  static Worker* w = new Worker;   // never destroyed: its thread may still be running at exit
  return *w;
}

// ---- what the game showed last (note_frame)
struct Seen {
  std::array<gx::HudPlayerSnapshot, 4> hud{};
  std::array<std::string, 4> names{};
  uint8_t scene_major = 0, scene_minor = 0;
  bool match = false;
};
Seen g_seen;   // render thread only

// ---- page choice
int g_page = kOnline;
int g_page_before_settings = kOnline;
ImVec2 g_area_origin(0, 0), g_area_size(0, 0);   // set_area; size 0: the whole display
ImVec2 area_origin() { return g_area_size.x > 0 ? g_area_origin : ImVec2(0, 0); }
ImVec2 area_size() { return g_area_size.x > 0 ? g_area_size : ImGui::GetIO().DisplaySize; }
int g_last_scene = -1;
float g_size_choice = 1.0f;   // the player's size (A- / A+), kept in companion.ini
bool g_size_loaded = false;
const char* kIni = "companion.ini";

void load_size() {
  if (g_size_loaded) return;
  g_size_loaded = true;
  std::ifstream in(kIni);
  std::string key;
  float v = 0;
  while (in >> key >> v)
    if (key == "size") g_size_choice = std::clamp(v, 0.6f, 1.6f);
}
void save_size() {
  std::ofstream out(kIni, std::ios::trunc);
  out << "size " << g_size_choice << "\n";
}

bool sticks_page_available() {
#ifdef __ANDROID__
  return android_host::builtin_pad_present();
#else
  return false;
#endif
}

// ======================================================================================
// Replays: the list (worker-filled) and each one's stats
struct ReplayRow {
  std::string path;
  long long time = 0;
  bool inspected = false;
  launcher::replay::Info info;
};
std::mutex g_replays_mutex;
std::vector<ReplayRow> g_replays;               // newest first
bool g_replays_scanning = false, g_replays_stale = true;
std::map<std::string, launcher::replay::Info> g_replay_stats;   // with stats, by path
std::vector<std::string> g_stats_pending;
std::string g_selected_replay;                  // the Replays page's detail view (render thread)
// The games of this session, newest last (Online page: the last game and the set).
std::vector<launcher::replay::Info> g_session_games;
std::string g_last_saved;
std::atomic<bool> g_last_saved_new{false};

void scan_replays() {
  {
    std::lock_guard<std::mutex> lock(g_replays_mutex);
    if (g_replays_scanning) return;
    g_replays_scanning = true;
    g_replays_stale = false;
  }
  const std::string dir = host::options.replay_dir;
  worker().post([dir] {
    std::vector<ReplayRow> rows;
    std::error_code ec;
    for (fs::directory_iterator it(fs::u8path(dir), ec), end; !ec && it != end; it.increment(ec)) {
      if (!it->is_regular_file(ec)) continue;
      const std::string ext = it->path().extension().u8string();
      if (ext != ".slp" && ext != ".slpz") continue;
      ReplayRow r;
      r.path = it->path().u8string();
      r.time = launcher::replay::game_time(it->path(), it->last_write_time(ec));
      rows.push_back(std::move(r));
    }
    std::sort(rows.begin(), rows.end(), [](const ReplayRow& a, const ReplayRow& b) { return a.time > b.time; });
    if (rows.size() > 400) rows.resize(400);
    {
      std::lock_guard<std::mutex> lock(g_replays_mutex);
      // Keep what was already read for the same files.
      std::map<std::string, launcher::replay::Info> known;
      for (auto& old : g_replays) if (old.inspected) known[old.path] = std::move(old.info);
      for (auto& r : rows) if (auto f = known.find(r.path); f != known.end()) { r.info = std::move(f->second); r.inspected = true; }
      g_replays = rows;
    }
    // Then each one's players and stage, newest first.
    for (size_t i = 0; i < rows.size(); ++i) {
      {
        std::lock_guard<std::mutex> lock(g_replays_mutex);
        if (i >= g_replays.size() || g_replays[i].inspected) continue;
      }
      launcher::replay::Info info = launcher::replay::inspect(fs::u8path(rows[i].path), false);
      std::lock_guard<std::mutex> lock(g_replays_mutex);
      if (i < g_replays.size() && g_replays[i].path == rows[i].path) { g_replays[i].info = std::move(info); g_replays[i].inspected = true; }
    }
    std::lock_guard<std::mutex> lock(g_replays_mutex);
    g_replays_scanning = false;
  });
}

// The stats of one replay (with_stats): asked once, filled by the worker.
const launcher::replay::Info* replay_stats(const std::string& path) {
  std::lock_guard<std::mutex> lock(g_replays_mutex);
  if (auto f = g_replay_stats.find(path); f != g_replay_stats.end()) return &f->second;
  if (std::find(g_stats_pending.begin(), g_stats_pending.end(), path) == g_stats_pending.end()) {
    g_stats_pending.push_back(path);
    worker().post([path] {
      launcher::replay::Info info = launcher::replay::inspect(fs::u8path(path), true);
      std::lock_guard<std::mutex> lock(g_replays_mutex);
      g_replay_stats[path] = std::move(info);
      g_stats_pending.erase(std::remove(g_stats_pending.begin(), g_stats_pending.end(), path), g_stats_pending.end());
    });
  }
  return nullptr;
}

std::string players_line(const launcher::replay::Info& info) {
  std::string s;
  for (size_t i = 0; i < info.players.size(); ++i) {
    const auto& p = info.players[i];
    if (i) s += "  vs  ";
    s += launcher::replay::character_name(p.character);
    if (!p.name.empty()) s += " (" + p.name + ")";
    else if (!p.code.empty()) s += " (" + p.code + ")";
  }
  return s;
}

double replay_seconds(const launcher::replay::Info& info) { return std::max(0, info.last_frame + 124) / 60.0; }

// Slippi-style numbers for each player of a game, as the Slippi Launcher shows them.
void stats_table(const launcher::replay::Info& info) {
  if (!info.stats_loaded || info.players.empty()) { muted("Reading the game..."); return; }
  const double minutes = std::max(1.0 / 60.0, replay_seconds(info) / 60.0);
  const int columns = 1 + (int)info.players.size();
  if (!ImGui::BeginTable("##stats", columns, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingStretchProp)) return;
  ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthStretch, 1.3f);
  for (const auto& p : info.players) {
    std::string label = launcher::replay::character_name(p.character);
    if (!p.code.empty()) label += "\n" + p.code;
    else if (!p.name.empty()) label += "\n" + p.name;
    ImGui::TableSetupColumn(label.c_str(), ImGuiTableColumnFlags_WidthStretch, 1.0f);
  }
  ImGui::TableHeadersRow();
  auto row = [&](const char* name, const std::function<std::string(const launcher::replay::Player&)>& value) {
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::TextUnformatted(name);
    for (const auto& p : info.players) { ImGui::TableNextColumn(); ImGui::TextUnformatted(value(p).c_str()); }
  };
  auto num = [](double v, const char* fmt) { char b[32]; std::snprintf(b, sizeof b, fmt, v); return std::string(b); };
  row("Kills", [&](const auto& p) { return std::to_string(p.kills); });
  row("Damage done", [&](const auto& p) { return num(p.damage_done, "%.0f%%"); });
  row("Openings / kill", [&](const auto& p) { return p.kills ? num((double)p.openings / p.kills, "%.1f") : std::string("-"); });
  row("Damage / opening", [&](const auto& p) { return p.openings ? num(p.damage_done / p.openings, "%.1f") : std::string("-"); });
  row("Neutral wins", [&](const auto& p) { return std::to_string(p.neutral_wins); });
  row("Counter hits", [&](const auto& p) { return std::to_string(p.counter_hits); });
  if (info.l_cancel_available)
    row("L-cancels", [&](const auto& p) {
      const int all = p.l_success + p.l_fail;
      return all ? num(100.0 * p.l_success / all, "%.0f%%") + " (" + std::to_string(p.l_success) + "/" + std::to_string(all) + ")" : std::string("-");
    });
  row("Inputs / min", [&](const auto& p) { return num(p.inputs / minutes, "%.0f"); });
  row("Wavedashes", [&](const auto& p) { return std::to_string(p.wavedashes); });
  ImGui::EndTable();
}

// ======================================================================================
// Online
char g_code[16] = "";
bool g_teams = false;
std::string g_code_error;
struct Recent { std::vector<std::string> direct, teams; double read_at = -100; };
Recent g_recent;
std::mutex g_recent_mutex;

void refresh_recent_codes() {
  const double now = host::now_seconds();
  {
    std::lock_guard<std::mutex> lock(g_recent_mutex);
    if (now - g_recent.read_at < 5.0) return;
    g_recent.read_at = now;
  }
  const std::string dir = slippi::online::config().user_dir;
  worker().post([dir] {
    auto read = [&](const char* name) {
      std::vector<std::string> codes;
      std::ifstream in(fs::u8path(dir) / name);
      if (!in) return codes;
      try {
        const nlohmann::json j = nlohmann::json::parse(in, nullptr, false);
        if (j.is_array())
          for (const auto& e : j)
            if (e.is_object() && e.find("connectCode") != e.end() && e["connectCode"].is_string()) codes.push_back(e["connectCode"].get<std::string>());
      } catch (...) {}
      if (codes.size() > 8) codes.resize(8);
      return codes;
    };
    Recent r;
    r.direct = read("direct-codes.json");
    r.teams = read("teams-codes.json");
    std::lock_guard<std::mutex> lock(g_recent_mutex);
    r.read_at = g_recent.read_at;
    g_recent = std::move(r);
  });
}

// A keyboard for connect codes (letters, digits, #), sized to the page.
void code_keyboard(float width) {
  static const char* const rows[] = {"1234567890", "QWERTYUIOP", "ASDFGHJKL#", "ZXCVBNM"};
  const float gap = 4.0f;
  const float key = std::floor((width - gap * 9.0f) / 10.0f);
  for (const char* row : rows) {
    const int n = (int)std::strlen(row);
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (10 - n) * (key + gap) * 0.5f);
    for (int i = 0; i < n; ++i) {
      if (i) ImGui::SameLine(0, gap);
      char label[8] = {row[i], 0};
      ImGui::PushID(row[i]);
      if (ImGui::Button(label, ImVec2(key, 42.0f))) {
        const size_t len = std::strlen(g_code);
        if (len + 1 < 10) { g_code[len] = row[i]; g_code[len + 1] = 0; }
        g_code_error.clear();
      }
      ImGui::PopID();
    }
  }
  if (ImGui::Button("Delete", ImVec2(key * 3 + gap * 2, 42.0f))) {
    const size_t len = std::strlen(g_code);
    if (len) g_code[len - 1] = 0;
  }
  ImGui::SameLine(0, gap);
  if (ImGui::Button("Clear", ImVec2(key * 3 + gap * 2, 42.0f))) g_code[0] = 0;
}

void net_graph(float width) {
  static std::vector<net_trace::Record> records(net_trace::kCapacity);
  const size_t n = net_trace::snapshot(records.data(), records.size());
  if (n < 2) return;
  const float height = 70.0f;
  const ImVec2 o = ImGui::GetCursorScreenPos();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  dl->AddRectFilled(o, ImVec2(o.x + width, o.y + height), kPanel, 4.0f);
  int max_ping = 40;
  for (size_t i = 0; i < n; ++i) max_ping = std::max<int>(max_ping, records[i].ping_ms);
  const float dx = width / (float)(n - 1);
  for (size_t i = 0; i < n; ++i) {
    const auto& r = records[i];
    const float x = o.x + dx * i;
    if (r.rollbacks) dl->AddLine(ImVec2(x, o.y + height), ImVec2(x, o.y + height - std::min(1.0f, r.rollback_depth / 7.0f) * height * 0.6f - 3), kAccentDim, std::max(1.0f, dx));
    if (r.flags & 128) dl->AddLine(ImVec2(x, o.y), ImVec2(x, o.y + height), kBad, 2.0f);   // a desync
    if (i) {
      const auto& p = records[i - 1];
      dl->AddLine(ImVec2(x - dx, o.y + height - p.ping_ms * height / max_ping), ImVec2(x, o.y + height - r.ping_ms * height / max_ping), kGood, 2.0f);
    }
  }
  int rollbacks = 0;
  for (size_t i = 0; i < n; ++i) rollbacks += records[i].rollbacks;
  char label[96];
  std::snprintf(label, sizeof label, "ping %d ms   rollbacks %d in the last %.0f s", records[n - 1].ping_ms, rollbacks,
                records[n - 1].wall - records[0].wall);
  dl->AddText(ImVec2(o.x + 6, o.y + 4), IM_COL32(220, 224, 232, 255), label);
  ImGui::Dummy(ImVec2(width, height));
}

void page_online(const np::Snapshot& sn) {
  const float width = ImGui::GetContentRegionAvail().x;
  refresh_recent_codes();
  // ---- the match now
  const bool searching = sn.phase == np::Phase::Searching;
  const bool online_match = g_seen.match && !g_seen.names[0].empty() && !g_seen.names[1].empty();
  if (online_match || sn.phase == np::Phase::InMatch || sn.phase == np::Phase::OnlineFlow) {
    heading("This match");
    for (int i = 0; i < 4; ++i) {
      if (g_seen.names[i].empty()) continue;
      const auto& h = g_seen.hud[i];
      ImGui::Text("P%d  %s", i + 1, g_seen.names[i].c_str());
      if (h.present) {
        ImGui::SameLine(width * 0.62f);
        ImGui::Text("%.0f%%   %d stock%s", h.percent, h.stocks, h.stocks == 1 ? "" : "s");
      }
    }
    const int ping = slippi::online::ping_ms();
    if (ping > 0) ImGui::Text("Ping %d ms", ping);
    net_graph(width);
  }
  // ---- the search
  heading(searching ? "Searching" : "Play a connect code");
  if (!sn.status.empty()) ImGui::TextWrapped("%s", sn.status.c_str());
  if (!sn.detail.empty()) muted("%s", sn.detail.c_str());
  if (searching) {
    if (button("Cancel the search", width)) np::submit_cancel();
  } else {
    if (button("Direct", width * 0.5f - 4, true, !g_teams)) g_teams = false;
    ImGui::SameLine(0, 8);
    if (button("Teams", width * 0.5f - 4, true, g_teams)) g_teams = true;
    // The code as typed, large.
    ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 2.0f);
    const ImVec2 o = ImGui::GetCursorScreenPos();
    const float h = ImGui::GetFontSize() + 16.0f;
    ImGui::GetWindowDrawList()->AddRectFilled(o, ImVec2(o.x + width, o.y + h), kPanel, 6.0f);
    const bool blink = std::fmod(ImGui::GetTime(), 1.0) < 0.5;
    char shown[24];
    std::snprintf(shown, sizeof shown, "%s%s", g_code, blink ? "_" : " ");
    ImGui::GetWindowDrawList()->AddText(ImVec2(o.x + 12, o.y + 8), g_code[0] ? IM_COL32(240, 242, 246, 255) : kMuted,
                                        g_code[0] ? shown : (blink ? "ABCD#123_" : "ABCD#123"));
    ImGui::Dummy(ImVec2(width, h));
    ImGui::PopFont();
    code_keyboard(width);
    if (!g_code_error.empty()) {
      ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.93f, 0.4f, 0.36f, 1));
      ImGui::TextWrapped("%s", g_code_error.c_str());
      ImGui::PopStyleColor();
    }
    if (button(g_teams ? "Search (Teams)" : "Search (Direct)", width, sn.can_start && g_code[0])) {
      std::string normalized, error;
      const bool ok = g_teams ? np::normalize_teams_code(g_code, &normalized, &error) : np::normalize_direct_code(g_code, &normalized, &error);
      if (!ok) g_code_error = error.empty() ? "That is not a connect code." : error;
      else if (g_teams) np::submit_start_teams(normalized);
      else np::submit_start_direct(normalized);
    }
    if (!sn.can_start)
      muted("The search starts from Training or a match against the CPU (you keep playing while it looks). "
            "On Melee's own Direct screen, type the code there.");
    std::vector<std::string> recent;
    {
      std::lock_guard<std::mutex> lock(g_recent_mutex);
      recent = g_teams ? g_recent.teams : g_recent.direct;
    }
    if (!recent.empty()) {
      muted("Recent");
      float x = 0;
      for (const std::string& code : recent) {
        const float w = ImGui::CalcTextSize(code.c_str()).x + 28.0f;
        if (x > 0 && x + w > width) x = 0;
        else if (x > 0) ImGui::SameLine(0, 6);
        if (ImGui::Button(code.c_str(), ImVec2(w, 38.0f))) { std::snprintf(g_code, sizeof g_code, "%s", code.c_str()); g_code_error.clear(); }
        x += w + 6;
      }
    }
  }
  // ---- the last game and the set
  launcher::replay::Info last;
  bool have_last = false;
  std::vector<launcher::replay::Info> games;
  {
    std::lock_guard<std::mutex> lock(g_replays_mutex);
    if (!g_session_games.empty()) { last = g_session_games.back(); have_last = true; games = g_session_games; }
  }
  if (have_last) {
    heading("Last game");
    ImGui::TextWrapped("%s  %s  %s", last.stage.c_str(), clock_text(replay_seconds(last)).c_str(), players_line(last).c_str());
    if (last.winner >= 0 && last.winner < (int)last.players.size()) {
      const auto& w = last.players[last.winner];
      ImGui::Text("Won by %s", !w.code.empty() ? w.code.c_str() : !w.name.empty() ? w.name.c_str() : launcher::replay::character_name(w.character));
    }
    stats_table(last);
    // This session's games between the same two players: the set so far.
    if (games.size() >= 2 && last.players.size() == 2) {
      auto who = [](const launcher::replay::Player& p) { return !p.code.empty() ? p.code : p.name; };
      const std::string a = who(last.players[0]), b = who(last.players[1]);
      int wins_a = 0, wins_b = 0;
      for (auto it = games.rbegin(); it != games.rend(); ++it) {
        if (it->players.size() != 2) break;
        const std::string x = who(it->players[0]), y = who(it->players[1]);
        if (!((x == a && y == b) || (x == b && y == a))) break;
        if (it->winner < 0) continue;
        (who(it->players[it->winner]) == a ? wins_a : wins_b)++;
      }
      if (wins_a + wins_b >= 2 && !a.empty() && !b.empty()) ImGui::Text("Set: %s %d - %d %s", a.c_str(), wins_a, wins_b, b.c_str());
    }
  }
}

// ======================================================================================
// Training
constexpr uint32_t kTrainingScene = 0x1C;
float bits(uint32_t v) { float f; std::memcpy(&f, &v, sizeof f); return f; }

// The controller as the game read it: both sticks, the triggers and the buttons, `size` wide.
void controller(const MuPadStatus& pad, float size) {
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImVec2 o = ImGui::GetCursorScreenPos();
  const float r = size * 0.16f;
  auto stick = [&](ImVec2 c, int8_t x, int8_t y, ImU32 color, const char* name) {
    dl->AddCircleFilled(c, r, kPanel, 32);
    dl->AddCircle(c, r, kPanelHi, 32, 2.0f);
    const ImVec2 p(c.x + std::clamp(x / 80.0f, -1.2f, 1.2f) * r, c.y - std::clamp(y / 80.0f, -1.2f, 1.2f) * r);
    dl->AddLine(c, p, color, 2.0f);
    dl->AddCircleFilled(p, r * 0.22f, color, 20);
    char v[24];
    std::snprintf(v, sizeof v, "%s %d,%d", name, x, y);
    dl->AddText(ImVec2(c.x - r, c.y + r + 4), kMuted, v);
  };
  stick(ImVec2(o.x + r + 6, o.y + r + 22), pad.stick_x, pad.stick_y, IM_COL32(236, 240, 246, 255), "Stick");
  stick(ImVec2(o.x + size * 0.62f, o.y + r * 1.6f + 22), pad.sub_x, pad.sub_y, IM_COL32(250, 210, 70, 255), "C");
  // Triggers: the analog value, and the click.
  auto trigger = [&](float x, uint8_t value, bool click, const char* name) {
    const ImVec2 a(o.x + x, o.y + 2), b(a.x + size * 0.18f, a.y + 14);
    dl->AddRectFilled(a, b, kPanel, 3.0f);
    dl->AddRectFilled(a, ImVec2(a.x + (b.x - a.x) * value / 255.0f, b.y), click ? kAccent : IM_COL32(170, 176, 190, 255), 3.0f);
    dl->AddText(ImVec2(a.x + 4, a.y), IM_COL32(20, 20, 24, 255), name);
  };
  trigger(0, pad.trigger_l, pad.button & 0x40, "L");
  trigger(size * 0.8f, pad.trigger_r, pad.button & 0x20, "R");
  // Buttons.
  auto lamp = [&](ImVec2 c, float radius, bool on, ImU32 color, const char* name) {
    dl->AddCircleFilled(c, radius, on ? color : kPanel, 24);
    dl->AddCircle(c, radius, kPanelHi, 24, 1.5f);
    const ImVec2 t = ImGui::CalcTextSize(name);
    dl->AddText(ImVec2(c.x - t.x * 0.5f, c.y - t.y * 0.5f), on ? IM_COL32(16, 16, 20, 255) : kMuted, name);
  };
  const float bx = o.x + size * 0.86f, by = o.y + r + 30;
  lamp(ImVec2(bx, by), r * 0.42f, pad.button & 0x100, IM_COL32(60, 200, 150, 255), "A");
  lamp(ImVec2(bx - r * 0.75f, by + r * 0.45f), r * 0.28f, pad.button & 0x200, IM_COL32(230, 80, 80, 255), "B");
  lamp(ImVec2(bx + r * 0.7f, by - r * 0.15f), r * 0.28f, pad.button & 0x400, IM_COL32(220, 224, 232, 255), "X");
  lamp(ImVec2(bx - r * 0.1f, by - r * 0.72f), r * 0.28f, pad.button & 0x800, IM_COL32(220, 224, 232, 255), "Y");
  lamp(ImVec2(bx + r * 0.55f, by - r * 0.85f), r * 0.24f, pad.button & 0x10, IM_COL32(140, 110, 230, 255), "Z");
  lamp(ImVec2(o.x + size * 0.4f, o.y + r + 22), r * 0.22f, pad.button & 0x1000, IM_COL32(220, 224, 232, 255), "S");
  // D-pad.
  const ImVec2 d(o.x + size * 0.4f, o.y + r * 2.4f + 22);
  const float s = r * 0.22f;
  auto pad_arm = [&](float dx, float dy, bool on) {
    dl->AddRectFilled(ImVec2(d.x + dx - s * 0.5f, d.y + dy - s * 0.5f), ImVec2(d.x + dx + s * 0.5f, d.y + dy + s * 0.5f), on ? kAccent : kPanelHi, 2.0f);
  };
  pad_arm(0, -s, pad.button & 0x8);
  pad_arm(0, s, pad.button & 0x4);
  pad_arm(-s, 0, pad.button & 0x1);
  pad_arm(s, 0, pad.button & 0x2);
  ImGui::Dummy(ImVec2(size, r * 2.6f + 50));   // down to the C-stick's reading, the lowest thing drawn
}

Result page_training(const np::Snapshot& sn) {
  Result result;
  const float width = ImGui::GetContentRegionAvail().x;
  MuStatePod state{};
  MuPadStatus pads[4]{};
  const bool have = training_overlay::latest(&state, pads);
  const bool training = have && state.scene_major == kTrainingScene;
  const int port = std::clamp(sn.controller_port, 0, 3);
  heading("Controller");
  if (have) controller(pads[port], std::min(width, 520.0f));
  else muted("Waiting for the game...");
  if (!training) muted("The fighters' readings and the tools below are for Training mode.");
  if (training) {
    heading("Fighters");
    if (ImGui::BeginTable("##fighters", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
      ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthStretch, 0.4f);
      ImGui::TableSetupColumn("Action", ImGuiTableColumnFlags_WidthStretch, 1.6f);
      ImGui::TableSetupColumn("Frame", ImGuiTableColumnFlags_WidthStretch, 0.6f);
      ImGui::TableSetupColumn("Damage", ImGuiTableColumnFlags_WidthStretch, 0.7f);
      ImGui::TableHeadersRow();
      for (int i = 0; i < 4; ++i) {
        const MuFighterState& f = state.player[i];
        if (!f.present) continue;
        ImGui::TableNextRow();
        ImGui::TableNextColumn(); ImGui::Text("P%d", i + 1);
        ImGui::TableNextColumn();
        const char* name = training_overlay::action_name(f.action);
        if (*name) ImGui::TextUnformatted(name); else ImGui::Text("Own action %u", f.action);
        ImGui::TableNextColumn(); ImGui::Text("%.0f", bits(f.anim_frame));
        ImGui::TableNextColumn(); ImGui::Text("%.1f%%", bits(f.percent));
      }
      ImGui::EndTable();
    }
    int adv = 0, kind = 0;
    if (training_overlay::advantage(&adv, &kind)) {
      ImGui::PushStyleColor(ImGuiCol_Text, adv >= 0 ? ImVec4(0.38f, 0.82f, 0.5f, 1) : ImVec4(0.93f, 0.38f, 0.35f, 1));
      ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 1.4f);
      ImGui::Text("Frame advantage %+d on %s", adv, kind == 2 ? "shield" : "hit");
      ImGui::PopFont();
      ImGui::PopStyleColor();
    }
  }
  // ---- L-cancels
  heading("L-cancels");
  lcancel::Landing landings[24];
  int total = 0, successes = 0;
  const size_t n = lcancel::recent_landings(landings, 24, &total, &successes);
  if (!total) muted("Land an aerial: each one shows here, with how early the L, R or Z press was.");
  else {
    ImGui::Text("%d of %d  (%.0f%%)", successes, total, 100.0 * successes / total);
    ImGui::SameLine(width - 90);
    if (ImGui::SmallButton("Reset")) lcancel::reset_history();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 o = ImGui::GetCursorScreenPos();
    const float dot = std::min(18.0f, (width - 4) / 24.0f);
    for (size_t i = 0; i < n; ++i)
      dl->AddCircleFilled(ImVec2(o.x + dot * (i + 0.5f), o.y + dot * 0.5f), dot * 0.38f, landings[i].success ? kGood : kBad, 16);
    ImGui::Dummy(ImVec2(width, dot + 4));
    const lcancel::Landing& last = landings[n - 1];
    const int window = lcancel::lcancel_window();
    // `frames` is the game's own count at the landing (frames since the press; under the window cancels).
    if (last.frames >= 255) ImGui::TextWrapped("Last landing: no L, R or Z press before it.");
    else if (last.success) ImGui::TextWrapped("Last landing: L-cancelled, pressed %d frame%s before (under %d).", last.frames, last.frames == 1 ? "" : "s", window);
    else ImGui::TextWrapped("Last landing: pressed %d frames before, %d too early (under %d cancels).", last.frames, last.frames - window + 1, window);
  }
  // ---- the tools (what the Practice menu's Tools page sets)
  heading("Tools");
  MuPcConfig cfg = practice_cpu::config();
  const float half = width * 0.5f - 4;
  struct Tool { const char* label; uint8_t bit; };
  static const Tool tools[] = {{"Save / load on the D-pad", MU_PC_TOOL_DPAD}, {"Hitboxes", MU_PC_TOOL_HITBOXES},
                               {"Inputs and frame data", MU_PC_TOOL_OVERLAY}, {"Frame advance (L+R+Z)", MU_PC_TOOL_FRAME_ADVANCE},
                               {"Loop the recording", MU_PC_TOOL_LOOP}, {"Combo counter", MU_PC_TOOL_COMBO_COUNTER}};
  for (size_t i = 0; i < sizeof tools / sizeof tools[0]; ++i) {
    if (i % 2) ImGui::SameLine(0, 8);
    bool on = (cfg.tools & tools[i].bit) != 0;
    ImGui::PushID((int)i);
    if (toggle(tools[i].label, &on, half)) {
      cfg.tools = on ? (cfg.tools | tools[i].bit) : (cfg.tools & ~tools[i].bit);
      practice_cpu::set_config(cfg);
      result.tools_changed = true;
    }
    ImGui::PopID();
  }
  // Save and load: the Lab's own D-pad save state, pressed for the player.
  const bool dpad = (cfg.tools & MU_PC_TOOL_DPAD) != 0;
  if (button("Save state", half, training)) {
    if (!dpad) { cfg.tools |= MU_PC_TOOL_DPAD; practice_cpu::set_config(cfg); result.tools_changed = true; }
    host::input_pulse(port, 0x0002, 3);   // D-pad right
  }
  ImGui::SameLine(0, 8);
  if (button("Load state", half, training)) {
    if (!dpad) { cfg.tools |= MU_PC_TOOL_DPAD; practice_cpu::set_config(cfg); result.tools_changed = true; }
    host::input_pulse(port, 0x0001, 3);   // D-pad left
  }
  if (training) muted("Save and load are Training's own: the D-pad does the same (right saves, left loads).");
  return result;
}

// ======================================================================================
// Replays
void page_replays() {
  const float width = ImGui::GetContentRegionAvail().x;
  // ---- the replay playing in this game: its timeline
  const replay_control::Status st = replay_control::status();
  if (st.active) {
    heading("Now playing");
    const int first = st.first_frame, last = std::max(st.last_frame, st.first_frame + 1);
    static int dragging = INT32_MIN;
    int frame = dragging != INT32_MIN ? dragging : std::clamp(st.frame, first, last);
    ImGui::SetNextItemWidth(width);
    char label[48];
    std::snprintf(label, sizeof label, "%s / %s", clock_text((frame - first) / 60.0).c_str(), clock_text((last - first) / 60.0).c_str());
    if (ImGui::SliderInt("##timeline", &frame, first, last, label)) dragging = frame;
    if (ImGui::IsItemDeactivatedAfterEdit() && dragging != INT32_MIN) { replay_control::seek_to(dragging); dragging = INT32_MIN; }
    if (!ImGui::IsItemActive() && !ImGui::IsItemDeactivated()) dragging = INT32_MIN;
    const float w5 = (width - 32) / 5.0f;
    if (button("-5 s", w5)) replay_control::seek_to(std::max(first, st.frame - 300));
    ImGui::SameLine(0, 8);
    if (button(st.paused ? "Play" : "Pause", w5, true, st.paused)) replay_control::toggle_pause();
    ImGui::SameLine(0, 8);
    if (button("Step", w5, st.paused)) replay_control::step_frame();
    ImGui::SameLine(0, 8);
    if (button("+5 s", w5)) replay_control::seek_to(std::min(last, st.frame + 300));
    ImGui::SameLine(0, 8);
    bool fast = replay_control::fast_forward_held();
    if (toggle("Fast", &fast, w5)) replay_control::set_fast_forward(fast);
    const float w3 = (width - 16) / 3.0f;
    static const char* const speeds[] = {"1/4 speed", "1/2 speed", "Full speed"};
    const int level = st.speed <= 0.3 ? 0 : st.speed <= 0.6 ? 1 : 2;
    for (int i = 0; i < 3; ++i) {
      if (i) ImGui::SameLine(0, 8);
      if (button(speeds[i], w3, true, level == i)) replay_control::set_speed_level(i);
    }
  }
  // ---- one replay
  if (!g_selected_replay.empty()) {
    launcher::replay::Info info;
    if (const auto* s = replay_stats(g_selected_replay)) info = *s;
    if (button("< All replays", width * 0.4f)) { g_selected_replay.clear(); return; }
    heading(info.valid ? info.stage.c_str() : "Replay");
    if (info.valid) {
      ImGui::TextWrapped("%s", players_line(info).c_str());
      muted("%s   %s", launcher::replay::display_date(info).c_str(), clock_text(replay_seconds(info)).c_str());
    }
    stats_table(info);
    ImGui::Dummy(ImVec2(0, 6));
#ifdef __ANDROID__
    const float half = width * 0.5f - 4;
    // Playback records what it showed again (replay-dir): into "viewer" beside the recordings, so
    // watching does not add a copy to this list each time.
    const fs::path viewer_dir = fs::u8path(host::options.replay_dir) / "viewer";
    if (button("Watch on the top screen", half, !st.active)) {
      if (android_host::relaunch("--replay " + g_selected_replay + " --replay-dir " + viewer_dir.u8string(), true)) host::request_exit(0);
    }
    ImGui::SameLine(0, 8);
    if (button("Watch on this screen", half, android_host::second_display())) {
      android_host::viewer_close();
      android_host::viewer_launch({"--replay", g_selected_replay, "--volume", "0", "--replay-dir", viewer_dir.u8string()});
    }
    muted("The top screen leaves the game and comes back to it when the replay ends. This screen plays it here while you go on playing.");
#endif
    return;
  }
  // ---- the list
  bool stale;
  {
    std::lock_guard<std::mutex> lock(g_replays_mutex);
    stale = g_replays_stale;
  }
  if (stale) scan_replays();
  heading("Replays");
  std::lock_guard<std::mutex> lock(g_replays_mutex);
  if (g_replays.empty()) { muted(g_replays_scanning ? "Looking..." : "No replays yet: every game played is recorded."); return; }
  ImGuiListClipper clipper;
  clipper.Begin((int)g_replays.size(), 62.0f);
  while (clipper.Step()) {
    for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
      const ReplayRow& r = g_replays[i];
      ImGui::PushID(i);
      const ImVec2 o = ImGui::GetCursorScreenPos();
      const bool clicked = ImGui::InvisibleButton("##row", ImVec2(width, 58.0f));
      ImDrawList* dl = ImGui::GetWindowDrawList();
      dl->AddRectFilled(o, ImVec2(o.x + width, o.y + 58), ImGui::IsItemHovered() ? kPanelHi : kPanel, 6.0f);
      if (r.inspected && r.info.valid) {
        dl->AddText(ImVec2(o.x + 10, o.y + 6), IM_COL32(236, 240, 246, 255), players_line(r.info).c_str());
        const std::string line = launcher::replay::display_date(r.info) + "   " + r.info.stage + "   " + clock_text(replay_seconds(r.info));
        dl->AddText(ImVec2(o.x + 10, o.y + 32), kMuted, line.c_str());
      } else {
        dl->AddText(ImVec2(o.x + 10, o.y + 6), IM_COL32(236, 240, 246, 255), fs::u8path(r.path).filename().u8string().c_str());
        dl->AddText(ImVec2(o.x + 10, o.y + 32), kMuted, r.inspected ? "Not a replay this game can read" : "Reading...");
      }
      if (clicked) g_selected_replay = r.path;
      ImGui::PopID();
    }
  }
  clipper.End();
}

// ======================================================================================
// Room
Result page_room(const np::Snapshot& sn, gx::RenderOptions& options) {
  Result result;
  const float width = ImGui::GetContentRegionAvail().x;
  const auto& w = sn.warmup;
  heading("Warm-up room");
  if (!w.active && !w.offered) {
    muted("In a Direct or Teams set, or waiting in an arena room, WARM UP takes you to Training while you wait. "
          "It shows here then: who is where, the room's game and its score.");
  } else {
    if (w.active) ImGui::Text("Warming up%s%s", w.mode.empty() ? "" : " - ", w.mode.c_str());
    if (!w.sparring.empty()) ImGui::Text("Sparring: %s", w.sparring.c_str());
    if (!w.arena_place.empty()) ImGui::Text("Your place: %s", w.arena_place.c_str());
    if (!w.arena_versus.empty()) ImGui::Text("Playing now: %s", w.arena_versus.c_str());
    if (w.countdown_ms >= 0) {
      ImGui::PushStyleColor(ImGuiCol_Text, kAccentV);
      ImGui::Text("Your turn in %d", (w.countdown_ms + 999) / 1000);
      ImGui::PopStyleColor();
    }
    for (int i = 0; i < w.peers && i < 3; ++i)
      if (!w.peer[i].name.empty()) ImGui::BulletText("%s", w.peer[i].name.c_str());
    if (w.score_live) {
      heading("The room's game");
      for (const auto& f : w.score_fighter) {
        if (!f.present) continue;
        ImGui::Text("%s", f.name.empty() ? "Player" : f.name.c_str());
        ImGui::SameLine(width * 0.6f);
        ImGui::Text("%.0f%%  %d stock%s", f.percent, f.stocks, f.stocks == 1 ? "" : "s");
      }
    }
    if (w.offered && !w.active) {
      if (button("WARM UP", width, w.enabled)) np::submit_warmup();
      if (!w.enabled && !w.why.empty()) muted("%s", w.why.c_str());
    }
    if (w.active && button("Back to the match", width)) np::submit_warmup_return();
  }
#ifdef __ANDROID__
  if (android_host::second_display()) {
    heading("The room's match");
    const int watch = gx::RenderOptions::live_warmup_watch();
    if (button("Show it on this screen", width * 0.5f - 4, true, watch != 0)) {
      options.warmup_watch = gx::RenderOptions::live_warmup_watch() = 1;
      result.options_changed = true;
    }
    ImGui::SameLine(0, 8);
    if (button("Don't show it", width * 0.5f - 4, true, watch == 0)) {
      options.warmup_watch = gx::RenderOptions::live_warmup_watch() = 0;
      android_host::viewer_close();
      result.options_changed = true;
    }
    muted("While you warm up in an arena room, the room's game plays on this screen (a second game, muted). Tap here again after it ends.");
    if (android_host::viewer_running() && button("Close it now", width)) android_host::viewer_close();
  }
#else
  (void)options;
#endif
  return result;
}

// ======================================================================================
// Sticks (the handheld's built-in sticks)
#ifdef __ANDROID__
void stick_plot(const char* name, const android_host::StickCalibration& cal, float rx, float ry, bool raw, float size) {
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImVec2 o = ImGui::GetCursorScreenPos();
  const float r = size * 0.5f - 6;
  const ImVec2 c(o.x + size * 0.5f, o.y + size * 0.5f);
  dl->AddRectFilled(o, ImVec2(o.x + size, o.y + size), kPanel, 6.0f);
  dl->AddCircle(c, r, kPanelHi, 48, 2.0f);
  dl->AddCircle(c, r * cal.deadzone, kAccentDim, 24, 1.5f);
  // The raw value, re-centred and scaled outward as calibrated (what reaches the curve), and the result.
  float sx = rx - cal.center_x, sy = ry - cal.center_y;
  if (raw) { sx *= sx >= 0 ? cal.scale_x_pos : cal.scale_x_neg; sy *= sy >= 0 ? cal.scale_y_pos : cal.scale_y_neg; }
  float ox = 0, oy = 0;
  android_host::calibrate_stick(cal, rx, ry, raw, ox, oy);
  dl->AddCircleFilled(ImVec2(c.x + std::clamp(sx, -1.3f, 1.3f) * r, c.y + std::clamp(sy, -1.3f, 1.3f) * r), 5.0f, kMuted, 12);
  dl->AddCircleFilled(ImVec2(c.x + ox * r, c.y + oy * r), 8.0f, kAccent, 16);
  char text[64];
  std::snprintf(text, sizeof text, "%s  %.2f, %.2f", name, ox, -oy);
  dl->AddText(ImVec2(o.x + 6, o.y + 4), IM_COL32(220, 224, 232, 255), text);
  ImGui::Dummy(ImVec2(size, size));
}

void page_sticks() {
  const float width = ImGui::GetContentRegionAvail().x;
  static android_host::StickCalibration main_cal, c_cal;
  static bool loaded = false, dirty = false;
  static std::string message;
  if (!loaded) { android_host::builtin_calibration(main_cal, c_cal); loaded = true; }
  float raw[4];
  const bool fresh = android_host::builtin_raw(raw);
  heading("Built-in sticks");
  if (!fresh) muted("The raw reader (AYN's mapping service) is not delivering: the sticks come from Android, already scaled.");
  const float plot = std::min(width * 0.5f - 6, 260.0f);
  stick_plot("Stick", main_cal, raw[0], raw[1], fresh, plot);
  ImGui::SameLine(0, 12);
  stick_plot("C-stick", c_cal, raw[2], raw[3], fresh, plot);
  muted("Grey: the stick as read, scaled outward. Red: what the game gets.");
  // ---- measuring
  static int measuring = 0;   // 1 centre, 2 range
  static double measure_until = 0;
  static double sum[4]; static int count = 0;
  static float lo[4], hi[4];
  const double now = host::now_seconds();
  if (measuring == 1) {
    for (int i = 0; i < 4; ++i) sum[i] += raw[i];
    ++count;
    if (now >= measure_until && count > 0) {
      main_cal.center_x = (float)(sum[0] / count); main_cal.center_y = (float)(sum[1] / count);
      c_cal.center_x = (float)(sum[2] / count); c_cal.center_y = (float)(sum[3] / count);
      measuring = 0; dirty = true;
      message = "Centre set from where the sticks rest.";
    }
  } else if (measuring == 2) {
    for (int i = 0; i < 4; ++i) { lo[i] = std::min(lo[i], raw[i]); hi[i] = std::max(hi[i], raw[i]); }
    if (now >= measure_until) {
      auto scale = [](float reach) { return std::clamp(reach > 0.02f ? 1.0f / reach : 4.0f, 0.25f, 8.0f); };
      main_cal.scale_x_pos = scale(hi[0] - main_cal.center_x); main_cal.scale_x_neg = scale(main_cal.center_x - lo[0]);
      main_cal.scale_y_pos = scale(hi[1] - main_cal.center_y); main_cal.scale_y_neg = scale(main_cal.center_y - lo[1]);
      c_cal.scale_x_pos = scale(hi[2] - c_cal.center_x); c_cal.scale_x_neg = scale(c_cal.center_x - lo[2]);
      c_cal.scale_y_pos = scale(hi[3] - c_cal.center_y); c_cal.scale_y_neg = scale(c_cal.center_y - lo[3]);
      measuring = 0; dirty = true;
      message = "Range measured: each direction now reaches the edge.";
    }
  }
  const float half = width * 0.5f - 4;
  if (measuring == 1) ImGui::TextColored(kAccentV, "Leave both sticks alone... %.1f s", measure_until - now);
  else if (measuring == 2) ImGui::TextColored(kAccentV, "Roll both sticks around their edges... %.1f s", measure_until - now);
  else {
    if (button("Set the centre", half, fresh)) { measuring = 1; measure_until = now + 1.0; count = 0; for (double& s : sum) s = 0; }
    ImGui::SameLine(0, 8);
    if (button("Measure the range", half, fresh)) {
      measuring = 2; measure_until = now + 5.0;
      for (int i = 0; i < 4; ++i) lo[i] = hi[i] = raw[i];
    }
  }
  if (!message.empty()) muted("%s", message.c_str());
  // ---- the numbers
  auto sliders = [&](const char* name, android_host::StickCalibration& cal) {
    heading(name);
    ImGui::PushID(name);
    ImGui::SetNextItemWidth(width * 0.6f);
    dirty |= ImGui::SliderFloat("Deadzone", &cal.deadzone, 0.0f, 0.4f, "%.2f");
    ImGui::SetNextItemWidth(width * 0.6f);
    dirty |= ImGui::SliderFloat("Response curve", &cal.curve, 1.0f, 3.0f, "%.2f");
    ImGui::SetNextItemWidth(width * 0.6f);
    dirty |= ImGui::SliderFloat("Largest output", &cal.cap, 0.3f, 1.0f, "%.2f");
    float scale = (cal.scale_x_pos + cal.scale_x_neg + cal.scale_y_pos + cal.scale_y_neg) * 0.25f;
    ImGui::SetNextItemWidth(width * 0.6f);
    if (ImGui::SliderFloat("Outward scale", &scale, 0.5f, 8.0f, "%.2f")) {
      cal.scale_x_pos = cal.scale_x_neg = cal.scale_y_pos = cal.scale_y_neg = scale;
      dirty = true;
    }
    ImGui::PopID();
  };
  sliders("Stick", main_cal);
  sliders("C-stick", c_cal);
  if (dirty) { android_host::set_builtin_calibration(main_cal, c_cal); }
  ImGui::Dummy(ImVec2(0, 6));
  const float third = (width - 16) / 3.0f;
  if (button("Save", third, dirty)) {
    message = android_host::save_builtin_calibration() ? "Saved: builtin-sticks.ini." : "Could not save.";
    dirty = false;
  }
  ImGui::SameLine(0, 8);
  if (button("Undo", third)) { android_host::set_builtin_calibration(main_cal, c_cal); loaded = false; dirty = false; message.clear(); }
  ImGui::SameLine(0, 8);
  if (button("Defaults", third)) {
    android_host::default_builtin_calibration(main_cal, c_cal);
    android_host::set_builtin_calibration(main_cal, c_cal);
    dirty = true;
    message = "slippi-android's defaults (Save keeps them).";
  }
  muted("Changes apply at once; Save keeps them for the next start.");
}
#endif

}  // namespace

// ======================================================================================

bool available() { return true; }

void replay_saved(const std::string& path) {
  {
    std::lock_guard<std::mutex> lock(g_replays_mutex);
    g_replays_stale = true;
    g_last_saved = path;
  }
  worker().post([path] {
    launcher::replay::Info info = launcher::replay::inspect(fs::u8path(path), true);
    std::lock_guard<std::mutex> lock(g_replays_mutex);
    g_replay_stats[path] = info;
    if (info.valid) {
      g_session_games.push_back(std::move(info));
      if (g_session_games.size() > 30) g_session_games.erase(g_session_games.begin());
    }
  });
}

void note_frame(const gx::Frame& frame) {
  g_seen.hud = frame.hud_players;
  g_seen.names = frame.player_names;
  g_seen.scene_major = frame.scene_major;
  g_seen.scene_minor = frame.scene_minor;
  g_seen.match = gx::frame_in_match(frame);
}

float ui_scale(float density) {
  load_size();
  return std::clamp(density * 0.72f * g_size_choice, 1.0f, 5.0f);
}

void set_area(ImVec2 origin, ImVec2 size) { g_area_origin = origin; g_area_size = size; }

Begin begin(bool panel_opened) {
  load_size();
  static bool page_from_env = false;
  if (!page_from_env) {
    page_from_env = true;
    if (const char* p = std::getenv("MELEE_COMPANION_PAGE")) {
      static const char* const names[kPageCount] = {"settings", "online", "training", "replays", "room", "sticks"};
      for (int i = 0; i < kPageCount; ++i) if (std::strcmp(p, names[i]) == 0) g_page = i;
    }
    if (const char* r = std::getenv("MELEE_COMPANION_REPLAY")) g_selected_replay = r;   // tests: one replay's page
  }
  Begin b;
  const ImVec2 origin = area_origin();
  const ImVec2 display = area_size();
  // Follow the game: the Settings page when the controller opens the panel, Training on entering
  // Training, Replays when a replay starts playing.
  if (panel_opened && g_page != kSettings) { g_page_before_settings = g_page; g_page = kSettings; }
  const int scene = g_seen.scene_major;
  if (scene != g_last_scene) {
    if (scene == (int)kTrainingScene) g_page = kTraining;
    g_last_scene = scene;
  }
  static bool replay_was = false;
  const bool replay_now = replay_control::status().active;
  if (replay_now && !replay_was) g_page = kReplays;
  replay_was = replay_now;
  if (g_page == kSticks && !sticks_page_available()) g_page = kOnline;
  // What the pages need fed while they show.
  training_overlay::set_wanted(g_page == kTraining);
  lcancel::set_history_wanted(g_page == kTraining);

  // ---- the rail
  const float rail = std::clamp(display.x * 0.17f, 84.0f, 130.0f);
  ImGui::SetNextWindowPos(origin);
  ImGui::SetNextWindowSize(ImVec2(rail, display.y));
  ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.055f, 0.063f, 0.086f, 1.0f));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(6, 6));
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(4, 6));
  ImGui::Begin("##companion_rail", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoMove |
                                               ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoScrollWithMouse);
  struct Entry { int page; const char* label; };
  Entry entries[kPageCount] = {{kSettings, "Settings"}, {kOnline, "Online"}, {kTraining, "Training"},
                               {kReplays, "Replays"}, {kRoom, "Room"}, {kSticks, "Sticks"}};
  int shown = 0;
  for (const Entry& e : entries) if (e.page != kSticks || sticks_page_available()) ++shown;
  const float size_row = 44.0f;
  const float h = std::max(48.0f, (display.y - 12 - size_row - 6.0f * (shown + 1)) / shown);
  for (const Entry& e : entries) {
    if (e.page == kSticks && !sticks_page_available()) continue;
    const bool on = g_page == e.page;
    ImGui::PushStyleColor(ImGuiCol_Button, on ? kAccentV : ImVec4(0.11f, 0.12f, 0.16f, 1));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, on ? kAccentV : ImVec4(0.16f, 0.17f, 0.22f, 1));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, kAccentV);
    if (ImGui::Button(e.label, ImVec2(rail - 12, h))) {
      if (e.page == kSettings && g_page != kSettings) g_page_before_settings = g_page;
      if (e.page == kReplays) {   // the list read again whenever the page is opened
        std::lock_guard<std::mutex> lock(g_replays_mutex);
        g_replays_stale = true;
      }
      g_page = e.page;
    }
    ImGui::PopStyleColor(3);
  }
  // The size of everything on this screen.
  ImGui::SetCursorPosY(display.y - size_row - 6);
  const float half = (rail - 16) * 0.5f;
  if (ImGui::Button("A-", ImVec2(half, size_row))) { g_size_choice = std::max(0.6f, g_size_choice - 0.1f); save_size(); }
  ImGui::SameLine(0, 4);
  if (ImGui::Button("A+", ImVec2(half, size_row))) { g_size_choice = std::min(1.6f, g_size_choice + 0.1f); save_size(); }
  ImGui::End();
  ImGui::PopStyleVar(2);
  ImGui::PopStyleColor();
  b.page = (Page)g_page;
  b.page_min = ImVec2(origin.x + rail, origin.y);
  b.page_max = ImVec2(origin.x + display.x, origin.y + display.y);
  return b;
}

void leave_settings() { if (g_page == kSettings) g_page = g_page_before_settings; }

Result page(gx::RenderOptions& options) {
  Result result;
  if (g_page == kSettings) return result;
  const ImVec2 origin = area_origin();
  const ImVec2 display = area_size();
  const float rail = std::clamp(display.x * 0.17f, 84.0f, 130.0f);
  ImGui::SetNextWindowPos(ImVec2(origin.x + rail, origin.y));
  ImGui::SetNextWindowSize(ImVec2(display.x - rail, display.y));
  ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.075f, 0.083f, 0.11f, 1.0f));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14, 10));
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8, 8));
  ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 6.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_GrabMinSize, 28.0f);
  ImGui::Begin("##companion_page", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoMove |
                                               ImGuiWindowFlags_NoBringToFrontOnFocus);
  const np::Snapshot sn = np::snapshot();
  switch (g_page) {
    case kOnline: page_online(sn); break;
    case kTraining: result = page_training(sn); break;
    case kReplays: page_replays(); break;
    case kRoom: result = page_room(sn, options); break;
#ifdef __ANDROID__
    case kSticks: page_sticks(); break;
#endif
    default: break;
  }
  ImGui::Dummy(ImVec2(0, 24));   // room under the last row for a thumb
  ImGui::End();
  ImGui::PopStyleVar(4);
  ImGui::PopStyleColor();
  return result;
}

// ---- touch: taps, drags on sliders, and a drag anywhere else scrolls (companion_input.h)
namespace {
struct TouchState {
  bool down = false, scrolling = false, lift_pending = false;
  int pointer = -1;
  ImVec2 start, last;
  ImGuiWindow* window = nullptr;
  float scroll_start = 0;
};
TouchState g_touch;
ImGuiWindow* scrollable(ImGuiWindow* w) {
  for (; w; w = w->ParentWindow)
    if (w->ScrollMax.y > 0.5f) return w;
  return nullptr;
}
}  // namespace

void feed_touches(const Touch* touches, size_t count, float scale) {
  ImGuiIO& io = ImGui::GetIO();
  ImGuiContext& g = *ImGui::GetCurrentContext();
  if (g_touch.lift_pending && !g_touch.down) {
    // Touch has no hover: once the lift has been seen, nothing stays under the "pointer".
    io.AddMouseSourceEvent(ImGuiMouseSource_TouchScreen);
    io.AddMousePosEvent(-FLT_MAX, -FLT_MAX);
    g_touch.lift_pending = false;
  }
  for (size_t i = 0; i < count; ++i) {
    const Touch& t = touches[i];
    const ImVec2 p(t.x / scale, t.y / scale);
    io.AddMouseSourceEvent(ImGuiMouseSource_TouchScreen);
    switch (t.action) {
      case 0:   // down (a second finger is ignored)
        if (g_touch.down) break;
        g_touch = TouchState{};
        g_touch.down = true; g_touch.pointer = t.pointer; g_touch.start = g_touch.last = p;
        io.AddMousePosEvent(p.x, p.y);
        io.AddMouseButtonEvent(0, true);
        break;
      case 1:   // move
        if (!g_touch.down || t.pointer != g_touch.pointer) break;
        g_touch.last = p;
        if (!g_touch.scrolling && std::fabs(p.y - g_touch.start.y) > 12.0f && g.ActiveId == 0) {
          // Not on a slider or another control that takes drags: the page scrolls, and the press is
          // let go away from what it touched so it does not click.
          if (ImGuiWindow* w = scrollable(g.HoveredWindow)) {
            g_touch.scrolling = true; g_touch.window = w; g_touch.scroll_start = w->Scroll.y;
            io.AddMousePosEvent(-FLT_MAX, -FLT_MAX);
            io.AddMouseButtonEvent(0, false);
          }
        }
        if (g_touch.scrolling) {
          if (g_touch.window) ImGui::SetScrollY(g_touch.window, std::clamp(g_touch.scroll_start - (p.y - g_touch.start.y), 0.0f, g_touch.window->ScrollMax.y));
        } else {
          io.AddMousePosEvent(p.x, p.y);
        }
        break;
      case 2:   // up
      case 3:   // cancel
        if (!g_touch.down || (t.action == 2 && t.pointer != g_touch.pointer)) break;
        if (!g_touch.scrolling) {
          if (t.action == 3) io.AddMousePosEvent(-FLT_MAX, -FLT_MAX);
          else io.AddMousePosEvent(p.x, p.y);
          io.AddMouseButtonEvent(0, false);
        }
        g_touch.down = false;
        g_touch.lift_pending = true;
        break;
      default: break;
    }
  }
  // A scrolled window that went away (another page) is not touched again.
  if (g_touch.window && !ImGui::FindWindowByID(g_touch.window->ID)) g_touch.window = nullptr;
}

}  // namespace companion

#endif  // MELEE_COMPANION
