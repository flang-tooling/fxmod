module o2
  type :: point
    real :: x, y
  end type point
contains
  function make_point(v) result(p)
    real, intent(in) :: v
    type(point) :: p
  end function make_point
end module o2
