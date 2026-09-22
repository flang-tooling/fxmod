module j1
  type :: point
    real :: x, y
  end type point
contains
  subroutine take_point_array(arr)
    type(point), intent(in) :: arr(3)
  end subroutine take_point_array
end module j1
