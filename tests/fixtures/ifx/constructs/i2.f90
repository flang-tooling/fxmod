module i2
  type :: point
    real :: x, y
  end type point
  type :: t2
    type(point) :: a
    type(point) :: arr(3)
  end type t2
end module i2
