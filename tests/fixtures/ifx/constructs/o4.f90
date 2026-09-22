module o4
  type :: point
    real :: x, y
  end type point
contains
  function make_points(v) result(arr)
    real, intent(in) :: v
    type(point) :: arr(2)
  end function make_points
end module o4
