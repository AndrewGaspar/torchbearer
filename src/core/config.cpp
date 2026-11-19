#include "core/config.h"

#include <windows.h>

#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <vector>

#include "core/log.h"
#include "core/module.h"

namespace tb {
namespace {

struct Listener {
  std::string prefix;
  std::function<void(const std::string&, const std::string&)> fn;
};

std::mutex g_mu;
std::map<std::string, std::string> g_values;
std::vector<Listener> g_listeners;
std::atomic<unsigned> g_gen{0};
std::wstring g_path;
FILETIME g_mtime{};

std::string trim(const std::string& s) {
  size_t a = s.find_first_not_of(" \t\r\n");
  if (a == std::string::npos) return "";
  size_t b = s.find_last_not_of(" \t\r\n");
  return s.substr(a, b - a + 1);
}

std::map<std::string, std::string> parse(const std::wstring& path) {
  std::map<std::string, std::string> out;
  std::ifstream f{std::filesystem::path(path)};
  std::string line, section;
  while (std::getline(f, line)) {
    size_t c = line.find_first_of(";#");
    if (c != std::string::npos) line.resize(c);
    line = trim(line);
    if (line.empty()) continue;
    if (line.front() == '[' && line.back() == ']') {
      section = trim(line.substr(1, line.size() - 2));
      continue;
    }
    size_t eq = line.find('=');
    if (eq == std::string::npos) continue;
    std::string key = trim(line.substr(0, eq));
    std::string val = trim(line.substr(eq + 1));
    if (val.size() >= 2 && val.front() == '"' && val.back() == '"') val = val.substr(1, val.size() - 2);
    out[section.empty() ? key : section + "." + key] = val;
  }
  return out;
}

void notify(const std::vector<std::pair<std::string, std::string>>& changed) {
  std::vector<Listener> ls;
  {
    std::lock_guard lk(g_mu);
    ls = g_listeners;
  }
  for (auto& [id, v] : changed)
    for (auto& l : ls)
      if (id.compare(0, l.prefix.size(), l.prefix) == 0) l.fn(id, v);
}

bool file_mtime(FILETIME* out) {
  WIN32_FILE_ATTRIBUTE_DATA d;
  if (!GetFileAttributesExW(g_path.c_str(), GetFileExInfoStandard, &d)) return false;
  *out = d.ftLastWriteTime;
  return true;
}

// The file is authoritative for the keys it changed since the last read.
std::map<std::string, std::string> g_file_last;

void reload(bool initial) {
  auto fresh = parse(g_path);
  std::vector<std::pair<std::string, std::string>> changed;
  {
    std::lock_guard lk(g_mu);
    for (auto& [k, v] : fresh) {
      auto prev = g_file_last.find(k);
      bool file_changed = prev == g_file_last.end() || prev->second != v;
      if (initial || file_changed) {
        auto cur = g_values.find(k);
        if (cur == g_values.end() || cur->second != v) {
          g_values[k] = v;
          changed.emplace_back(k, v);
        }
      }
    }
    g_file_last = std::move(fresh);
  }
  g_gen++;
  TB_LOG("config %s (gen %u, %zu changed)", initial ? "loaded" : "reloaded", g_gen.load(),
         changed.size());
  for (auto& [k, v] : changed) TB_LOG("  %s = %s", k.c_str(), v.c_str());
  notify(changed);
}

DWORD WINAPI poll_thread(void*) {
  for (;;) {
    Sleep(250);
    FILETIME m;
    if (file_mtime(&m) && CompareFileTime(&m, &g_mtime) != 0) {
      g_mtime = m;
      Sleep(30);  // let an editor finish writing
      reload(false);
    }
  }
}

}  // namespace

void config_init() {
  g_path = game_dir() + L"\\torchbearer.ini";
  file_mtime(&g_mtime);
  reload(true);
  CloseHandle(CreateThread(nullptr, 0, poll_thread, nullptr, 0, nullptr));
}

std::string cfg_str(const std::string& id, const std::string& def) {
  std::lock_guard lk(g_mu);
  auto it = g_values.find(id);
  return it == g_values.end() ? def : it->second;
}

double cfg_num(const std::string& id, double def) {
  std::string s = cfg_str(id);
  if (s.empty()) return def;
  char* end = nullptr;
  double v = strtod(s.c_str(), &end);
  return end == s.c_str() ? def : v;
}

bool cfg_bool(const std::string& id, bool def) {
  std::string s = cfg_str(id);
  if (s.empty()) return def;
  return s == "1" || s == "true" || s == "on" || s == "yes";
}

void cfg_set(const std::string& id, const std::string& value, const char* origin) {
  {
    std::lock_guard lk(g_mu);
    auto it = g_values.find(id);
    if (it != g_values.end() && it->second == value) return;
    g_values[id] = value;
  }
  g_gen++;
  TB_LOG("config set by %s: %s = %s (gen %u)", origin, id.c_str(), value.c_str(), g_gen.load());
  notify({{id, value}});
}

void cfg_listen(const std::string& prefix,
                std::function<void(const std::string&, const std::string&)> fn) {
  std::lock_guard lk(g_mu);
  g_listeners.push_back({prefix, std::move(fn)});
}

unsigned cfg_generation() { return g_gen.load(); }

}  // namespace tb
