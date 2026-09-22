// Reads a gfortran module file: gunzip, split the header line from the body,
// and parse the body as an S-expression forest. Nothing here interprets the
// symbol schema -- that's symbols.hpp -- this layer only owns the version
// gate, because whether the schema decoder in symbols.cpp is even valid for
// this file is decided here, once, before any of it is trusted.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "../sexpr.hpp"

namespace fxmod::gfortran {

// Thrown for anything outside the parser's verified, supported subset.
// Deliberately the same "refuse, don't approximate" exception used
// throughout: an unknown module version is refused, not guessed at.
class UnsupportedError : public fxmod::UnsupportedError {
public:
  using fxmod::UnsupportedError::UnsupportedError;
};

struct Module {
  std::string path;
  // The literal version string from the header, e.g. "15" or "16". Kept as
  // a string (not parsed as an integer) because GCC's own MOD_VERSION is a
  // string and can carry non-numeric suffixes (development snapshots write
  // things like "16s") -- those are read here and then rejected explicitly
  // by read_module(), rather than by failing to parse the header at all.
  std::string version;
  std::string created_from;
  sexpr::List forest;

  // Module name, taken from the file name the way gfortran itself does.
  std::string name() const;
};

// The gfortran module-file schema versions this library has actually
// verified against real GCC source (see the project README / plan notes):
//   "15" -- GCC 12, 13, 14 (module.cc is byte-identical between 12 and 13,
//           and 14 only adds new self-describing attribute names)
//   "16" -- GCC 15.x, 16.1, 16.2 (mio_symbol() unchanged from "15" except
//           one added, self-delimited OpenMP block)
// Anything else -- including GCC's own in-development snapshots, which
// bump this string with every format tweak and don't even stay compatible
// with their own previous snapshot -- is refused rather than guessed at.
bool is_supported_version(const std::string &version);

// True if the file starts with the gzip magic bytes gfortran writes.
bool looks_like_gfortran_module(const std::string &path);

// Gunzip + parse. Throws sexpr::FormatError (or UnsupportedError for an
// unrecognised module version) on anything that doesn't check out.
Module read_module(const std::string &path);

} // namespace fxmod::gfortran
