// Interprets the gfortran module schema: the symbol pool and symtree that
// sit at the end of every module body. Layout follows mio_symbol() in
// gcc/fortran/module.cc:
//
//   <number> '<name>' '<module>' '<binding-label>' <flag>
//     ( <attributes> <components> [<component-access>] <typespec> ... )
//
// The component-access atom is present only when the component list is
// non-empty, which is why the typespec position shifts between symbols --
// see parse_symbols(). Verified identical for module versions "15" and
// "16" (see module.hpp); this file does not itself branch on version.
//
// Anything that can reference another symbol by number (a derived-type
// typespec, an array bound that's a dummy argument, a structure
// constructor's type) is deliberately NOT resolved here: the symbol pool
// is a flat, unordered list, so the referenced symbol may not exist yet in
// the map being built. Such things are parsed into an intermediate,
// still-unresolved form (a symbol number, or a raw sexpr::Node) and only
// resolved later in fortran_emitter.cpp, once the whole pool is available.
#pragma once

#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "module.hpp"

namespace fxmod::gfortran {

struct Symbol; // forward declaration; TypeSpec::to_fortran needs the map type

// One `use` statement the emitted source needs, collected while rendering
// rather than known up front: gfortran's module file only ever stores a
// cross-module symbol's *name* and owning module, never that module's own
// source, so a derived type from another module is satisfied by a `use
// <module>, only: <name>` line rather than (unreliably) redeclared
// locally from whatever this module's pool happens to carry for it. Keyed
// by the resolved module name, lowercased, so the same module is only
// requested once no matter how many symbols reference it or in what case
// gfortran spelled it.
struct ModuleUse {
  std::string display_name; // as actually written in the `use` line
  bool intrinsic = false;   // `use, intrinsic ::` vs plain `use`
  std::set<std::string> only_clauses; // e.g. "C_ptr" or "Foo => Bar"
};
using NeededUses = std::map<std::string, ModuleUse>;

// gfortran spells most intrinsic modules (ISO_C_BINDING, ISO_FORTRAN_ENV,
// ...) internally with a leading "__" -- a real `use __iso_c_binding`
// doesn't compile, so that prefix is stripped and the result marked
// `intrinsic`. The IEEE modules it spells plainly, so those are
// recognised by name. Anything else is a plain external module, used
// exactly as spelled.
struct ResolvedModuleName {
  std::string name;
  bool intrinsic = false;
};
ResolvedModuleName resolve_module_name(const std::string &module_name);

// Records that `local_name` (as it needs to appear in this module's own
// output) must come from `module_name` via a `use` statement, resolving
// gfortran's internal intrinsic-module spelling first. `real_name` is the
// symbol's own name in its owning module; when it differs from
// `local_name` (a renaming re-export) the `only:` clause uses Fortran's
// `local => real` rename syntax.
void record_use(NeededUses &uses, const std::string &module_name,
                 const std::string &real_name, const std::string &local_name);

// One dimension's bounds, from mio_array_spec()'s per-dimension mio_expr()
// calls. A bound is either absent (assumed-shape's/deferred's upper bound,
// or a defaulted lower bound), a compile-time constant (rendered
// immediately -- constants never reference another symbol, so there's no
// forward-reference concern), or a reference to another symbol by number
// (typically a dummy argument used as an extent, e.g. `x(n)`) -- kept as
// a number and resolved to a name only at emission time.
struct ArrayBound {
  enum class Kind { Absent, Constant, SymbolRef };
  Kind kind = Kind::Absent;
  std::string constant_text; // valid when kind == Constant
  int symbol_ref = 0;        // valid when kind == SymbolRef
  // For a SymbolRef: a component path below the symbol (`self%n`), as
  // (derived-type symbol, component id) pairs from the reference list.
  std::vector<std::pair<int, int>> components;
};

struct TypeSpec {
  std::string base; // INTEGER, REAL, LOGICAL, CHARACTER, DERIVED, ...
  std::optional<int> kind;
  std::optional<int> derived_ref;
  // CLASS(...) rather than TYPE(...): `derived_ref` then names gfortran's
  // internal __class_* container, whose _data component carries the
  // declared type.
  bool is_class = false;
  // The interface of a procedure entity declared `procedure(iface)`, by
  // symbol number: the typespec's third item. A procedure dummy with such
  // an interface has base "UNKNOWN".
  std::optional<int> interface_ref;
  // CHARACTER length: a constant or a symbol (`len=n`), assumed (`len=*`),
  // deferred (`len=:`), or not decoded (unset, rendered without a length).
  enum class CharLen { None, Bound, Assumed, Deferred };
  CharLen char_len = CharLen::None;
  ArrayBound char_len_bound; // valid when char_len == Bound

