// REAL and COMPLEX named constants: gfortran writes their values as
// base-16 MPFR strings ("-0.199999a@0" for -0.1), which must come back as
// literals of exactly the same value and kind. The check program compares
// each translated constant against the expression that defined it, both
// evaluated by gfortran, so any rounding in the translation shows up as an
// inequality.
#include <fxmod/module.hpp>

#include "test_util.hpp"

namespace fs = std::filesystem;

int main() {
  fs::path gf = fresh_dir("fxmod-real-constants-test");
  gfortran_fixture_modules(gf, {"real_constants.f90"});

  fxmod::EmitResult r =
      fxmod::ModuleFile::open((gf / "real_constants.mod").string())
          .emit_fortran_source(/*strict=*/true);
  FXMOD_CHECK(r.problems.empty());
  // Kind suffixes survive, and a whole number stays a REAL literal.
  FXMOD_CHECK(r.source.find("real(4), parameter :: tenth = -0.100000001_4") !=
              std::string::npos);
  FXMOD_CHECK(r.source.find(":: whole = 42.0_8") != std::string::npos);
  FXMOD_CHECK(r.source.find(":: zi = (0.5_8, -2.0_8)") != std::string::npos);

  fs::path out = fresh_dir("fxmod-real-constants-test-out");
  check_gfortran_accepts(out, "real_constants.f90", r.source, "-c");
  check_gfortran_accepts(
      out, "check.f90",
      "program check\n"
      "  use real_constants\n"
      "  implicit none\n"
      "  if (fill_double /= 9.9692099683868690d+36) error stop 1\n"
      "  if (fill_float /= 9.9692099683868690e+36) error stop 2\n"
      "  if (tenth /= -0.1) error stop 3\n"
      "  if (third /= 1.0d0 / 3.0d0) error stop 4\n"
      "  if (zero /= 0.0d0 .or. whole /= 42.0d0) error stop 5\n"
      "  if (tiny8 /= tiny(1.0d0) .or. huge8 /= huge(1.0d0)) error stop 6\n"
      "  if (zi /= (0.5d0, -2.0d0)) error stop 7\n"
      "  if (zf /= (1.0e-3, 3.0e5)) error stop 8\n"
      "end program check\n",
      "-o check real_constants.o");
  std::string output;
  if (run_command((out / "check").string(), &output) != 0) {
    std::fprintf(stderr, "translated constants differ:\n%s", output.c_str());
    return 1;
  }

  std::puts("test_gfortran_real_constants: OK");
  return 0;
}
