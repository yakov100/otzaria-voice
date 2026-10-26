// A thin, header-only wrapper over the ONNX Runtime C API for the small
// models (the diacritizer, the light voice): loading the runtime, opening a
// session, making tensors over caller-owned memory and running. The
// narrator keeps its own copy of the same pieces, with the run options its
// decoder watchdog needs.
#pragma once

#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "../../third_party/onnxruntime_c_api.h"
#include "platform.h"

namespace ort {

class Runtime {
 public:
  // Loads [library] (onnxruntime.dll / .so) and makes an environment.
  // Several Runtime objects in one process share ONNX Runtime's one
  // environment; each holds its own reference. Throws std::runtime_error.
  Runtime(const platform::Path& library, const char* logId) {
    dll_ = std::make_unique<platform::DynamicLibrary>(library);
    using GetApiBase = const OrtApiBase*(ORT_API_CALL*)();
    auto getBase = reinterpret_cast<GetApiBase>(dll_->symbol("OrtGetApiBase"));
    if (!getBase) throw std::runtime_error("OrtGetApiBase missing");
    for (uint32_t v = ORT_API_VERSION; v >= 11 && !api; v--) api = getBase()->GetApi(v);
    if (!api) throw std::runtime_error("ONNX Runtime too old");
    check(api->CreateEnv(ORT_LOGGING_LEVEL_ERROR, logId, &env));
    check(api->CreateCpuMemoryInfo(OrtArenaAllocator, OrtMemTypeDefault, &memory));
    check(api->GetAllocatorWithDefaultOptions(&allocator));
  }
  ~Runtime() {
    if (!api) return;
    if (memory) api->ReleaseMemoryInfo(memory);
    if (env) api->ReleaseEnv(env);
  }
  Runtime(const Runtime&) = delete;
  Runtime& operator=(const Runtime&) = delete;

  const OrtApi* api = nullptr;
  OrtEnv* env = nullptr;
  OrtMemoryInfo* memory = nullptr;
  OrtAllocator* allocator = nullptr;

  void check(OrtStatus* s) const {
    if (!s) return;
    std::string msg = api->GetErrorMessage(s);
    api->ReleaseStatus(s);
    throw std::runtime_error("ONNX Runtime: " + msg);
  }

  OrtSession* open(const platform::Path& path, int threads) const {
    OrtSessionOptions* opts = nullptr;
    check(api->CreateSessionOptions(&opts));
    struct Guard {
      const OrtApi* api;
      OrtSessionOptions* p;
      ~Guard() { api->ReleaseSessionOptions(p); }
    } guard{api, opts};
    check(api->SetIntraOpNumThreads(opts, threads));
    check(api->SetInterOpNumThreads(opts, 1));
    check(api->SetSessionGraphOptimizationLevel(opts, ORT_ENABLE_ALL));
    check(api->AddSessionConfigEntry(opts, "session.intra_op.allow_spinning", "0"));
    OrtSession* s = nullptr;
    check(api->CreateSession(env, path.c_str(), opts, &s));
    return s;
  }

  void close(OrtSession* s) const {
    if (s) api->ReleaseSession(s);
  }

  std::vector<std::string> names(OrtSession* s, bool inputs) const {
    size_t n = 0;
    check(inputs ? api->SessionGetInputCount(s, &n) : api->SessionGetOutputCount(s, &n));
    std::vector<std::string> out;
    for (size_t i = 0; i < n; i++) {
      char* name = nullptr;
      check(inputs ? api->SessionGetInputName(s, i, allocator, &name)
                   : api->SessionGetOutputName(s, i, allocator, &name));
      out.push_back(name);
      allocator->Free(allocator, name);
    }
    return out;
  }

  // A tensor over caller-owned memory (kept alive by the caller during run).
  template <typename T>
  OrtValue* tensor(T* data, size_t count, std::vector<int64_t> shape, ONNXTensorElementDataType type) const {
    OrtValue* v = nullptr;
    check(api->CreateTensorWithDataAsOrtValue(memory, data, count * sizeof(T), shape.data(), shape.size(),
                                              type, &v));
    return v;
  }

  std::vector<int64_t> shapeOf(OrtValue* v) const {
    OrtTensorTypeAndShapeInfo* info = nullptr;
    check(api->GetTensorTypeAndShape(v, &info));
    size_t dims = 0;
    OrtStatus* s = api->GetDimensionsCount(info, &dims);
    std::vector<int64_t> shape(dims);
    if (!s && dims) s = api->GetDimensions(info, shape.data(), dims);
    api->ReleaseTensorTypeAndShapeInfo(info);
    check(s);
    return shape;
  }

  template <typename T>
  T* dataOf(OrtValue* v) const {
    void* p = nullptr;
    check(api->GetTensorMutableData(v, &p));
    return static_cast<T*>(p);
  }

  // Runs [s]; the outputs are owned by the caller (release()).
  std::vector<OrtValue*> run(OrtSession* s, const std::vector<std::string>& inNames,
                             const std::vector<OrtValue*>& in, const std::vector<std::string>& outNames) const {
    std::vector<const char*> ip, op;
    for (auto& n : inNames) ip.push_back(n.c_str());
    for (auto& n : outNames) op.push_back(n.c_str());
    std::vector<OrtValue*> out(outNames.size(), nullptr);
    check(api->Run(s, nullptr, ip.data(), in.data(), in.size(), op.data(), op.size(), out.data()));
    return out;
  }

  void release(std::vector<OrtValue*>& values) const {
    for (auto*& v : values)
      if (v) {
        api->ReleaseValue(v);
        v = nullptr;
      }
  }

 private:
  std::unique_ptr<platform::DynamicLibrary> dll_;
};

// Releases two sets of values when it goes out of scope (inputs and outputs of a run).
struct ValueGuard {
  const Runtime& rt;
  std::vector<OrtValue*>& a;
  std::vector<OrtValue*>& b;
  ~ValueGuard() {
    rt.release(a);
    rt.release(b);
  }
};

}  // namespace ort
