// Assumed-type (type(*)) dummies, as MPI and OpenACC declare their choice
// buffers, and gfortran's NO_ARG_CHECK attribute on them, which the
// translation must carry: it is what keeps a generic over such specifics
// unambiguous. It is emitted in gfortran's and Flang's spelling
// (!GCC$ ATTRIBUTES NO_ARG_CHECK, !DIR$ IGNORE_TKR), each compiler
// ignoring the other's.
#include <fxmod/module.hpp>

#include "test_util.hpp"

namespace fs = std::filesystem;

int main() {
  fs::path gf = fresh_dir("fxmod-assumed-type-test");
  gfortran_fixture_modules(gf, {"assumed_type.f90"});

  fxmod::EmitResult r =
      fxmod::ModuleFile::open((gf / "assumed_type.mod").string())
          .emit_fortran_source(/*strict=*/true);
  FXMOD_CHECK(r.problems.empty());
  FXMOD_CHECK(r.source.find("type(*), intent(in) :: buf") != std::string::npos);
  FXMOD_CHECK(r.source.find("type(*), dimension(..), intent(in) :: buf") !=
              std::string::npos);
  FXMOD_CHECK(r.source.find("!GCC$ ATTRIBUTES NO_ARG_CHECK :: buf") !=
              std::string::npos);
  FXMOD_CHECK(r.source.find("!DIR$ IGNORE_TKR (tkr) buf") != std::string::npos);
  // Only send_count's buffer carries it.
  FXMOD_CHECK(r.source.find("NO_ARG_CHECK") == r.source.rfind("NO_ARG_CHECK"));

  fs::path out = fresh_dir("fxmod-assumed-type-test-out");
  check_gfortran_accepts(out, "assumed_type.f90", r.source);
  check_gfortran_accepts(out, "user.f90",
                         "program user\n"
                         "  use assumed_type\n"
                         "  implicit none\n"
                         "  real(8) :: x(3)\n"
                         "  integer :: k\n"
                         "  call take_scalar(k)\n"
                         "  call take_rank(x)\n"
                         "  call send(x, 1)\n"
                         "  call send(x, 3, 1)\n"
                         "end program user\n");

  std::puts("test_gfortran_assumed_type: OK");
  return 0;
}
