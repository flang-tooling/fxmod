module h2
  type :: point
    real :: x, y
  end type point
contains
  subroutine take_point(p)
    type(point), intent(in) :: p
  end subroutine take_point
end module h2
