// Character types keep their length -- without one, character(len=12)
// would come back as character(len=1) -- and character constants their
// value: gfortran writes it after its length, with non-printable
// characters escaped (\Uxxxxxxxx), which comes back as achar(). The check
// program compares each against the expression that defined it.
#include <fxmod/module.hpp>

#include "test_util.hpp"

namespace fs = std::filesystem;

int main() {
  fs::path gf = fresh_dir("fxmod-characters-test");
  gfortran_fixture_modules(gf, {"characters.f90"});

  fxmod::EmitResult r =
      fxmod::ModuleFile::open((gf / "characters.mod").string())
          .emit_fortran_source(/*strict=*/true);
  FXMOD_CHECK(r.problems.empty());
  const std::string &src = r.source;
  for (const char *decl : {
           "character(len=10, kind=1), parameter :: greeting = "
           "'it''s'//achar(0)//achar(10)//'done'",
           "character(len=5, kind=1), parameter :: padded = 'ab   '",
           "character(len=:, kind=1), allocatable :: deferred",
           "character(len=12, kind=1) :: fixed",
           "character(len=*, kind=1), intent(in) :: s",
           "character(len=n, kind=1), intent(out) :: t",
           "character(len=:, kind=1), intent(out), allocatable :: u",
       })
    if (src.find(decl) == std::string::npos) {
      std::fprintf(stderr, "missing '%s' in:\n%s", decl, src.c_str());
      return 1;
    }

  fs::path out = fresh_dir("fxmod-characters-test-out");
  check_gfortran_accepts(out, "characters.f90", src, "-c");
  check_gfortran_accepts(
      out, "check.f90",
      "program check\n"
      "  use characters\n"
      "  implicit none\n"
      "  if (greeting /= 'it''s'//achar(0)//achar(10)//'done') error stop 1\n"
      "  if (len(greeting) /= 10 .or. len(padded) /= 5) error stop 2\n"
      "  if (padded /= 'ab') error stop 3\n"
      "  if (len(fixed) /= 12) error stop 4\n"
      "end program check\n",
      "-o check characters.o");
  std::string output;
  if (run_command((out / "check").string(), &output) != 0) {
    std::fprintf(stderr, "translated constants differ:\n%s", output.c_str());
    return 1;
  }

  std::puts("test_gfortran_characters: OK");
  return 0;
}
