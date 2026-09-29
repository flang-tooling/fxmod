// `fxmod-cli wrap` converts a directory's foreign modules by compiling their
// translations, and a translation that USEs another converted module only
// compiles once that module's converted .mod exists. Directory order is no
// dependency order: here a_dep USEs z_base, which sorts after it. Both are
// synthetic Flang-format modules converted for gfortran, as in test_wrap.
#include <fxmod/fxmod.h>

#include "test_util.hpp"

#ifndef FXMOD_CLI_EXE
#error "FXMOD_CLI_EXE must be defined by the build"
#endif

namespace fs = std::filesystem;

namespace {

void write_flang_module(const fs::path &path, const std::string &body) {
  char *bytes = nullptr;
  size_t len = 0;
  FXMOD_CHECK_EQ(fxmod_wrap_flang_header(body.data(), body.size(), &bytes,
                                           &len, nullptr),
                 FXMOD_OK);
  write_file(path.string(), std::string(bytes, len));
  fxmod_free_string(bytes);
}

} // namespace

int main() {
  std::string gfortran = which("gfortran");
  if (gfortran.empty())
    skip("no gfortran binary on PATH");

  fs::path root = fresh_dir("fxmod-wrap-order-test");
  fs::path src_dir = root / "src";
  fs::path cache_dir = root / "cache";
  fs::create_directories(src_dir);

  write_flang_module(src_dir / "a_dep.mod",
                     "module a_dep\nuse z_base,only:base_t\nimplicit none\n"
                     "type(base_t)::b\nend module a_dep\n");
  write_flang_module(src_dir / "z_base.mod",
                     "module z_base\nimplicit none\ntype::base_t\n"
                     "integer(4)::v\nend type\nend module z_base\n");

  std::string out;
  std::string cmd = std::string(FXMOD_CLI_EXE) + " wrap --target gfortran --cache " +
                    cache_dir.string() + " -v -- " + gfortran + " -I" +
                    src_dir.string() + " -fsyntax-only /dev/null";
  FXMOD_CHECK_EQ(run_command(cmd, &out), 0);
  if (out.find("failed to compile") != std::string::npos) {
    std::fprintf(stderr, "wrap output:\n%s", out.c_str());
    return 1;
  }
  FXMOD_CHECK(out.find("2 converted") != std::string::npos);
  for (const char *m : {"a_dep.mod", "z_base.mod"}) {
    fxmod_module *mod = nullptr;
    FXMOD_CHECK_EQ(fxmod_open((cache_dir / m).string().c_str(), &mod, nullptr),
                   FXMOD_OK);
    FXMOD_CHECK_EQ(fxmod_module_family(mod), FXMOD_FAMILY_GFORTRAN);
    fxmod_module_free(mod);
  }

  std::puts("test_wrap_order: OK");
  return 0;
}
