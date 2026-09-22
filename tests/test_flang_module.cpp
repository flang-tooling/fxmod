// Round-trips a synthesized Flang-style module through the reader: a good
// checksum must be accepted and the body handed back verbatim as Fortran
// source (no re-parsing); a corrupted checksum must be refused. Also
// exercises the C API (fxmod_wrap_flang_header) end-to-end against the
// C++ reader (fxmod::ModuleFile::open), as a cross-check between the two
// public surfaces.
#include <fxmod/fxmod.h>
#include <fxmod/module.hpp>

#include <filesystem>

#include "test_util.hpp"

int main() {
  std::filesystem::path tmp =
      std::filesystem::temp_directory_path() / "fxmod-flang-test";
  std::filesystem::create_directories(tmp);

  const std::string body =
      "module foo\n implicit none\n integer(4) :: x\nend module foo\n";

  char *bytes = nullptr;
  size_t len = 0;
  fxmod_error *err = nullptr;
  fxmod_status st = fxmod_wrap_flang_header(body.data(), body.size(),
                                               &bytes, &len, &err);
  FXMOD_CHECK_EQ(st, FXMOD_OK);
  FXMOD_CHECK(bytes != nullptr);
  FXMOD_CHECK(err == nullptr);

  std::string good_path = (tmp / "foo.mod").string();
  write_file(good_path, std::string(bytes, len));

  {
    fxmod::ModuleFile mf = fxmod::ModuleFile::open(good_path);
    FXMOD_CHECK(mf.family() == fxmod::Family::Flang);
    FXMOD_CHECK_EQ(mf.version(), std::string("v1"));
    fxmod::EmitResult r = mf.emit_fortran_source();
    FXMOD_CHECK_EQ(r.source, body);
    FXMOD_CHECK(r.problems.empty());
  }

  // Corrupt one byte inside the body without touching the header's
  // checksum: must be refused, not silently accepted.
  {
    std::string corrupted(bytes, len);
    std::size_t body_start = corrupted.size() - body.size();
    corrupted[body_start + 5] = corrupted[body_start + 5] == 'x' ? 'y' : 'x';
    std::string bad_path = (tmp / "corrupt.mod").string();
    write_file(bad_path, corrupted);

    bool threw = false;
    try {
      fxmod::ModuleFile::open(bad_path);
    } catch (const fxmod::Error &) {
      threw = true;
    }
    FXMOD_CHECK(threw);
  }

  fxmod_free_string(bytes);

  // Same good file, opened through the C API instead, as a smoke test of
  // that surface too.
  {
    fxmod_module *m = nullptr;
    fxmod_error *open_err = nullptr;
    FXMOD_CHECK_EQ(fxmod_open(good_path.c_str(), &m, &open_err), FXMOD_OK);
    FXMOD_CHECK(m != nullptr);
    FXMOD_CHECK_EQ(fxmod_module_family(m), FXMOD_FAMILY_FLANG);

    char *text = nullptr;
    FXMOD_CHECK_EQ(fxmod_module_emit_fortran_source(m, 1, &text, nullptr,
                                                        nullptr, nullptr),
                    FXMOD_OK);
    FXMOD_CHECK(text != nullptr);
    FXMOD_CHECK_EQ(std::string(text), body);
    fxmod_free_string(text);
    fxmod_module_free(m);
  }

  std::puts("test_flang_module: OK");
  return 0;
}
