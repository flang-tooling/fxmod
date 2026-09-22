// The real round-trip check for the ifx emitter, mirroring
// test_gfortran_emit.cpp: emit Fortran source from a real ifx module and
// feed it to an actual, independent gfortran binary, rather than just
// comparing strings. Doesn't need find_gfortran_finclude_dir() (that's
// specifically for locating gfortran's own bundled .mod files) -- only a
// gfortran binary on PATH to compile the emitted source with.
#include <fxmod/module.hpp>

#include <cstdlib>

#include "test_util.hpp"

#ifndef FXMOD_FIXTURES_DIR
#error "FXMOD_FIXTURES_DIR must be defined by the build"
#endif

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
  std::string gfortran = which("gfortran");
  if (gfortran.empty())
    skip("no gfortran binary on PATH");

  std::filesystem::path tmp =
      std::filesystem::temp_directory_path() / "fxmod-ifx-emit-test";
  std::filesystem::create_directories(tmp);

  std::string dir = std::string(FXMOD_FIXTURES_DIR) + "/ifx";

  // Fully within the decoded subset (see src/ifx/fortran_emitter.hpp):
  // scalar PARAMETERs/variables of every decoded type including
  // CHARACTER/COMPLEX/REAL16, fixed-size arrays of rank 1 and 2 with
  // both default and non-default lower bounds, a derived type +
  // variable, nested derived types (a type composed of another local
  // type, emitted in dependency order), procedure interfaces with scalar
  // or CHARACTER (explicit or assumed length) arguments, a CHARACTER
  // variable/dummy-argument/derived-type field with an explicit constant
  // length, a COMMON block -- one real sample per construct, plus a
  // real-world 34-entity OpenMP module exercising all of it together.
  const char *clean[] = {
      "mymod.mod",         "testmod.mod",        "constructs/t1.mod",
      "constructs/t2.mod", "constructs/t3.mod",  "constructs/t4.mod",
      "constructs/t5.mod", "constructs/t6.mod",  "constructs/t7.mod",
      "constructs/t8.mod", "constructs/t9.mod",  "constructs/t10.mod",
      "constructs/t11.mod","constructs/c1.mod",  "constructs/c2.mod",
      "constructs/c3.mod", "constructs/c5.mod",  "constructs/c5b.mod",
      "constructs/d2.mod", "constructs/d3.mod",  "constructs/d7.mod",
      "constructs/e1.mod",  "constructs/e2.mod",  "constructs/f1.mod",
      "constructs/f3.mod",  "constructs/f4.mod",  "constructs/f5.mod",
      "constructs/g1.mod",  "constructs/g2.mod",  "constructs/g3.mod",
      "constructs/h1.mod",  "constructs/h2.mod",
      "constructs/i1.mod",  "constructs/i2.mod",
      "constructs/j1.mod",  "constructs/j2.mod",
      "constructs/k1.mod",  "constructs/k2.mod",
      "constructs/l1.mod",  "constructs/l2.mod",  "constructs/l3.mod",
      "constructs/m1.mod",  "constructs/m3.mod",  "constructs/m4.mod",
      "constructs/n1.mod",
      "constructs/o1.mod",  "constructs/o2.mod",  "constructs/o3.mod",  "constructs/o4.mod",
      "constructs/p1.mod",  "constructs/p2.mod",  "constructs/p3.mod",
      "constructs/p4.mod",  "constructs/p5.mod",
      "constructs/q1.mod",  "constructs/q2.mod",  "constructs/r1.mod",
      "constructs/s1.mod",  "constructs/s2.mod",  "constructs/t1.mod",
      "constructs/u1.mod",
      "constructs/c4.mod",
      "omp/omp_lib_kinds.mod",
      // Not here: constructs/g3b.mod -- exercises cross-module StructField
      // and FuncRetStructArray together (`use G3, only: POINT`), verified
      // manually with a real two-step `ifx` compile (g3 then g3b) but not
      // addable to this loop as-is: check_compiles() only runs
      // `-fsyntax-only` per fixture, with no prior real `-c` compile of
      // g3's own emitted source to produce a real G3.mod for g3b's `use`
      // to resolve against.
  };
  // Real, documented gaps remain (symbolic-length CHARACTER, REAL/
  // LOGICAL/CHARACTER/COMPLEX PARAMETER arrays or rank > 1 ones --
  // INTEGER rank-1 PARAMETER arrays, formerly here, are decoded now, see
  // constructs/c4.mod above) -- strict emission must refuse. omp_lib.mod's
  // own cross-module references (to omp_lib_kinds, confirmed via the real
  // `OMP_ALLOCTRAIT+OMP_LIB_KINDS`-style compound directory names) are
  // satisfied with a `use OMP_LIB_KINDS, only: ...` line and compile
  // cleanly -- gfortran's own bundled omp_lib_kinds.mod, on its default
  // search path, happens to satisfy that `use` for this syntax check
  // too, since both vendors implement the same standard module.
  const char *partial[] = {"omp/omp_lib.mod"};

  for (const char *file : clean) {
    std::string path = dir + "/" + file;
    if (!std::filesystem::exists(path)) {
      std::fprintf(stderr, "skipping %s: not present\n", file);
      continue;
    }
    fxmod::ModuleFile mf = fxmod::ModuleFile::open(path);

    fxmod::EmitResult r = mf.emit_fortran_source(/*strict=*/true);
    FXMOD_CHECK(r.problems.empty());
    FXMOD_CHECK(!r.source.empty());

    std::string safe_name = file;
    for (char &c : safe_name)
      if (c == '/')
        c = '_';
    std::string src_path = (tmp / (safe_name + ".f90")).string();
    write_file(src_path, r.source);
    std::string err = check_compiles(gfortran, src_path);
    if (!err.empty()) {
      std::fprintf(stderr,
                    "gfortran -fsyntax-only rejected emitted source for "
                    "%s:\n%s\nsource was:\n%s\n",
                    file, err.c_str(), r.source.c_str());
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

    bool threw = false;
    try {
      mf.emit_fortran_source(/*strict=*/true);
    } catch (const fxmod::UnsupportedError &) {
      threw = true;
    }
    if (!threw) {
      std::fprintf(stderr,
                    "%s: strict emission unexpectedly succeeded -- update "
                    "this test if the gap was genuinely closed\n",
                    file);
      return 1;
    }

    fxmod::EmitResult best = mf.emit_fortran_source(/*strict=*/false);
    FXMOD_CHECK(!best.problems.empty());
    FXMOD_CHECK(!best.source.empty());

    std::string safe_name = file;
    for (char &c : safe_name)
      if (c == '/')
        c = '_';
    std::string src_path = (tmp / (safe_name + ".best_effort.f90")).string();
    write_file(src_path, best.source);
    std::string err = check_compiles(gfortran, src_path);
    if (!err.empty()) {
      std::fprintf(stderr,
                    "gfortran -fsyntax-only rejected best-effort source "
                    "for %s:\n%s\nsource was:\n%s\n",
                    file, err.c_str(), best.source.c_str());
      return 1;
    }
  }

  std::puts("test_ifx_emit: OK");
  return 0;
}
