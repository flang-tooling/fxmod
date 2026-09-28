// Turns a parsed gfortran module into portable Fortran module source text --
// the flagship output of this library. The result is plain Fortran; it is
// not tied to any particular compiler's own module-file format. Feed it to
// gfortran, flang, or (eventually) ifx and let that compiler regenerate its
// own native module file.
//
// Follows the same "refuse, never approximate" rule as the rest of this
// project: a symbol outside the translated subset is reported as a problem
// rather than emitted as a plausible-but-wrong declaration.
#pragma once

#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "module.hpp"

namespace fxmod::gfortran {

struct EmitResult {
  std::string source;
  // One entry per public symbol that could not be translated. Empty means
  // every public symbol made it into `source`.
  std::vector<std::string> problems;
};

// strict = true (default): throws UnsupportedError if any public symbol
// could not be translated, naming how many and a sample of why.
// strict = false: returns the best-effort source (everything that could be
// translated) together with the list of problems, letting the caller decide
// what to do about the gap.
//
// `unsupported_kinds`: (category, kind) pairs the target compiler lacks;
// see fxmod::EmitOptions.
// `available_modules`: see fxmod::EmitOptions.
using UnsupportedKinds = std::vector<std::pair<std::string, int>>;
using AvailableModules = std::optional<std::vector<std::string>>;
EmitResult emit_fortran_source(const Module &module, bool strict = true,
                               const UnsupportedKinds &unsupported_kinds = {},
                               const AvailableModules &available_modules = {});

} // namespace fxmod::gfortran
