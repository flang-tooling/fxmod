// What fxmod::EmitOptions tells the translation about the compiler it is
// for. unsupported_kinds: a generic keeps only the specifics that compiler
// can call, anything else using the kind is a problem, and a re-export of
// such an entity is dropped silently, as its own module's translation
// drops it.
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
  gfortran_fixture_modules(gf, {"wide_kinds.f90", "wide_reexport.f90"});

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

  std::puts("test_gfortran_target_options: OK");
  return 0;
}
