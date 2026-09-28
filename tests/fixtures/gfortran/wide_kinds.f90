! Entities using real(16), which some compilers (Flang on x86-64) lack.
module wide_kinds
  implicit none
  interface widen
    module procedure widen_8, widen_16
  end interface widen
  real(16), parameter :: quad_one = 1.0_16
contains
  real(8) function widen_8(x)
    real(8), intent(in) :: x
    widen_8 = x
  end function widen_8
  real(16) function widen_16(x)
    real(16), intent(in) :: x
    widen_16 = x
  end function widen_16
  subroutine only_quad(x)
    real(16), intent(inout) :: x
  end subroutine only_quad
end module wide_kinds
