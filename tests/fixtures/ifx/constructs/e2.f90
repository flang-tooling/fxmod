module e2
contains
  function make_greeting() result(msg)
    character(len=10) :: msg
    msg = "hi"
  end function make_greeting
end module e2
