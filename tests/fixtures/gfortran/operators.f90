! Defined operators: an intrinsic one (==, also reachable as .eq.) whose
! specific is shared with a named generic, the old-style spelling of
! another (.ne.), a user-defined operator, and a defined assignment.
module operators
  implicit none
  type :: handle_t
    integer :: v
  end type handle_t
  interface operator(==)
    module procedure handle_eq
  end interface
  interface operator(.ne.)
    module procedure handle_ne
  end interface
  interface operator(.cross.)
    module procedure handle_cross
  end interface
  interface assignment(=)
    module procedure handle_from_int
  end interface
  interface same
    module procedure handle_eq
  end interface
contains
  logical function handle_eq(a, b)
    type(handle_t), intent(in) :: a, b
    handle_eq = a%v == b%v
  end function handle_eq
  logical function handle_ne(a, b)
    type(handle_t), intent(in) :: a, b
    handle_ne = a%v /= b%v
  end function handle_ne
  integer function handle_cross(a, b)
    type(handle_t), intent(in) :: a, b
    handle_cross = a%v * b%v
  end function handle_cross
  subroutine handle_from_int(h, i)
    type(handle_t), intent(out) :: h
    integer, intent(in) :: i
    h%v = i
  end subroutine handle_from_int
end module operators
