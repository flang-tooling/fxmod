// Defined operators and assignment live in their own sections of a
// gfortran module (one list per intrinsic operator, in gfc_intrinsic_op
// order, then the user-defined ones); without them a translated module
// silently loses every overloaded ==, .op. and = its users rely on. Checks
// the defining module gets interface blocks under the right spellings, a
// re-exporting module gets `use ..., only: operator(...)`, and gfortran
// resolves a program's operator uses through the translations.
#include <fxmod/module.hpp>

#include "test_util.hpp"

namespace fs = std::filesystem;

int main() {
  fs::path gf = fresh_dir("fxmod-operators-test");
  gfortran_fixture_modules(gf, {"operators.f90", "operators_reexport.f90"});

  fxmod::EmitResult def =
      fxmod::ModuleFile::open((gf / "operators.mod").string())
          .emit_fortran_source(/*strict=*/true);
  fxmod::EmitResult re =
      fxmod::ModuleFile::open((gf / "operators_reexport.mod").string())
          .emit_fortran_source(/*strict=*/true);
  FXMOD_CHECK(def.problems.empty());
  FXMOD_CHECK(re.problems.empty());

  for (const char *spec : {"interface operator(==)", "interface operator(/=)",
                           "interface operator(.cross.)",
                           "interface assignment(=)"})
    FXMOD_CHECK(def.source.find(spec) != std::string::npos);
  // handle_eq is declared once, under whichever generic comes first, and
  // referenced from the other.
  FXMOD_CHECK(def.source.find(" function handle_eq(") ==
              def.source.rfind(" function handle_eq("));
  FXMOD_CHECK(def.source.find("procedure :: handle_eq") != std::string::npos);

  for (const char *spec : {"operator(==)", "operator(/=)", "operator(.cross.)",
                           "assignment(=)"})
    FXMOD_CHECK(re.source.find(spec) != std::string::npos);
  FXMOD_CHECK(re.source.find("interface") == std::string::npos);

  fs::path out = fresh_dir("fxmod-operators-test-out");
  check_gfortran_accepts(out, "operators.f90", def.source);
  check_gfortran_accepts(out, "operators_reexport.f90", re.source);
  check_gfortran_accepts(out, "user.f90",
                         "program user\n"
                         "  use operators_reexport\n"
                         "  implicit none\n"
                         "  type(handle_t) :: a, b\n"
                         "  a = 3\n"
                         "  b = 4\n"
                         "  if (a == b .or. .not. (a .ne. b)) stop 1\n"
                         "  if (same(a, b)) stop 2\n"
                         "  print *, a .cross. b\n"
                         "end program user\n");

  std::puts("test_gfortran_operators: OK");
  return 0;
}
