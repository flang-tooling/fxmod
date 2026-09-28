// Procedure interfaces beyond plain data dummies: dummy procedures
// (procedure(iface), external), the attributes that change what an actual
// may be (value, pointer, allocatable, target, contiguous), CLASS dummies
// and results -- whose pointer/allocatable gfortran encodes in the name of
// the __class_* container -- a result variable declared apart from the
// function, and an explicit-shape bound reaching into a CLASS dummy
// (`v(grid%n)`, as ELPA's interfaces have). The abstract interface a
// procedure dummy names must be declared ahead of its users.
#include <fxmod/module.hpp>

#include "test_util.hpp"

namespace fs = std::filesystem;

int main() {
  fs::path gf = fresh_dir("fxmod-procedures-test");
  gfortran_fixture_modules(gf, {"procedures.f90"});

  fxmod::EmitResult r =
      fxmod::ModuleFile::open((gf / "procedures.mod").string())
          .emit_fortran_source(/*strict=*/true);
  FXMOD_CHECK(r.problems.empty());
  const std::string &src = r.source;
  for (const char *decl : {
           "procedure(kernel_i) :: f",
           "procedure(kernel_i), pointer, intent(in) :: g",
           "real(8), external :: h",
           "integer(4), value :: a",
           "real(8), dimension(:), intent(in), pointer :: p",
           "real(8), dimension(:), intent(inout), allocatable :: q",
           "real(8), intent(in), target :: t",
           "real(8), dimension(:), intent(in), contiguous :: c",
           "class(Grid_t), intent(in) :: grid",
           "real(8), dimension(grid%n), intent(in) :: v",
           "class(Grid_t), pointer :: make",
           "class(*), intent(in) :: x",
           "real(8), dimension(:), allocatable :: series",
       })
    if (src.find(decl) == std::string::npos) {
      std::fprintf(stderr, "missing '%s' in:\n%s", decl, src.c_str());
      return 1;
    }
  FXMOD_CHECK(src.find("abstract interface") < src.find("subroutine apply"));

  fs::path out = fresh_dir("fxmod-procedures-test-out");
  check_gfortran_accepts(out, "procedures.f90", src);
  check_gfortran_accepts(out, "user.f90",
                         "program user\n"
                         "  use procedures\n"
                         "  implicit none\n"
                         "  procedure(kernel_i), pointer :: pk => null()\n"
                         "  class(grid_t), pointer :: g\n"
                         "  real(8), allocatable :: q(:)\n"
                         "  real(8), pointer :: p(:) => null()\n"
                         "  real(8), target :: t\n"
                         "  real(8), external :: ext\n"
                         "  g => make(4)\n"
                         "  call attrs(1, p, q, t, [1d0, 2d0])\n"
                         "  call sized(g, [1d0, 2d0, 3d0, 4d0])\n"
                         "  call apply(pk, pk, ext, t)\n"
                         "  if (any_of(g)) q = series(3)\n"
                         "end program user\n");

  std::puts("test_gfortran_procedures: OK");
  return 0;
}
