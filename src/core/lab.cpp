#include "core/lab.h"

#include <windows.h>

#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>

#include "core/log.h"

namespace tb {
namespace {

std::wstring g_lab;  // Z:\lab, empty outside the lab
std::mutex g_mu;
std::map<std::string, LabVerb> g_verbs;

std::wstring widen(const std::string& s) {
  int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
  std::wstring w(n, 0);
  MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
  return w;
}
std::string narrow(const std::wstring& w) {
  int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), nullptr, 0, nullptr, nullptr);
  std::string s(n, 0);
  WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), s.data(), n, nullptr, nullptr);
  return s;
}

void write_atomic(const std::wstring& path, const std::string& body) {
  std::wstring tmp = path + L".tmp";
  {
    std::ofstream f(std::filesystem::path(tmp), std::ios::binary | std::ios::trunc);
    f << body;
  }
  MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING);
}

// Minimal reader for {"id":"..","verb":"..","args":["..",..]}: string values only.
struct Req {
  std::string id, verb;
  std::vector<std::string> args;
};

bool read_jstr(const std::string& s, size_t& i, std::string& out) {
  while (i < s.size() && s[i] != '"') i++;
  if (i >= s.size()) return false;
  i++;
  out.clear();
  while (i < s.size() && s[i] != '"') {
    if (s[i] == '\\' && i + 1 < s.size()) {
      i++;
      char c = s[i];
      out += c == 'n' ? '\n' : c == 't' ? '\t' : c;
    } else {
      out += s[i];
    }
    i++;
  }
  i++;
  return true;
}

bool parse_req(const std::string& s, Req& r) {
  size_t i = 0;
  std::string key;
  while (read_jstr(s, i, key)) {
    while (i < s.size() && s[i] != ':') i++;
    i++;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\n')) i++;
    if (i >= s.size()) break;
    if (key == "args" && s[i] == '[') {
      size_t end = s.find(']', i);
      std::string v;
      while (i < end) {
        size_t q = s.find('"', i);
        if (q == std::string::npos || q > end) break;
        i = q;
        read_jstr(s, i, v);
        r.args.push_back(v);
      }
      i = end + 1;
    } else if (s[i] == '"') {
      std::string v;
      read_jstr(s, i, v);
      if (key == "id") r.id = v;
      if (key == "verb") r.verb = v;
    } else {
      // number or other scalar
      size_t e = s.find_first_of(",}", i);
      std::string v = s.substr(i, e - i);
      if (key == "id") r.id = v;
      i = e;
    }
  }
  return !r.verb.empty();
}

void serve(const std::wstring& file) {
  std::ifstream f(std::filesystem::path(file), std::ios::binary);
  std::stringstream ss;
  ss << f.rdbuf();
  f.close();
  DeleteFileW(file.c_str());
  Req r;
  if (!parse_req(ss.str(), r)) {
    TB_LOG("lab: unreadable request %s", narrow(file).c_str());
    return;
  }
  LabVerb fn;
  {
    std::lock_guard lk(g_mu);
    auto it = g_verbs.find(r.verb);
    if (it != g_verbs.end()) fn = it->second;
  }
  std::string body;
  if (!fn) {
    body = "{\"ok\":false,\"error\":" + json_str("unknown verb " + r.verb) + "}";
  } else {
    bool ok = true;
    std::string res = fn(r.args, ok);
    if (ok)
      body = "{\"ok\":true,\"result\":" + (res.empty() ? std::string("null") : res) + "}";
    else
      body = "{\"ok\":false,\"error\":" + (res.empty() ? json_str("failed") : res) + "}";
  }
  std::string idq = r.id;
  write_atomic(g_lab + L"\\rsp\\" + widen(idq) + L".json", body);
}

DWORD WINAPI lab_thread(void*) {
  std::wstring dir = g_lab + L"\\req\\";
  for (;;) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"*.json").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
      do serve(dir + fd.cFileName);
      while (FindNextFileW(h, &fd));
      FindClose(h);
    }
    Sleep(50);
  }
}

}  // namespace

std::string json_str(const std::string& s) {
  std::string o = "\"";
  for (char c : s) {
    if (c == '"' || c == '\\') o += '\\', o += c;
    else if (c == '\n') o += "\\n";
    else if (static_cast<unsigned char>(c) < 0x20) o += ' ';
    else o += c;
  }
  return o + "\"";
}

void lab_init() {
  wchar_t buf[MAX_PATH];
  DWORD n = GetEnvironmentVariableW(L"QUIVER_LAB", buf, MAX_PATH);
  if (n == 0 || n >= MAX_PATH) return;
  g_lab.assign(buf, n);
  CreateDirectoryW((g_lab + L"\\req").c_str(), nullptr);
  CreateDirectoryW((g_lab + L"\\rsp").c_str(), nullptr);
  TB_LOG("lab: channel at %s", narrow(g_lab).c_str());
  lab_verb("ping", [](const std::vector<std::string>&, bool&) { return json_str("pong"); });
  CloseHandle(CreateThread(nullptr, 0, lab_thread, nullptr, 0, nullptr));
}

bool lab_active() { return !g_lab.empty(); }

void lab_state(const std::string& state, const std::vector<std::string>& kv) {
  if (g_lab.empty()) return;
  std::string body = state + "\n";
  for (auto& l : kv) body += l + "\n";
  write_atomic(g_lab + L"\\state", body);
}

void lab_verb(const std::string& verb, LabVerb fn) {
  std::lock_guard lk(g_mu);
  g_verbs[verb] = std::move(fn);
}

}  // namespace tb
