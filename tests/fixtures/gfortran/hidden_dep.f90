! Stands for a module a library uses but does not install: re-exports
! from it must not become `use` statements (see passes_on.f90).
module hidden_dep
  implicit none
  integer, parameter :: version = 3
  character(len=*), parameter :: label = 'dep'
  integer :: counter
  type :: item_t
    integer :: v
  end type item_t
contains
  integer function doubled(x)
    integer, intent(in) :: x
    doubled = 2 * x
  end function doubled
end module hidden_dep
