! Defines what reexport.f90 passes on, and pulls in iso_c_binding, whose
! entities gfortran then lists among this module's own public names.
module reexport_base
  use iso_c_binding
  implicit none
  integer, parameter :: answer = 42
  type :: box_t
    integer :: v
  end type box_t
  interface twice
    module procedure twice_int
  end interface twice
contains
  integer function twice_int(x)
    integer, intent(in) :: x
    twice_int = 2 * x
  end function twice_int
end module reexport_base
