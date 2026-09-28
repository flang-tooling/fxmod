// The real round-trip check: emit Fortran source from a real gfortran
// module and feed it to an actual, independent gfortran binary. This is
// the concrete version of "put it through any compiler to regenerate the
// mod file" -- not just a string comparison against expected output.
//
// Covers every fixture in the corpus, not just the fully-translatable
// ones: best-effort output for a module with real gaps (generics, arrays,
// derived-type constants) must still be *valid* Fortran for whatever it
// did manage to translate. This is what actually caught real bugs during
// development -- interface generation that only became wrong once fed to
// a real compiler (a nested INTERFACE block inside a generic, a
// use-associated type referenced without being declared, the same
// specific procedure declared twice under two generic names) -- so this
// test intentionally keeps exercising the harder, partially-translatable
// files rather than only the two clean ones.
#include <fxmod/module.hpp>

#include <cstdlib>

#include "test_util.hpp"

namespace {

// Returns gfortran's stderr, or asserts and returns "" on success.
std::string check_compiles(const std::string &gfortran, const std::string &path) {
  std::string cmd = gfortran + " -fsyntax-only " + path + " 2>&1";
  FILE *p = popen(cmd.c_str(), "r");
  FXMOD_CHECK(p != nullptr);
  std::string output;
  char buf[1024];
  while (std::size_t n = std::fread(buf, 1, sizeof buf, p))
    output.append(buf, n);
  int status = pclose(p);
  return status == 0 ? std::string() : output;
}

} // namespace

int main() {
  std::string dir = find_gfortran_finclude_dir();
  if (dir.empty())
    skip("no gfortran finclude directory found");
  std::string gfortran = which("gfortran");
  if (gfortran.empty())
    skip("no gfortran binary on PATH");

  std::filesystem::path tmp =
      std::filesystem::temp_directory_path() / "fxmod-emit-test";
  std::filesystem::create_directories(tmp);

  // Fully translatable (0 untranslatable symbols) -- strict emission must
  // succeed and match best-effort exactly. omp_lib.mod's own gaps were
  // all cross-module derived-type references (to omp_lib_kinds and
  // gfortran's internal __iso_c_binding) -- closed once the emitter
  // started satisfying those with a `use` statement instead of refusing.
  // openacc.mod's were assumed-type (type(*)) dummies and a specific
  // shared by two generics, closed by emitting type(*) and naming the
  // second occurrence in a procedure statement.
  const char *clean[] = {"omp_lib_kinds.mod", "openacc_kinds.mod",
                          "ieee_features.mod", "omp_lib.mod", "openacc.mod"};
  // The rest of the corpus: real, documented gaps remain (generics,
  // arrays, derived-type constants), so strict emission must refuse --
  // but best-effort must still produce valid Fortran for everything it
  // did translate.
  const char *partial[] = {"ieee_exceptions.mod", "ieee_arithmetic.mod"};

  for (const char *file : clean) {
    std::string path = dir + "/" + file;
    if (!std::filesystem::exists(path)) {
      std::fprintf(stderr, "skipping %s: not present\n", file);
      continue;
    }
    fxmod::ModuleFile mf = fxmod::ModuleFile::open(path);

    fxmod::EmitResult strict = mf.emit_fortran_source(/*strict=*/true);
    FXMOD_CHECK(strict.problems.empty());
    FXMOD_CHECK(!strict.source.empty());

    std::string src_path = (tmp / (std::string(file) + ".f90")).string();
    write_file(src_path, strict.source);
    std::string err = check_compiles(gfortran, src_path);
    if (!err.empty()) {
      std::fprintf(stderr,
                    "gfortran -fsyntax-only rejected emitted source for "
                    "%s:\n%s\n",
                    file, err.c_str());
      return 1;
    }
  }

  for (const char *file : partial) {
    std::string path = dir + "/" + file;
    if (!std::filesystem::exists(path)) {
      std::fprintf(stderr, "skipping %s: not present\n", file);
      continue;
    }
    fxmod::ModuleFile mf = fxmod::ModuleFile::open(path);

    // Strict mode must refuse -- this file has known, documented gaps
    // (see test_gfortran_parse.cpp's expected counts).
    bool threw = false;
    try {
      mf.emit_fortran_source(/*strict=*/true);
    } catch (const fxmod::UnsupportedError &) {
      threw = true;
    }
    if (!threw) {
      std::fprintf(stderr,
                    "%s: strict emission unexpectedly succeeded -- update "
                    "test_gfortran_parse.cpp's expected count if this "
                    "file's gaps were genuinely closed\n",
                    file);
      return 1;
    }

    // Best-effort must still be valid Fortran for whatever it translated.
    fxmod::EmitResult best = mf.emit_fortran_source(/*strict=*/false);
    FXMOD_CHECK(!best.problems.empty());
    FXMOD_CHECK(!best.source.empty());

    std::string src_path =
        (tmp / (std::string(file) + ".best-effort.f90")).string();
    write_file(src_path, best.source);
    std::string err = check_compiles(gfortran, src_path);
    if (!err.empty()) {
      std::fprintf(stderr,
                    "gfortran -fsyntax-only rejected best-effort source "
                    "for %s:\n%s\n",
                    file, err.c_str());
      return 1;
    }
  }

  std::puts("test_gfortran_emit: OK");
  return 0;
}
