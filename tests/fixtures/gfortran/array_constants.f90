! Array constants, of rank 1 and 2, and an integer(8) constant beyond
! default-integer range.
module array_constants
  implicit none
  integer(8), parameter :: big = -9223372036854775807_8
  integer(8), parameter :: dims(2) = [-1_8, 7_8]
  real(8), parameter :: grid(2, 2) = reshape([1d0, 2d0, 3d0, 4d0], [2, 2])
  character(len=2), parameter :: names(3) = ['ab', 'cd', 'ef']
end module array_constants
