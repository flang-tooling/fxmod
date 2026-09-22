module t5
contains
  subroutine bump(y)
    integer, intent(inout) :: y
    y = y + 1
  end subroutine bump
end module t5
