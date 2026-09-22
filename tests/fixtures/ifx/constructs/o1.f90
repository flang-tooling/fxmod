module o1
contains
  function make_greeting(n) result(msg)
    integer, intent(in) :: n
    character(len=10) :: msg
    msg = "hi"
  end function make_greeting
end module o1
