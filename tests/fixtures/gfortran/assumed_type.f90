! Assumed-type dummies, and a generic that is only unambiguous because of
! gfortran's NO_ARG_CHECK (the shape of OpenMPI's and OpenACC's choice-
! buffer interfaces).
module assumed_type
  implicit none
  interface send
    module procedure send_any, send_count
  end interface send
contains
  subroutine take_scalar(buf)
    type(*), intent(in) :: buf
  end subroutine take_scalar
  subroutine take_rank(buf)
    type(*), dimension(..), intent(in) :: buf
  end subroutine take_rank
  subroutine send_any(buf, tag)
    type(*), dimension(..), intent(in) :: buf
    integer, intent(in) :: tag
  end subroutine send_any
  subroutine send_count(buf, count, tag)
!GCC$ ATTRIBUTES NO_ARG_CHECK :: buf
    type(*), dimension(*), intent(in) :: buf
    integer, intent(in) :: count, tag
  end subroutine send_count
end module assumed_type
