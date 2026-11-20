#pragma once
// Minimal DirectX Shader Compiler (dxcompiler.dll) interfaces, declared here because mingw-w64 has
// no dxcapi.h. The game ships Microsoft's dxcompiler.dll next to its exe (it loads it itself to
// strip debug parts from its precompiled DXIL, static: strings in Game_f_x64_EOS.exe), so the
// raytracing probe can compile its test shaders and disassemble the game's DXIL in-process.
// Layouts follow dxcapi.h (IDxcBlob, IDxcBlobEncoding, IDxcLibrary, IDxcOperationResult,
// IDxcCompiler), in declaration order.
#include <windows.h>
#include <unknwn.h>

#include <cstdint>
#include <string>
#include <vector>

namespace tb::rt {

struct IDxcBlob : IUnknown {
  virtual LPVOID STDMETHODCALLTYPE GetBufferPointer() = 0;
  virtual SIZE_T STDMETHODCALLTYPE GetBufferSize() = 0;
};
struct IDxcBlobEncoding : IDxcBlob {
  virtual HRESULT STDMETHODCALLTYPE GetEncoding(BOOL* known, UINT32* codePage) = 0;
};
struct IDxcIncludeHandler;
struct DxcDefine {
  LPCWSTR Name;
  LPCWSTR Value;
};
struct IDxcLibrary : IUnknown {
  virtual HRESULT STDMETHODCALLTYPE SetMalloc(IMalloc*) = 0;
  virtual HRESULT STDMETHODCALLTYPE CreateBlobFromBlob(IDxcBlob*, UINT32, UINT32, IDxcBlob**) = 0;
  virtual HRESULT STDMETHODCALLTYPE CreateBlobFromFile(LPCWSTR, UINT32*, IDxcBlobEncoding**) = 0;
  virtual HRESULT STDMETHODCALLTYPE CreateBlobWithEncodingFromPinned(LPCVOID, UINT32, UINT32,
                                                                     IDxcBlobEncoding**) = 0;
  virtual HRESULT STDMETHODCALLTYPE CreateBlobWithEncodingOnHeapCopy(LPCVOID, UINT32, UINT32,
                                                                     IDxcBlobEncoding**) = 0;
  virtual HRESULT STDMETHODCALLTYPE CreateBlobWithEncodingOnMalloc(LPCVOID, IMalloc*, UINT32, UINT32,
                                                                   IDxcBlobEncoding**) = 0;
  virtual HRESULT STDMETHODCALLTYPE CreateIncludeHandler(IDxcIncludeHandler**) = 0;
  virtual HRESULT STDMETHODCALLTYPE CreateStreamFromBlobReadOnly(IDxcBlob*, IStream**) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetBlobAsUtf8(IDxcBlob*, IDxcBlobEncoding**) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetBlobAsUtf16(IDxcBlob*, IDxcBlobEncoding**) = 0;
};
struct IDxcOperationResult : IUnknown {
  virtual HRESULT STDMETHODCALLTYPE GetStatus(HRESULT*) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetResult(IDxcBlob**) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetErrorBuffer(IDxcBlobEncoding**) = 0;
};
struct IDxcCompiler : IUnknown {
  virtual HRESULT STDMETHODCALLTYPE Compile(IDxcBlob* src, LPCWSTR name, LPCWSTR entry, LPCWSTR target,
                                            LPCWSTR* args, UINT32 nargs, const DxcDefine* defs, UINT32 ndefs,
                                            IDxcIncludeHandler* inc, IDxcOperationResult** out) = 0;
  virtual HRESULT STDMETHODCALLTYPE Preprocess(IDxcBlob*, LPCWSTR, LPCWSTR*, UINT32, const DxcDefine*, UINT32,
                                               IDxcIncludeHandler*, IDxcOperationResult**) = 0;
  virtual HRESULT STDMETHODCALLTYPE Disassemble(IDxcBlob* src, IDxcBlobEncoding** out) = 0;
};

// The game's dxcompiler.dll (loaded from the exe folder). False if it is missing.
bool dxc_available(std::string* why = nullptr);
// Compile HLSL text. On failure returns false and puts the compiler's messages in `err`.
bool dxc_compile(const char* hlsl, const wchar_t* entry, const wchar_t* target, std::vector<uint8_t>& out,
                 std::string& err);
// DXIL container -> LLVM assembly text (IDxcCompiler::Disassemble).
bool dxc_disassemble(const void* code, size_t size, std::string& out);

}  // namespace tb::rt
