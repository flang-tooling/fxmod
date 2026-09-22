module o3
contains
  function make_array(n) result(arr)
    integer, intent(in) :: n
    integer :: arr(3)
    arr = [1, 2, 3]
  end function make_array
end module o3
