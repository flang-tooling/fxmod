module g1
contains
  function make_array() result(arr)
    integer :: arr(3)
    arr = [1, 2, 3]
  end function make_array
end module g1
