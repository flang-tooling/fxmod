! Type extension, an abstract type, type-bound procedures of every kind
! (deferred, renamed, nopass, pass(name), generic, private), binding targets
! that are not public, and types whose declaration order matters:
! holder_t's component is a point_t, which sorts after it, and point_t's
! is a private type.
module derived_types
  implicit none
  private
  public :: holder_t, point_t, shape_t, circle_t, area_i

  type :: coords_t
    real(8) :: x, y
  end type coords_t

  type :: point_t
    type(coords_t) :: c
  end type point_t

  type :: holder_t
    type(point_t) :: p
  end type holder_t

  type, abstract :: shape_t
    integer :: id = 0
  contains
    procedure(area_i), deferred :: area
    procedure :: describe => shape_describe
    procedure, nopass :: kind_name
    procedure, pass(other) :: same_id
    procedure, private :: secret
    generic :: info => describe
  end type shape_t

  abstract interface
    real(8) function area_i(self)
      import shape_t
      class(shape_t), intent(in) :: self
    end function area_i
  end interface

  type, extends(shape_t) :: circle_t
    real(8) :: r = 1
  contains
    procedure :: area => circle_area
  end type circle_t

contains
  subroutine shape_describe(self)
    class(shape_t), intent(in) :: self
  end subroutine shape_describe
  integer function kind_name()
    kind_name = 1
  end function kind_name
  logical function same_id(n, other)
    integer, intent(in) :: n
    class(shape_t), intent(in) :: other
    same_id = n == other%id
  end function same_id
  subroutine secret(self)
    class(shape_t), intent(in) :: self
  end subroutine secret
  real(8) function circle_area(self)
    class(circle_t), intent(in) :: self
    circle_area = 3 * self%r**2
  end function circle_area
end module derived_types
