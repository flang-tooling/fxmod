! omp_lib differs between compilers: a re-export must not name gfortran's
! entities one by one.
module openmp_reexport
  use omp_lib
  implicit none
end module openmp_reexport
