// Turns a parsed ifx module into portable Fortran module source text, the
// same job src/gfortran/fortran_emitter.hpp does for gfortran modules.
//
// Scope, deliberately narrower than the gfortran emitter: only what
// src/ifx/module.hpp actually decodes can be emitted -- scalar
// PARAMETERs and plain variables of any decoded ScalarType, CHARACTER
// variables/dummy-arguments/derived-type fields with an explicit,
// compile-time-constant length (CHARACTER PARAMETERs use
// `character(len=*)` instead, which infers the length from the
// initializer), derived types whose fields are all plain/`#`-tagged (no
// inline default-value expression), fixed-size arrays of any rank that
// aren't PARAMETERs (a PARAMETER array's constant value isn't decoded),
// and subroutine/function interfaces whose dummy arguments are all
// decoded scalars or explicit-length CHARACTER (no arrays, no INTENT --
// intent's flags bit isn't decoded, so it's simply omitted, a weaker but
// still valid declaration). A cross-module (USE-associated) symbol --
// detected by the `NAME+SOURCE_MODULE` compound directory-entry naming
// confirmed on
// real omp_lib.mod -- is satisfied with a `use <module>, only: <name>`
// line rather than redeclared locally, the same approach the gfortran
// emitter uses; this covers a type name referenced by a variable
// declaration or re-exported under this module's own public name, but
// not (yet) a foreign type used as a derived-type field's or dummy
// argument's own type (StructField/StructArg aren't decoded at all).
#pragma once

#include <string>
#include <vector>

#include "module.hpp"

namespace fxmod::ifx {

struct EmitResult {
  std::string source;
  // One entry per top-level entity that could not be translated. Empty
  // means every one made it into `source`.
  std::vector<std::string> problems;
};

// strict = true (default): throws UnsupportedError if any entity could
// not be translated, naming how many and a sample of why.
// strict = false: returns the best-effort source together with the list
// of problems.
EmitResult emit_fortran_source(const Module &module, bool strict = true);

} // namespace fxmod::ifx
