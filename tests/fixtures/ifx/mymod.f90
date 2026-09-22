module mymod
  implicit none

  integer, parameter :: answer = 42
  real, parameter :: pi_approx = 3.14159

contains

  integer function add_answer(x)
    integer, intent(in) :: x
    add_answer = x + answer
  end function add_answer

end module mymod
