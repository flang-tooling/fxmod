// Derived types as declared, not just their components: EXTENDS (gfortran
// stores the parent as a first component, which must not become one),
// ABSTRACT, and the type-bound procedures of the type's f2k namespace.
// Binding targets that are not public still need an interface here,
// declared private. Types are declared in component order: holder_t needs
// point_t first, although it sorts before it, and point_t the private
// coords_t, which is declared although it is not public -- as is a parent
// type before its extensions.
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
  for (const char *line : {
           "type, abstract :: Shape_t",
           "  procedure(area_i), deferred :: area",
           "  procedure :: describe => shape_describe",
           "  procedure, nopass :: kind_name",
           "  procedure, pass(other) :: same_id",
           "  procedure, private :: secret",
           "  generic :: info => describe",
           "type, extends(Shape_t) :: Circle_t",
           "  procedure :: area => circle_area",
           "private :: shape_describe",
       })
    if (src.find(line) == std::string::npos) {
      std::fprintf(stderr, "missing '%s' in:\n%s", line, src.c_str());
      return 1;
    }
  FXMOD_CHECK(src.find(":: shape_t") == std::string::npos); // no parent component
  FXMOD_CHECK(src.find("type :: Coords_t") < src.find("type :: Point_t"));
  FXMOD_CHECK(src.find("type :: Point_t") < src.find("type :: Holder_t"));

  fs::path out = fresh_dir("fxmod-derived-types-test-out");
  check_gfortran_accepts(out, "derived_types.f90", src);
  check_gfortran_accepts(out, "user.f90",
                         "program user\n"
                         "  use derived_types\n"
                         "  implicit none\n"
                         "  type(circle_t) :: c\n"
                         "  class(shape_t), allocatable :: s\n"
                         "  type(holder_t) :: h\n"
                         "  h%p%c%x = 1d0\n"
                         "  allocate(s, source=c)\n"
                         "  call s%info()\n"
                         "  print *, s%area(), c%kind_name(), c%same_id(1)\n"
                         "end program user\n");

  std::puts("test_gfortran_derived_types: OK");
  return 0;
}
