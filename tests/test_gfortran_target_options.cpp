// What fxmod::EmitOptions tells the translation about the compiler it is
// for. unsupported_kinds: a generic keeps only the specifics that compiler
// can call, anything else using the kind is a problem, and a re-export of
// such an entity is dropped silently, as its own module's translation
// drops it. available_modules: a re-export from a module outside the list
// is declared from the re-exporting module's own copy -- a constant by its
// value, a procedure by its interface, a type by its components -- since a
// `use` of it could not compile; a variable is a problem. Independently of
// either, re-exports of the compilers' own OpenMP module use it whole.
#include <fxmod/module.hpp>

#include "test_util.hpp"

namespace fs = std::filesystem;

namespace {

bool has(const std::string &src, const std::string &what) {
  return src.find(what) != std::string::npos;
}

} // namespace

int main() {
  fs::path gf = fresh_dir("fxmod-target-options-test");
  gfortran_fixture_modules(gf, {"wide_kinds.f90", "wide_reexport.f90",
                                "hidden_dep.f90", "passes_on.f90"});

  // Without options, the real(16) interfaces are translated (the real(16)
  // constant is not: its value is beyond what a double carries).
  fxmod::ModuleFile wide = fxmod::ModuleFile::open((gf / "wide_kinds.mod").string());
  FXMOD_CHECK(has(wide.emit_fortran_source(false).source, "widen_16"));

  fxmod::EmitOptions no_quad;
  no_quad.strict = false;
  no_quad.unsupported_kinds = {{"REAL", 16}};
  fxmod::EmitResult w = wide.emit_fortran_source(no_quad);
  FXMOD_CHECK(has(w.source, "interface widen") && has(w.source, "widen_8"));
  FXMOD_CHECK(!has(w.source, "widen_16") && !has(w.source, "real(16)"));
  FXMOD_CHECK_EQ(w.problems.size(), std::size_t(2)); // quad_one, only_quad
  for (const std::string &p : w.problems)
    FXMOD_CHECK(has(p, "real(16)"));

  fxmod::EmitOptions no_quad_strict = no_quad;
  no_quad_strict.strict = true;
  fxmod::EmitResult re =
      fxmod::ModuleFile::open((gf / "wide_reexport.mod").string())
          .emit_fortran_source(no_quad_strict);
  FXMOD_CHECK(has(re.source, "use wide_kinds, only: widen, widen_8"));
  FXMOD_CHECK(!has(re.source, "quad_one") && !has(re.source, "only_quad"));

  // passes_on re-exports from hidden_dep, which is not available.
  fxmod::EmitOptions alone;
  alone.strict = false;
  alone.available_modules = std::vector<std::string>{};
  fxmod::EmitResult p =
      fxmod::ModuleFile::open((gf / "passes_on.mod").string())
          .emit_fortran_source(alone);
  FXMOD_CHECK(!has(p.source, "use hidden_dep"));
  FXMOD_CHECK(has(p.source, "integer(4), parameter :: version = 3"));
  FXMOD_CHECK(has(p.source, "parameter :: label = 'dep'"));
  FXMOD_CHECK(has(p.source, "function doubled(x)"));
  FXMOD_CHECK(has(p.source, "type :: Item_t"));
  FXMOD_CHECK_EQ(p.problems.size(), std::size_t(1));
  FXMOD_CHECK(has(p.problems[0], "counter"));
  fs::path out = fresh_dir("fxmod-target-options-test-out");
  check_gfortran_accepts(out, "passes_on.f90", p.source); // no hidden_dep.mod

  // Listed as available, hidden_dep is used as usual.
  alone.available_modules = std::vector<std::string>{"hidden_dep"};
  FXMOD_CHECK(has(fxmod::ModuleFile::open((gf / "passes_on.mod").string())
                      .emit_fortran_source(alone)
                      .source,
                  "use hidden_dep, only:"));

  fs::path omp = fresh_dir("fxmod-target-options-test-omp");
  gfortran_fixture_modules(omp, {"openmp_reexport.f90"}, "-fopenmp");
  fxmod::EmitResult o =
      fxmod::ModuleFile::open((omp / "openmp_reexport.mod").string())
          .emit_fortran_source(true);
  FXMOD_CHECK(has(o.source, "\nuse omp_lib\n"));
  FXMOD_CHECK(!has(o.source, "omp_lib, only"));

  std::puts("test_gfortran_target_options: OK");
  return 0;
}
