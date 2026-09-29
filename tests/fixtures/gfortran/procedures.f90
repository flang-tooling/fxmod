! Procedure dummies, dummy attributes, polymorphic dummies and results, a
! result variable of its own, and an array bound reaching into a dummy.
module procedures
  implicit none
  type :: grid_t
    integer :: n = 3
  end type grid_t
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
  subroutine sized(grid, v)
    class(grid_t), intent(in) :: grid
    real(8), intent(in) :: v(grid%n)
  end subroutine sized
  function make(n) result(g)
    integer, intent(in) :: n
    class(grid_t), pointer :: g
    allocate(g)
    g%n = n
  end function make
  logical function any_of(x)
    class(*), intent(in) :: x
    any_of = .true.
  end function any_of
  function series(n) result(r)
    integer, intent(in) :: n
    real(8), allocatable :: r(:)
    allocate(r(n))
  end function series
end module procedures
