// Public C++ API. A thin, uniform wrapper over the three format-specific
// readers: detect which family a file belongs to, parse it, and -- the
// library's flagship capability -- regenerate portable Fortran module
// source from it, suitable for feeding to any Fortran compiler (gfortran,
// Flang, and eventually ifx) to have it produce its own native module file.
//
// This header has no dependency on fxmod.h (the C API) or vice versa --
// fxmod.h is implemented in terms of this class, not the other way
// around. Use this header directly from C++; use fxmod.h from C or other
// languages.
#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <fxmod/error.hpp>

namespace fxmod {

// Result of regenerating Fortran source from a parsed module.
struct EmitResult {
  std::string source;
  // One entry per public symbol that could not be translated (gfortran
  // family only; always empty for Flang, since its body is source
  // already). Empty means every public symbol made it into `source`.
  std::vector<std::string> problems;
};

// How emit_fortran_source() translates.
struct EmitOptions {
  // Throw UnsupportedError if any public symbol could not be translated,
  // rather than returning the rest together with the list of problems.
  bool strict = true;
  // Intrinsic types the compiler the source is meant for lacks, as
  // (category, kind): {"REAL", 16} for real(16) (COMPLEX follows REAL).
  // A generic's specifics using one are left out -- that compiler could
  // not call them -- and any other symbol using one is a problem. gfortran
  // modules only; ignored for the other families.
  std::vector<std::pair<std::string, int>> unsupported_kinds;
};

// A parsed module file. Construct with ModuleFile::open(); the concrete
// family is picked automatically from the file's own magic bytes.
class ModuleFile {
public:
  // Detects the family and reads the file. Throws UnsupportedError for an
  // ifx module (recognized, not yet implemented) or an unsupported
  // gfortran module version; throws Error for anything else that doesn't
  // parse (bad gzip, checksum mismatch, malformed S-expression, ...), and
  // for a file that matches none of the known formats.
  static ModuleFile open(const std::string &path);

  Family family() const { return family_; }

  // The format's own version marker: gfortran's module version ("15" or
  // "16"), or "v1" for Flang. Empty if not meaningful.
  const std::string &version() const { return version_; }

  // Best-effort module name (the file's stem, the way both gfortran and
  // Flang derive it).
  const std::string &name() const { return name_; }

  // Regenerates portable Fortran module source. For gfortran modules this
  // walks the parsed symbol table (see the "refuse, never approximate"
  // note on strict below); for Flang modules the body already *is*
  // Fortran source and is returned as-is; for ifx it emits what
  // src/ifx/module.hpp's entity decoders cover -- scalar PARAMETERs and
  // variables, rank-1 fixed arrays, derived types, and procedure
  // interfaces -- and reports anything else (CHARACTER variables, array
  // PARAMETERs, rank > 1, cross-module references, ...) as a problem
  // rather than guessing at it.
  //
  // strict = true (default): throws UnsupportedError if any public symbol
  // could not be translated. strict = false: returns the best-effort
  // source together with the list of what was skipped and why.
  EmitResult emit_fortran_source(bool strict = true) const;
  EmitResult emit_fortran_source(const EmitOptions &options) const;

  // Deeper, format-specific structural access than the uniform API above.
  // Only meaningful when family() == Family::Gfortran; throws Error
  // otherwise. Exposed for tools like `fxmod-cli info` that want to
  // report symbol/flavor counts rather than just emit source.
  struct SymbolSummary {
    std::string name;
    std::string flavor;
    bool is_artificial = false;
  };
  std::vector<SymbolSummary> gfortran_public_symbols() const;

  // Structural access to an ifx module's entity directory. Only
  // meaningful when family() == Family::Ifx; throws Error otherwise. See
  // src/ifx/module.hpp for exactly which fields are decoded and which
  // aren't (most type/value "specifics" are deliberately omitted here for
  // that reason). `value` is the one exception: for a scalar constant of
  // any type ifx supports (decl_rec_kind 12 or 13 -- see
  // ifx::parse_scalar_constant()), it holds that constant's value
  // rendered as Fortran source text (e.g. "42", ".true.", "'hello'",
  // "(1.5,2.5)"); unset for anything else, including a plain
  // (non-constant) variable.
  struct IfxEntitySummary {
    std::string name;
    std::uint32_t decl_rec_kind = 0;
    std::uint32_t sym_id = 0;
    std::uint8_t klass = 0;
    std::uint8_t skind = 0;
    std::optional<std::string> value;

    // decl_rec_kind SimpleScalarArg (a scalar dummy argument): its type,
    // Fortran-spelled (e.g. "integer(4)").
    std::optional<std::string> dummy_arg_type;
    // decl_rec_kind SimpleScalarField/CharField/StructField (a derived
    // type's own component) or SimpleScalarCommon (a COMMON block
    // member): its type, Fortran-spelled. Unset for a SimpleScalarField
    // with its own undecoded default value -- see
    // ifx::SimpleScalarFieldInfo::has_undecoded_initializer.
    std::optional<std::string> field_type;
    // decl_rec_kind Struct (a variable of a derived type): the type's
    // name. Unset if this variable is a PARAMETER (see
    // ifx::StructVariableInfo).
    std::optional<std::string> struct_type_name;
    // decl_rec_kind SimpleScalarArray (a fixed-size array): its element
    // type and dimension count.
    std::optional<std::string> array_element_type;
    std::optional<std::uint32_t> array_size;
    std::optional<std::uint8_t> array_ndimen;
    // Every dimension's bounds, comma-joined in declaration order, e.g.
    // "1:10" or "1:3, 1:4" -- unset for a PARAMETER array (its constant
    // value isn't decoded, see src/ifx/module.hpp).
    std::optional<std::string> array_bounds;
    // decl_rec_kind Subr or FuncRetSimpleScalar (a subroutine/function):
    // set together. `return_type` is unset for a subroutine.
    // `argument_names` is the ordered dummy-argument name list.
    std::optional<bool> is_function;
    std::optional<std::string> return_type;
    std::vector<std::string> argument_names;
    // decl_rec_kind DerivedType (a type definition): its field names, in
    // order. Empty if the type isn't a DerivedType, or if a field uses
    // an inline default-value expression that isn't decoded (see
    // src/ifx/module.hpp's ifx::parse_derived_type()) -- check
    // decl_rec_kind == 11 to tell "not a derived type" from "undecoded".
    std::vector<std::string> field_names;
    // decl_rec_kind Common (a COMMON block): its member names, in order.
    std::vector<std::string> common_variable_names;
  };
  std::vector<IfxEntitySummary> ifx_entities() const;

  ~ModuleFile();
  ModuleFile(ModuleFile &&) noexcept;
  ModuleFile &operator=(ModuleFile &&) noexcept;
  ModuleFile(const ModuleFile &) = delete;
  ModuleFile &operator=(const ModuleFile &) = delete;

private:
  ModuleFile();

  struct Impl;
  std::unique_ptr<Impl> impl_;
  Family family_ = Family::Unknown;
  std::string version_;
  std::string name_;
};

} // namespace fxmod
