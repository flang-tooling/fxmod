// Array constants: gfortran writes an array constructor (the elements, in
// array element order, plus the shape), which comes back as a constructor
// -- reshaped for rank > 1 -- declared with the PARAMETER's own dimension.
// Integer constants keep their kind suffix, without which a value beyond
// default-integer range does not compile. The check program compares each
// against the expression that defined it.
#include <fxmod/module.hpp>

#include "test_util.hpp"

namespace fs = std::filesystem;

int main() {
  fs::path gf = fresh_dir("fxmod-array-constants-test");
  gfortran_fixture_modules(gf, {"array_constants.f90"});

  fxmod::EmitResult r =
      fxmod::ModuleFile::open((gf / "array_constants.mod").string())
          .emit_fortran_source(/*strict=*/true);
  FXMOD_CHECK(r.problems.empty());
  const std::string &src = r.source;
  for (const char *decl : {
           "integer(8), parameter :: big = -9223372036854775807_8",
           "integer(8), dimension(2), parameter :: dims = [-1_8, 7_8]",
           "real(8), dimension(2, 2), parameter :: grid = reshape([",
           "dimension(3), parameter :: names = ['ab', 'cd', 'ef']",
       })
    if (src.find(decl) == std::string::npos) {
      std::fprintf(stderr, "missing '%s' in:\n%s", decl, src.c_str());
      return 1;
    }

  fs::path out = fresh_dir("fxmod-array-constants-test-out");
  check_gfortran_accepts(out, "array_constants.f90", src, "-c");
  check_gfortran_accepts(
      out, "check.f90",
      "program check\n"
      "  use array_constants\n"
      "  implicit none\n"
      "  if (big /= -9223372036854775807_8) error stop 1\n"
      "  if (any(dims /= [-1_8, 7_8])) error stop 2\n"
      "  if (any(grid /= reshape([1d0, 2d0, 3d0, 4d0], [2, 2]))) error stop 3\n"
      "  if (any(names /= ['ab', 'cd', 'ef'])) error stop 4\n"
      "end program check\n",
      "-o check array_constants.o");
  std::string output;
  if (run_command((out / "check").string(), &output) != 0) {
    std::fprintf(stderr, "translated constants differ:\n%s", output.c_str());
    return 1;
  }

  std::puts("test_gfortran_array_constants: OK");
  return 0;
}
