module t4
contains
  integer function add2(a, b)
    integer, intent(in) :: a, b
    add2 = a + b
  end function add2
end module t4
