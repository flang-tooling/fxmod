module f4
contains
  subroutine take_callback(f, x)
    interface
      real function f(x)
        real, intent(in) :: x
      end function f
    end interface
    real, intent(in) :: x
  end subroutine take_callback
end module f4
