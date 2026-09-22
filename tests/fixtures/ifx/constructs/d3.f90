module d3
  type :: point
    real :: x, y
  end type point
  type :: line
    type(point) :: p1, p2
  end type line
end module d3
