! Types whose declaration order matters: holder_t's component is a
! point_t, which sorts after it, and point_t's is a private type.
module derived_types
  implicit none
  private
  public :: holder_t, point_t

  type :: coords_t
    real(8) :: x, y
  end type coords_t

  type :: point_t
    type(coords_t) :: c
  end type point_t

  type :: holder_t
    type(point_t) :: p
  end type holder_t
end module derived_types
