! Procedure dummies, dummy attributes, and a result variable of its own.
module procedures
  implicit none
  abstract interface
    real(8) function kernel_i(x)
      real(8), intent(in) :: x
    end function kernel_i
  end interface
contains
  subroutine apply(f, g, h, x)
    procedure(kernel_i) :: f
    procedure(kernel_i), pointer, intent(in) :: g
    real(8), external :: h
    real(8), intent(inout) :: x
  end subroutine apply
  subroutine attrs(a, p, q, t, c)
    integer, value :: a
    real(8), pointer, intent(in) :: p(:)
    real(8), allocatable, intent(inout) :: q(:)
    real(8), target, intent(in) :: t
    real(8), contiguous, intent(in) :: c(:)
  end subroutine attrs
  function series(n) result(r)
    integer, intent(in) :: n
    real(8), allocatable :: r(:)
    allocate(r(n))
  end function series
end module procedures