  // Renders as a Fortran type-spec, e.g. "integer(4)" or "type(foo)".
  // `current_module` is the module being emitted: a DERIVED reference to a
  // symbol from any *other* module (a USE-associated type, e.g. iso_c_
  // binding's C_ptr) records a needed `use` statement into `uses` instead
  // of inlining anything -- the real type's layout comes from actually
  // using that module at compile time. Throws sexpr::FormatError for an
  // unresolvable derived-type reference, or a base type outside the
  // verified subset -- refuse, don't guess.
  std::string to_fortran(const std::map<int, Symbol> &symbols,
                          const std::string &current_module,
                          NeededUses &uses) const;
};

// Interprets a raw (<BASE> ...) typespec node -- shared by symbol
// typespecs, component typespecs, and expression typespecs (needed to
// resolve a structure-constructor's derived type). Returns nullopt for an
// empty/absent node or an unrecognised base ("UNKNOWN"); never throws --
// use TypeSpec::to_fortran() to turn an unsupported-but-recognised base
// into a refusal.
std::optional<TypeSpec> parse_typespec(const sexpr::Node &node);

// For a CLASS(...) typespec: the pointer/allocatable attribute of the
// entity, which gfortran encodes in the name of its __class_* container
// rather than on the entity ("pointer", "allocatable" or "").
std::string class_attribute(const TypeSpec &ts, const std::map<int, Symbol> &symbols);

struct Component {
  int id = 0; // the number a COMPONENT reference names it by
  std::string name;
  // Unset when the type could not be interpreted; only fatal if the
  // containing type actually has to be emitted.
  std::optional<TypeSpec> typespec;

  // gfortran vtable machinery: _copy, _vptr, _hash, _size, ... has no
  // source spelling and is never emitted.
  bool is_internal() const { return !name.empty() && name.front() == '_'; }
};

struct ArraySpec {
  // EXPLICIT, ASSUMED_SHAPE, DEFERRED, ASSUMED_SIZE, or ASSUMED_RANK --
  // see array_spec_types in module.cc. Coarrays (corank != 0) are refused
  // during parsing rather than represented here.
  std::string type;
  int rank = 0;
  // One (lower, upper) pair per dimension; empty for ASSUMED_RANK, whose
  // "(..)" rendering doesn't need per-dimension bounds at all.
  std::vector<std::pair<ArrayBound, ArrayBound>> dims;

  // Renders the dimension attribute text, e.g. "(10)", "(n, *)", "(:,:)",
  // "(..)". Throws sexpr::FormatError if a SymbolRef bound doesn't
  // resolve to a plain named symbol.
  std::string to_fortran(const std::map<int, Symbol> &symbols) const;
};

// Parses a raw array-spec node -- see mio_array_spec() in module.cc:
//   ( <rank> <corank> <TYPE> [ <lower0> <upper0> <lower1> <upper1> ... ] )
// Returns nullopt for an absent/empty spec (not an array). Throws
// sexpr::FormatError for a corank != 0 (coarray, not decoded) or a bound
// expression that isn't a plain constant or symbol reference (e.g. a
// general arithmetic expression) -- refuse rather than guess.
std::optional<ArraySpec> parse_array_spec(const sexpr::Node &node);

struct Symbol {
  int number = 0;
  std::string name;
  std::string module_name;
  std::vector<std::string> attributes;
  std::optional<TypeSpec> typespec;
  std::vector<Component> components;
  // The raw value expression for a PARAMETER, unresolved (see the file
  // comment) -- e.g. a structure constructor may reference its derived
  // type by a symbol number not yet in the pool map at parse time.
  // Rendered to Fortran text via render_value() in fortran_emitter.cpp.
  std::optional<sexpr::Node> value_node;
  // Symbol numbers of the dummy arguments, in order.
  std::vector<int> formal_args;
  std::optional<ArraySpec> array_spec;
  // A function's result variable, when declared apart from the function
  // (`result(r)`): its attributes (pointer, allocatable, dimension) are
  // the result's.
  std::optional<int> result_ref;
  // The ext_attr bitmask from the attribute list (!GCC$ ATTRIBUTES), bit
  // numbers per ext_attr_id_t in gfortran.h.
  unsigned ext_attr = 0;
  static constexpr unsigned kExtAttrNoArgCheck = 1u << 5;

