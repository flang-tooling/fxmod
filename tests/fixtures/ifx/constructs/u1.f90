module u1
  abstract interface
    function callback_t(x) result(y)
      real, intent(in) :: x
      real :: y
    end function callback_t
  end interface
contains
  subroutine apply(f, x)
    procedure(callback_t) :: f
    real, intent(in) :: x
  end subroutine apply
end module u1
