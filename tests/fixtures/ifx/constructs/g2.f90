module g2
  interface
    real function external_func(x)
      real, intent(in) :: x
    end function external_func
  end interface
end module g2
