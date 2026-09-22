module t1
  type :: t
    integer :: x
  contains
    procedure :: add_int
    procedure :: add_real
    generic :: add => add_int, add_real
  end type t
contains
  function add_int(this, n) result(v)
    class(t), intent(in) :: this
    integer, intent(in) :: n
    integer :: v
    v = this%x + n
  end function add_int
  function add_real(this, r) result(v)
    class(t), intent(in) :: this
    real, intent(in) :: r
    real :: v
    v = this%x + r
  end function add_real
end module t1
