#include <fxmod/fxmod.h>

#include <cstdlib>
#include <cstring>
#include <new>

#include <fxmod/module.hpp>

#include "flang/module.hpp" // for flang::wrap_module_bytes

struct fxmod_module {
  fxmod::ModuleFile mf;
};

struct fxmod_error {
  fxmod_status code;
  std::string message;
};

namespace {

char *dup_cstr(const std::string &s) {
  char *out = static_cast<char *>(std::malloc(s.size() + 1));
  if (out == nullptr)
    return nullptr;
  std::memcpy(out, s.data(), s.size());
  out[s.size()] = '\0';
  return out;
}

void set_error(fxmod_error **out_error, fxmod_status code,
                const std::string &message) {
  if (out_error == nullptr)
    return;
  *out_error = new (std::nothrow) fxmod_error{code, message};
}

// Runs `fn`, translating fxmod's C++ exceptions into a fxmod_status plus
// an optional fxmod_error. Every C API entry point below funnels its
// exception-throwing work through here so the C ABI boundary never lets a
// C++ exception escape.
template <typename Fn>
fxmod_status guard(fxmod_error **out_error, Fn &&fn) {
  try {
    fn();
    return FXMOD_OK;
  } catch (const fxmod::UnsupportedError &e) {
    set_error(out_error, FXMOD_UNSUPPORTED, e.what());
    return FXMOD_UNSUPPORTED;
  } catch (const std::exception &e) {
    set_error(out_error, FXMOD_ERROR, e.what());
    return FXMOD_ERROR;
  } catch (...) {
    set_error(out_error, FXMOD_ERROR, "unknown internal error");
    return FXMOD_ERROR;
  }
}

fxmod_family to_c_family(fxmod::Family f) {
  switch (f) {
  case fxmod::Family::Gfortran:
    return FXMOD_FAMILY_GFORTRAN;
  case fxmod::Family::Flang:
    return FXMOD_FAMILY_FLANG;
  case fxmod::Family::Ifx:
    return FXMOD_FAMILY_IFX;
  case fxmod::Family::Unknown:
    break;
  }
  return FXMOD_FAMILY_UNKNOWN;
}

} // namespace

extern "C" {

fxmod_status fxmod_error_code(const fxmod_error *err) {
  return err ? err->code : FXMOD_OK;
}

const char *fxmod_error_message(const fxmod_error *err) {
  return err ? err->message.c_str() : "";
}

void fxmod_error_free(fxmod_error *err) { delete err; }

void fxmod_free_string(char *s) { std::free(s); }

void fxmod_free_string_array(char **arr, size_t count) {
  if (arr == nullptr)
    return;
  for (size_t i = 0; i < count; ++i)
    std::free(arr[i]);
  std::free(arr);
}

fxmod_status fxmod_open(const char *path, fxmod_module **out_module,
                           fxmod_error **out_error) {
  if (out_module != nullptr)
    *out_module = nullptr;
  if (path == nullptr)
    return guard(out_error, [&] { throw fxmod::Error("path is NULL"); });

  fxmod_module *handle = nullptr;
  fxmod_status status = guard(out_error, [&] {
    handle = new fxmod_module{fxmod::ModuleFile::open(path)};
  });
  if (status == FXMOD_OK && out_module != nullptr)
    *out_module = handle;
  else
    delete handle;
  return status;
}

void fxmod_module_free(fxmod_module *module) { delete module; }

fxmod_family fxmod_module_family(const fxmod_module *module) {
  if (module == nullptr)
    return FXMOD_FAMILY_UNKNOWN;
  return to_c_family(module->mf.family());
}

const char *fxmod_module_version(const fxmod_module *module) {
  return module ? module->mf.version().c_str() : "";
}

const char *fxmod_module_name(const fxmod_module *module) {
  return module ? module->mf.name().c_str() : "";
}

fxmod_status fxmod_module_emit_fortran_source(
    const fxmod_module *module, int strict, char **out_text,
    char ***out_problems, size_t *out_problem_count, fxmod_error **out_error) {
  if (out_text != nullptr)
    *out_text = nullptr;
  if (out_problems != nullptr)
    *out_problems = nullptr;
  if (out_problem_count != nullptr)
    *out_problem_count = 0;

  if (module == nullptr)
    return guard(out_error, [&] { throw fxmod::Error("module is NULL"); });

  return guard(out_error, [&] {
    fxmod::EmitResult r = module->mf.emit_fortran_source(strict != 0);
    if (out_text != nullptr) {
      *out_text = dup_cstr(r.source);
      if (*out_text == nullptr)
        throw fxmod::Error("out of memory");
    }
    if (out_problems != nullptr && out_problem_count != nullptr &&
        !r.problems.empty()) {
      char **arr = static_cast<char **>(
          std::malloc(sizeof(char *) * r.problems.size()));
      if (arr == nullptr)
        throw fxmod::Error("out of memory");
      for (std::size_t i = 0; i < r.problems.size(); ++i)
        arr[i] = dup_cstr(r.problems[i]);
      *out_problems = arr;
      *out_problem_count = r.problems.size();
    }
  });
}

fxmod_status fxmod_wrap_flang_header(const char *body, size_t body_len,
                                        char **out_bytes, size_t *out_len,
                                        fxmod_error **out_error) {
  if (out_bytes != nullptr)
    *out_bytes = nullptr;
  if (out_len != nullptr)
    *out_len = 0;
  if (body == nullptr && body_len != 0)
    return guard(out_error,
                 [&] { throw fxmod::Error("body is NULL but body_len != 0"); });

  return guard(out_error, [&] {
    std::string text(body ? body : "", body_len);
    std::vector<std::uint8_t> bytes = fxmod::flang::wrap_module_bytes(text);
    if (out_bytes != nullptr) {
      char *buf = static_cast<char *>(std::malloc(bytes.size()));
      if (buf == nullptr)
        throw fxmod::Error("out of memory");
      std::memcpy(buf, bytes.data(), bytes.size());
      *out_bytes = buf;
    }
    if (out_len != nullptr)
      *out_len = bytes.size();
  });
}

} // extern "C"
