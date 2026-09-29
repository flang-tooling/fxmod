// A module that USEs another and passes its names on must translate to a
// `use` of that module, not to redeclarations: a redeclared constant or
// procedure would be a distinct entity from the original, ambiguous with it
// wherever both are visible (and intrinsic-module entities such as
// iso_c_binding's c_int would be redeclared from gfortran's internal view
// of them). Compiles the fixtures with gfortran, translates both modules,
// and checks gfortran accepts the translations together with a program
// that uses the re-exported names through the re-exporting module.
#include <fxmod/module.hpp>

#include "test_util.hpp"

namespace fs = std::filesystem;

int main() {
  fs::path gf = fresh_dir("fxmod-reexport-test");
  gfortran_fixture_modules(gf, {"reexport_base.f90", "reexport.f90"});

  fxmod::EmitResult base =
      fxmod::ModuleFile::open((gf / "reexport_base.mod").string())
          .emit_fortran_source(/*strict=*/true);
  fxmod::EmitResult re =
      fxmod::ModuleFile::open((gf / "reexport.mod").string())
          .emit_fortran_source(/*strict=*/true);
  FXMOD_CHECK(base.problems.empty());
  FXMOD_CHECK(re.problems.empty());

  // iso_c_binding's names are used from the intrinsic module, in both.
  for (const std::string *src : {&base.source, &re.source}) {
    FXMOD_CHECK(src->find("use, intrinsic :: iso_c_binding, only:") !=
                std::string::npos);
    FXMOD_CHECK(src->find(":: c_int =") == std::string::npos);
  }
  // reexport declares nothing of its own ...
  FXMOD_CHECK(re.source.find("parameter") == std::string::npos);
  FXMOD_CHECK(re.source.find("interface") == std::string::npos);
  FXMOD_CHECK(re.source.find("type ::") == std::string::npos);
  // ... and takes everything from reexport_base, the generic included.
  const std::size_t use = re.source.find("use reexport_base, only:");
  FXMOD_CHECK(use != std::string::npos);
  const std::string use_line = re.source.substr(use, re.source.find('\n', use) - use);
  for (const char *name : {"answer", "twice", "twice_int", "Box_t"})
    FXMOD_CHECK(use_line.find(name) != std::string::npos);

  fs::path out = fresh_dir("fxmod-reexport-test-out");
  check_gfortran_accepts(out, "reexport_base.f90", base.source);
  check_gfortran_accepts(out, "reexport.f90", re.source);
  check_gfortran_accepts(out, "user.f90",
                         "program user\n"
                         "  use reexport\n"
                         "  use reexport_base, only: answer\n" // same entity
                         "  implicit none\n"
                         "  type(box_t) :: b\n"
                         "  integer(c_int) :: k\n"
                         "  b%v = twice(answer)\n"
                         "  k = b%v\n"
                         "end program user\n");

  std::puts("test_gfortran_reexport: OK");
  return 0;
}
