! REAL and COMPLEX named constants, which gfortran stores as base-16
! MPFR strings.
module real_constants
  implicit none
  real(8), parameter :: fill_double = 9.9692099683868690d+36
  real(4), parameter :: fill_float = 9.9692099683868690e+36
  real(4), parameter :: tenth = -0.1
  real(8), parameter :: third = 1.0d0 / 3.0d0
  real(8), parameter :: zero = 0.0d0
  real(8), parameter :: whole = 42.0d0
  real(8), parameter :: tiny8 = tiny(1.0d0)
  real(8), parameter :: huge8 = huge(1.0d0)
  complex(8), parameter :: zi = (0.5d0, -2.0d0)
  complex(4), parameter :: zf = (1.0e-3, 3.0e5)
end module real_constants
