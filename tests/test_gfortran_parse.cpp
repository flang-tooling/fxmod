// Cross-checks the library against the real GCC 15.2-built fixtures whose
// symbol/flavor counts are already documented in README.md's table (and
// were independently re-verified against fxmod.py during development --
// see the plan notes). Skips if no matching gfortran install is found.
#include <fxmod/module.hpp>

#include "test_util.hpp"

namespace {
struct Expected {
  const char *file;
  int public_count;
  int untranslatable;
};
} // namespace

int main() {
  std::string dir = find_gfortran_finclude_dir();
  if (dir.empty())
    skip("no gfortran finclude directory found (set "
         "FXMOD_GFORTRAN_FINCLUDE_DIR to override)");

  // Counts re-verified against a real `gfortran -fsyntax-only` round-trip
  // of the emitted best-effort source for every one of these files (see
  // test_gfortran_emit.cpp) after array specs, generic interfaces,
  // derived-type constant values, and cross-module `use` statements were
  // implemented. `untranslatable` can exceed `public_count`: it also
  // counts generics that exist only in the module's generic-interfaces
  // section with no symtree entry of their own (an explicit `interface
  // NAME ... end interface` block in the source), which "public" --
  // driven purely by the symtree -- never counted in the first place.
  const Expected cases[] = {
      {"omp_lib_kinds.mod", 118, 0},   {"openacc_kinds.mod", 20, 0},
      {"ieee_features.mod", 17, 0},    {"ieee_exceptions.mod", 31, 2},
      {"openacc.mod", 28, 0},          {"ieee_arithmetic.mod", 62, 29},
      {"omp_lib.mod", 232, 0},
  };

  for (const Expected &c : cases) {
    std::string path = dir + "/" + c.file;
    if (!std::filesystem::exists(path)) {
      std::fprintf(stderr, "skipping %s: not present in %s\n", c.file,
                   dir.c_str());
      continue;
    }
    fxmod::ModuleFile mf = fxmod::ModuleFile::open(path);
    FXMOD_CHECK(mf.family() == fxmod::Family::Gfortran);
    FXMOD_CHECK(mf.version() == "15" || mf.version() == "16");

    auto syms = mf.gfortran_public_symbols();
    if (static_cast<int>(syms.size()) != c.public_count) {
      std::fprintf(stderr, "%s: public symbol count %zu != expected %d\n",
                    c.file, syms.size(), c.public_count);
      return 1;
    }

    fxmod::EmitResult r = mf.emit_fortran_source(/*strict=*/false);
    if (static_cast<int>(r.problems.size()) != c.untranslatable) {
      std::fprintf(stderr, "%s: untranslatable count %zu != expected %d\n",
                    c.file, r.problems.size(), c.untranslatable);
      return 1;
    }
  }

  std::puts("test_gfortran_parse: OK");
  return 0;
}
