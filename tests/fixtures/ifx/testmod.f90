! testmod.f90
module testmod
  implicit none
contains
  pure real function square(x)
    real, intent(in) :: x
    square = x * x
  end function square
end module testmod