  // Fortran intent clause for a dummy argument, if it has one. Attribute
  // slot 1 is IN / OUT / INOUT / UNKNOWN-INTENT when DUMMY is set.
  std::optional<std::string> intent() const;
  bool is_dummy() const;
  bool is_generic() const;
  bool is_function() const;
  std::string flavor() const;
  bool is_artificial() const;
};

// Symbols gfortran generates for itself (no source spelling).
extern const char *const kArtificialPrefixes[];
extern const std::size_t kArtificialPrefixCount;
extern const char *const kIntrinsicModule; // "(intrinsic)"

// Pull the symbol pool apart. Entries are a flat run of
//   <number> '<name>' '<module>' '<label>' <flag> ( <body> )
// inside a single top-level list.
std::map<int, Symbol> parse_symbols(const Module &module);

// Public name -> symbol number. The symtree is a flat run of
//   '<name>' <ambiguous-flag> <symbol-number>.
std::map<std::string, int> parse_symtree(const Module &module);

// A named generic mapping to the symbol numbers of its specific
// procedures, from the module's top-level "generic interfaces" section
// (forest[2] -- see write_generic()/load_generic_interfaces() in
// module.cc). Verified against real compiled output for both module
// versions "15" and "16": a flat list of
//   ( '<name>' '<module>' <specific-1> <specific-2> ... )
// entries. This covers two different source patterns that both end up
// here: an explicit `interface NAME ... end interface` block (NAME has no
// symbol-pool entry of its own), and a symbol whose own pool entry is
// separately marked GENERIC (Symbol::is_generic(), found via the symtree
// like any other symbol) -- gfortran's own auto-generated structure-
// constructor generic for a derived type is the latter, which is why
// callers must skip names that match a derived type before treating an
// is_generic() symbol's name as a real user generic.
struct GenericInterface {
  std::string name; // as spelled in the module, for rendering
  // The module that defines the generic: a module re-exporting a generic
  // it USEs lists it here too, under the defining module's name.
  std::string module;
  std::vector<int> specifics;
};

// Keyed by lowercased name (Fortran is case-insensitive; the module file
// preserves whatever case the source used).
std::map<std::string, GenericInterface> parse_generic_interfaces(const Module &module);

// A defined operator or assignment: `spelling` is the generic-spec as
// written in source, e.g. "operator(==)", "operator(.cross.)" or
// "assignment(=)". Intrinsic operators come from the module's first
// top-level section -- one list of specific symbol numbers per
// gfc_intrinsic_op, in enum order, INTRINSIC_USER skipped (see
// write_module() in module.cc) -- and user-defined ones from the second,
// a flat list of ( '<name>' '<module>' <specific-1> ... ) entries.
// Operators gfortran keeps apart but Fortran spells alike (== and .eq.,
// unary and binary +) are merged into one entry, specifics deduplicated.
struct OperatorInterface {
  std::string spelling;
  std::vector<int> specifics;
};

// Throws sexpr::FormatError when the intrinsic-operator section does not
// have the verified shape (one list per operator), rather than guessing
// which operator a list belongs to.
std::vector<OperatorInterface> parse_operator_interfaces(const Module &module);

} // namespace fxmod::gfortran
