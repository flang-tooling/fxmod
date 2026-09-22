module k1
  type :: point
    real :: x, y
  end type point
  integer :: a
  character(len=5) :: b
  type(point) :: c
  integer :: d(3)
  type(point) :: e(3)
  character(len=5) :: f(3)
  common /blk/ a, b, c, d, e, f
end module k1
