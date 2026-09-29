! Character lengths (constant, from a dummy, assumed, deferred), and
! character constants with non-printable characters and quotes.
module characters
  implicit none
  character(len=*), parameter :: greeting = 'it''s'//achar(0)//achar(10)//'done'
  character(len=5), parameter :: padded = 'ab'
  character(len=:), allocatable :: deferred
  character(len=12) :: fixed
contains
  subroutine take(s, n, t, u)
    character(len=*), intent(in) :: s
    integer, intent(in) :: n
    character(len=n), intent(out) :: t
    character(len=:), allocatable, intent(out) :: u
  end subroutine take
end module characters
