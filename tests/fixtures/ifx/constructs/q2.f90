module q2
  type :: t1
    integer :: x
  contains
    procedure, nopass :: get_zero
  end type t1
contains
  function get_zero() result(v)
    integer :: v
    v = 0
  end function get_zero
end module q2
