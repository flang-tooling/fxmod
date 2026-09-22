// Shared exception hierarchy and family enum for the fxmod C++ API.
// Kept in its own small header because the low-level parsers (sexpr,
// gfortran, flang, ifx) and the high-level ModuleFile wrapper all need it.
#pragma once

#include <stdexcept>

namespace fxmod {

// Which toolchain's module-file format a file was recognized as.
enum class Family {
  Unknown,
  Gfortran, // gzip-compressed S-expression, GCC 12-16 (module versions
            // "15" and "16" -- see gfortran/module.hpp for what that maps
            // to release-wise)
  Flang,    // BOM + "!mod$ v1 sum:" header + Fortran source body
  Ifx,      // recognized as "not gfortran, not Flang" only; not yet parsed
};

const char *family_name(Family family);

// Base of every error this library throws. Something about the input could
// not be understood -- malformed data, an unreadable file, and so on.
class Error : public std::runtime_error {
public:
  using std::runtime_error::runtime_error;
};

// A more specific case of Error: the input was understood well enough to
// know exactly what it is, but it falls outside what this library
// translates -- an unsupported module version, a generic interface, an ifx
// file (no published format at all). Thrown instead of emitting a
// plausible-but-wrong result: refuse, don't approximate.
class UnsupportedError : public Error {
public:
  using Error::Error;
};

} // namespace fxmod
