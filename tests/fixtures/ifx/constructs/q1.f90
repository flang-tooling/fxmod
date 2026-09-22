module q1
  type :: t1
    integer :: x
  contains
    procedure :: get_x
  end type t1
contains
  function get_x(this) result(v)
    class(t1), intent(in) :: this
    integer :: v
    v = this%x
  end function get_x
end module q1
