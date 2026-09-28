// Derived types are declared in component order: holder_t needs point_t
// first, although it sorts before it, and point_t the private coords_t,
// which is declared although it is not public.
#include <fxmod/module.hpp>

#include "test_util.hpp"

namespace fs = std::filesystem;

int main() {
  fs::path gf = fresh_dir("fxmod-derived-types-test");
  gfortran_fixture_modules(gf, {"derived_types.f90"});

  fxmod::EmitResult r =
      fxmod::ModuleFile::open((gf / "derived_types.mod").string())
          .emit_fortran_source(/*strict=*/true);
  FXMOD_CHECK(r.problems.empty());
  const std::string &src = r.source;
  FXMOD_CHECK(src.find("type :: Coords_t") < src.find("type :: Point_t"));
  FXMOD_CHECK(src.find("type :: Point_t") < src.find("type :: Holder_t"));

  fs::path out = fresh_dir("fxmod-derived-types-test-out");
  check_gfortran_accepts(out, "derived_types.f90", src);
  check_gfortran_accepts(out, "user.f90",
                         "program user\n"
                         "  use derived_types\n"
                         "  implicit none\n"
                         "  type(holder_t) :: h\n"
                         "  h%p%c%x = 1d0\n"
                         "end program user\n");

  std::puts("test_gfortran_derived_types: OK");
  return 0;
}
