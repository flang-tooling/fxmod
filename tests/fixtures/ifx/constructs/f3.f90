module f3
  type :: point
    real :: x, y
  end type point
contains
  function make_point() result(p)
    type(point) :: p
  end function make_point
end module f3
