module g3b
  use g3
  type :: line
    type(point) :: p1, p2
  end type line
contains
  function make_points() result(arr)
    type(point) :: arr(2)
  end function make_points
end module g3b
