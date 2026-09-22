module s2
  type :: t1
    real :: x
  end type t1
  type(t1), external :: ext_func2
end module s2
