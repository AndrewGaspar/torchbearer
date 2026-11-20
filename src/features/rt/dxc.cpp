// The game's own dxcompiler.dll, used in-process (dxc.h).
#include "features/rt/dxc.h"

#include <mutex>

#include "core/log.h"

namespace tb::rt {
namespace {

const GUID kClsidCompiler = {0x73e22d93, 0xe6ce, 0x47f3, {0xb5, 0xbf, 0xf0, 0x66, 0x4f, 0x39, 0xc1, 0xb0}};
const GUID kClsidLibrary = {0x6245d6af, 0x66e0, 0x48fd, {0x80, 0xb4, 0x4d, 0x27, 0x17, 0x96, 0x74, 0x8c}};
const GUID kIidLibrary = {0xe5204dc7, 0xd18c, 0x4c3c, {0xbd, 0xfb, 0x85, 0x16, 0x73, 0x98, 0x0f, 0xe7}};
const GUID kIidCompiler = {0x8c210bf3, 0x011f, 0x4422, {0x8d, 0x70, 0x6f, 0x9a, 0xcb, 0x8d, 0xb6, 0x17}};

using CreateInstance_t = HRESULT(WINAPI*)(REFCLSID, REFIID, void**);

std::once_flag g_once;
std::mutex g_mu;  // dxcompiler objects are not thread-safe
IDxcLibrary* g_lib = nullptr;
IDxcCompiler* g_cc = nullptr;
std::string g_why;

void load() {
  std::call_once(g_once, [] {
    // The exe's folder first (the game's copy), then the default search order.
    wchar_t path[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring p(path, n);
    p = p.substr(0, p.find_last_of(L"\\/") + 1) + L"dxcompiler.dll";
    HMODULE m = LoadLibraryW(p.c_str());
    if (!m) m = LoadLibraryW(L"dxcompiler.dll");
    if (!m) {
      g_why = "dxcompiler.dll not found";
      return;
    }
    auto ci = reinterpret_cast<CreateInstance_t>(GetProcAddress(m, "DxcCreateInstance"));
    if (!ci) {
      g_why = "no DxcCreateInstance";
      return;
    }
    if (FAILED(ci(kClsidLibrary, kIidLibrary, (void**)&g_lib)) || FAILED(ci(kClsidCompiler, kIidCompiler, (void**)&g_cc))) {
      g_why = "DxcCreateInstance failed";
      g_lib = nullptr;
      g_cc = nullptr;
      return;
    }
    TB_LOG("rt: dxcompiler.dll loaded (%p)", (void*)m);
  });
}

std::string blob_text(IDxcBlob* b) {
  if (!b) return {};
  const char* p = static_cast<const char*>(b->GetBufferPointer());
  size_t n = b->GetBufferSize();
  while (n && p[n - 1] == 0) n--;
  return std::string(p, n);
}

}  // namespace

bool dxc_available(std::string* why) {
  load();
  if (why) *why = g_why;
  return g_cc && g_lib;
}

bool dxc_compile(const char* hlsl, const wchar_t* entry, const wchar_t* target, std::vector<uint8_t>& out,
                 std::string& err) {
  if (!dxc_available(&err)) return false;
  std::lock_guard lk(g_mu);
  IDxcBlobEncoding* src = nullptr;
  if (FAILED(g_lib->CreateBlobWithEncodingFromPinned(hlsl, UINT32(strlen(hlsl)), CP_UTF8, &src)) || !src) {
    err = "CreateBlobWithEncodingFromPinned failed";
    return false;
  }
  LPCWSTR args[] = {L"-O3"};
  IDxcOperationResult* res = nullptr;
  HRESULT hr = g_cc->Compile(src, L"tb_rt.hlsl", entry, target, args, 1, nullptr, 0, nullptr, &res);
  src->Release();
  if (FAILED(hr) || !res) {
    err = "Compile call failed";
    return false;
  }
  HRESULT st = E_FAIL;
  res->GetStatus(&st);
  IDxcBlobEncoding* eb = nullptr;
  if (SUCCEEDED(res->GetErrorBuffer(&eb)) && eb) {
    err = blob_text(eb);
    eb->Release();
  }
  bool ok = false;
  if (SUCCEEDED(st)) {
    IDxcBlob* b = nullptr;
    if (SUCCEEDED(res->GetResult(&b)) && b) {
      auto* p = static_cast<const uint8_t*>(b->GetBufferPointer());
      out.assign(p, p + b->GetBufferSize());
      b->Release();
      ok = !out.empty();
    }
  }
  res->Release();
  return ok;
}

bool dxc_disassemble(const void* code, size_t size, std::string& out) {
  if (!dxc_available()) return false;
  std::lock_guard lk(g_mu);
  IDxcBlobEncoding* src = nullptr;
  if (FAILED(g_lib->CreateBlobWithEncodingFromPinned(code, UINT32(size), 0, &src)) || !src) return false;
  IDxcBlobEncoding* txt = nullptr;
  HRESULT hr = g_cc->Disassemble(src, &txt);
  src->Release();
  if (FAILED(hr) || !txt) return false;
  out = blob_text(txt);
  txt->Release();
  return true;
}

}  // namespace tb::rt
