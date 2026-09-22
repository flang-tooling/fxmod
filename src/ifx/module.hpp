// Reads ifx module files. There is no published specification for this
// format -- everything here was traced byte offset by byte offset from
// real ifx-produced .mod files, cross-checked wherever possible against
// ifx's own module-dump diagnostics (the compiler can be asked to dump a
// human-readable structural transcript of a module file it just wrote --
// see tests/fixtures/ifx/mymod.mod_dmp for a sample transcript). The
// header/directory/envelope layout is identical between ifx 2026.0.0 and
// 2026.1.1 (a fixture recompiled under 2026.1.1 differs from the
// 2026.0.0 one only in the 8-byte creation timestamp).
//
// Scope: decodes the file header, the entity directory, every entity's
// common envelope, scalar constant values (LOGICAL, INTEGER, REAL4,
// REAL8, REAL16, COMPLEX8, COMPLEX16, CHARACTER -- see
// parse_scalar_constant()), and enough of each entity's own type-
// specific "specifics" region to emit portable Fortran source for a
// meaningful subset of real modules (see src/ifx/fortran_emitter.hpp) --
// derived-type field lists (including nested-derived-type and CHARACTER
// fields), dummy-argument and procedure signatures (including CHARACTER,
// explicit or assumed length), arrays of any rank, and COMMON blocks.
// There are 53 distinct decl_rec_kind "specifics" shapes in total (the
// full list is below); USE-statement rename lists, type-bound
// procedures, PARAMETER arrays, and derived-type field default values
// are still kept as raw bytes, not decoded.
//
// Ground-truth samples: tests/fixtures/ifx/ (mymod.mod/testmod.mod, ifx
// 2026.0.0, plus mymod's own module-dump transcript) and
// tests/fixtures/ifx/constructs/ (t1-t8.mod, ifx 2026.1.1: a plain
// variable, character/logical PARAMETERs, a two-argument function, a
// subroutine, a derived type + a variable of it, a fixed-size array, and
// COMPLEX8/COMPLEX16/REAL16 PARAMETERs), plus ifx's own bundled
// omp_lib.mod / omp_lib_kinds.mod.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <fxmod/error.hpp>

namespace fxmod::ifx {

// Thrown for an ifx file whose major format id isn't "13" (see
// is_supported_major_id()), or when asked for something this reader
// doesn't decode (entity "specifics" beyond scalar constants, and
// therefore Fortran emission).
class UnsupportedError : public fxmod::UnsupportedError {
public:
  using fxmod::UnsupportedError::UnsupportedError;
};

// decl_rec_kind (body offset 0, u32) selects which "specifics" shape an
// entity's body has. The names are the vendor's own literal strings --
// e.g. "entity kind: simple_scalar" -- not labels invented here:
//
//    0 use_stmt                       18 simple_scalar_common
//    1 namelist_group                 19 struct_common
//    2 common                         20 simple_scalar_array_common
//    3 overlay                        21 struct_array_common
//    4 gennode                        22 char_field
//    5 tmpnode                        23 simple_scalar_field
//    6 intrinsic_proc                 24 struct_field
//    7 pub_priv                       25 simple_scalar_array_field
//    8 processor_abstract             26 struct_array_field
//    9 template_abstract              27 char_arg
//   10 file_name (synthetic, see       28 simple_scalar_arg
//      below -- different body shape)  29 struct_arg
//   11 derived_type                    30 simple_scalar_array_arg
//   12 char                            31 struct_array_arg
//   13 simple_scalar                   32 extern_dummy_proc
//   14 struct                          33 derived_type_dummy_proc
//   15 simple_scalar_array             34 simple_scalar_dummy_proc
//   16 struct_array                    35 subr
//   17 char_common                     36 func_ret_char
//                                       37 func_ret_simple_scalar
//                                       38 func_ret_struct
//                                       40 func_ret_simple_scalar_array
//                                       41 func_ret_struct_array
//                                       42 msobjcomment
//                                       43 char_array
//                                       44 char_array_arg
//                                       45 char_array_common
//                                       46 char_array_field
//                                       47 extern_func
//                                       48 extern_derived_type_func
//                                       49 type_bound_proc_field
//                                       51 TBP_gennode
//                                       52 type_bound_generic_field
//
// (39 and 50 have no case in the switch.) 7 is a PUBLIC/PRIVATE
// accessibility statement's bookkeeping, not a real symbol -- it explains
// the odd `uapp(...)`-named directory entries seen in real module files
// with class/skind values outside the table below; this reader doesn't
// special-case it yet, the same way it already needs to for 10.
//
// Decoded below: the common envelope (every kind except FileName), 10's
// own short special body, DerivedType's fixed-width header fields (not
// its field-name list encoding), SimpleScalarArray's fixed-width header
// fields (not its dim_info bounds encoding), and scalar constant values for
// Char and SimpleScalar via parse_scalar_constant(). Every other kind,
// and every `_common`/`_field`/`_arg`/`_array` variant of the ones just
// listed, is left as raw bytes in `specifics`.
enum class DeclRecKind : std::uint32_t {
  UseStmt = 0,
  NamelistGroup = 1,
  Common = 2,
  Overlay = 3,
  Gennode = 4,
  Tmpnode = 5,
  IntrinsicProc = 6,
  PubPriv = 7,
  ProcessorAbstract = 8,
  TemplateAbstract = 9,
  FileName = 10,
  DerivedType = 11,
  Char = 12,
  SimpleScalar = 13,
  Struct = 14,
  SimpleScalarArray = 15,
  StructArray = 16,
  CharCommon = 17,
  SimpleScalarCommon = 18,
  StructCommon = 19,
  SimpleScalarArrayCommon = 20,
  StructArrayCommon = 21,
  CharField = 22,
  SimpleScalarField = 23,
  StructField = 24,
  SimpleScalarArrayField = 25,
  StructArrayField = 26,
  CharArg = 27,
  SimpleScalarArg = 28,
  StructArg = 29,
  SimpleScalarArrayArg = 30,
  StructArrayArg = 31,
  ExternDummyProc = 32,
  DerivedTypeDummyProc = 33,
  SimpleScalarDummyProc = 34,
  Subr = 35,
  FuncRetChar = 36,
  FuncRetSimpleScalar = 37,
  FuncRetStruct = 38,
  // 39 has no case in the switch.
  FuncRetSimpleScalarArray = 40,
  FuncRetStructArray = 41,
  MsObjComment = 42,
  CharArray = 43,
  CharArrayArg = 44,
  CharArrayCommon = 45,
  CharArrayField = 46,
  ExternFunc = 47,
  ExternDerivedTypeFunc = 48,
  TypeBoundProcField = 49,
  // 50 has no case in the switch.
  TbpGennode = 51,
  TypeBoundGenericField = 52,
};

// The envelope's `class` field, e.g. "class:LOCAL" in the vendor's own
// module-dump diagnostic output.
enum class SymbolClass : std::uint8_t {
  Unass = 0,
  Local = 1,
  Common = 2,
  Global = 3,
  Dummy = 4,
  Alloc = 5,
  Abstract = 6,
  Useass = 7,
  Auto = 8,
  File = 9,
  Extern = 10,
};

// The envelope's `skind` field, e.g. "skind:PARAM" in the vendor's own
// module-dump diagnostic output. 50-57 (associate/accumulator-ish
// values) aren't individually named here.
enum class SymbolKind : std::uint8_t {
  Undef = 0,
  Prog = 1,
  Module = 2,
  Subr = 3,
  Func = 4,
  Bdata = 5,
  Cblock = 6,
  Nlgn = 7,
  Efunc = 8,
  Esubr = 9,
  Emodule = 10,
  Lfunc = 11,
  Lsubr = 12,
  Mfunc = 13,
  Msubr = 14,
  Gproc = 15,
  Usename = 16,
  Pubpriv = 17,
  Param = 18,
  Scalar = 19,
  Array = 20,
  Sfunc = 21,
  Ifunc = 22,
  Isubr = 23,
  Scar = 24,
  Scaf = 25,
  Eproc = 26,
  Mproc = 27,
  Dbscal = 28,
  Blkstr = 29,
  Rename = 30,
  Field = 31,
  Bitfield = 32,
  Type90 = 33,
  Vxtype = 34,
  Impltype = 35,
  Template = 36,
  Processor = 37,
  Typfwd = 38,
  Type = 39,
  Dbarra = 40,
  Enuman = 41,
  Var = 42,
  Struct = 43,
  Union = 44,
  Enum = 45,
  Domain = 46,
  DynDomain = 47,
  Label = 48,
  Index = 49,
  Debugsym = 58,
  Nosuchkind = 59,
};

// decl_rec_kind == SimpleScalar's `stype` field (offset 64), and the
// connode's own type code inside parse_scalar_constant() -- e.g.
// "stype:INT4" in the vendor's own module-dump diagnostic output. Only
// the values this reader actually handles are named; many more exist
// (bit fields, task IDs, booleans, pointers, ...) and aren't relevant to
// Fortran source.
enum class ScalarType : std::uint8_t {
  Log1 = 0,
  Log2 = 1,
  Log4 = 2,
  Log8 = 3,
  None = 4,
  Int1 = 5,
  Int2 = 6,
  Int4 = 7,
  Int8 = 8,
  Real4 = 9,
  Real8 = 10,
  Real16 = 11,
  Cmpx8 = 12,
  Cmpx16 = 13,
  Char = 14,
  Void = 40,
};

// One entry from the file's entity directory, plus its body's common
// envelope, at these byte offsets within the entity body:
//
//   offset  0 (u32): decl_rec_kind, see DeclRecKind above.
//   offset  8 (u64): flags -- a raw bitmask; individual bit meanings
//                     (EXPL_TYPE, INTENT, HAS_RESULT, ... per the vendor
//                     dump tool's own names) are not decoded here.
//   offset 56 (u32): sym_id
//   offset 62 (u8):  class, see SymbolClass above.
//   offset 63 (u8):  skind, see SymbolKind above.
//
// decl_rec_kind == FileName is structurally different from every other
// kind -- its body is only 24-28 bytes total, too short for the envelope
// above: decl_rec_kind, an unlabeled reserved u32, then two back-to-back
// NUL-terminated strings (the source file name and the module name), and
// nothing else. flags/sym_id/klass/skind are left at their zero value
// for it, and `specifics` holds everything from offset 4 onward.
//
// Past the envelope, decl_rec_kind == SimpleScalar (non-CHARACTER
// intrinsic types) reads one more byte at offset 64: `stype`, a ScalarType.
//
// decl_rec_kind == Char does NOT use stype/ScalarType -- CHARACTER is
// implied by the kind itself. Instead offset 64 (u32) holds
// `num_nbytes`, the character length. decl_rec_kind == Struct (a
// variable of a derived type) has the same offset-64 `nbytes` shape as
// Char, not the SimpleScalar stype byte -- not decoded further.
//
// decl_rec_kind == DerivedType (the type definition itself) has its own
// fixed header: width (u32@64, the type's total byte size), largest_el
// (u32@72), num_of_fields (u16@80), align (u8@82, indexes an
// alignment-category table), a misaligned flag (u8@83), a has-initializer
// flag (u8@84), howsequenced (u8@85, SEQUENCE/BIND(C)/plain). After that,
// at offset round_up8(86) == 88, num_of_fields back-to-back entries
// encode the field names -- see NamedRef and parse_derived_type() below.
//
// decl_rec_kind == SimpleScalarField (a derived type's own component,
// decl_rec_kind Struct's components are this kind) has: offset (u32@64,
// byte offset within the type), sym_seqno (u16@72), num_of_init_nodes
// (u16@74), type (u8@76, a ScalarType). A nonzero num_of_init_nodes means
// the component has its own default value, encoded separately and not
// yet understood -- not modeled, see parse_simple_scalar_field().
//
// decl_rec_kind == SimpleScalarArray (a fixed-size array of an intrinsic
// type) has: size (u32@64, element count), nbytes (u32@72, per-element
// byte size), type (u8@80, a ScalarType), ndimen (u8@81, dimension count) --
// see parse_simple_array() below. At offset round_up8(82) == 88: if
// skind is Param, a connode-derived constant expression the vendor's own
// diagnostics call "parameter_expression" -- an array-constructor
// encoding (built from node kinds it names AREX/CARRCON/CONTOK/SZLCON)
// that isn't modeled, and whose serialized byte length turns out to vary
// with the constructor's element *values* in a way not yet understood
// (the one piece of decompiled source available for the connode
// describes the compiler's in-memory representation, not this on-disk
// one -- not close enough to trust); otherwise (not a PARAMETER),
// per-dimension bounds are the "dim_info" the vendor's own module-dump
// diagnostics print as five named fields -- offsetExpr, dimen,
// sizeOfDims, distribution, shadow -- each a general compile-time
// expression, the same representation used elsewhere in this format.
// Confirmed byte-for-byte against several real samples with different
// ranks and bounds (fixtures t7.mod = arr(10), probes for a(5), a(2:8),
// and 2-D grid(3,4)/grid2(5,7)): `dimen` alone carries everything needed
// -- offsetExpr/sizeOfDims/distribution/shadow are skipped, not modeled.
// `dimen` is a chain, not a single bracketed pair: `open` (4 bytes --
// tag 2, 1-byte ScalarType, 1-byte unused) then one element, repeated
// ndimen times, then a single `close` (4 bytes, tag 3) -- for ndimen ==
// 1 this degenerates to the simpler "one bracketed element" shape seen
// first. Each element is either a plain 12-byte leaf (2-byte tag 368,
// 1-byte ScalarType, 1-byte unused, 4-byte little-endian value == that
// dimension's extent, when its lower bound is 1) or "COLON(lower,
// upper)" (a 2-child range node, tag 294, printed verbatim under that
// name by the diagnostics, repeating both bounds explicitly -- any other
// lower bound). See parse_array_bounds() below.
//
// decl_rec_kind == Subr (a subroutine) and FuncRetSimpleScalar (a
// function returning an intrinsic scalar) share one header at offset 64:
// num_uses (u16@64), num_abstracts (u16@66), num_types (u16@68),
// num_params (u16@70), num_commons (u16@72), num_intrinsics (u16@74),
// sym_nargu (u16@76, the dummy argument count), a blank-common flag
// (u8@78). FuncRetSimpleScalar additionally has its return type (u8@80,
// a ScalarType) -- Subr does not. After the header (round_up8(79) == 80 for
// Subr, round_up8(81) == 88 for FuncRetSimpleScalar), six back-to-back
// NamedRef lists follow in this order -- uses/bound_vars, abstracts/
// use_procedures, types, params, commons, (one more NamedRef only if the
// blank-common flag is set), intrinsics -- each `num_*` entries long,
// followed finally by the arguments list itself, sym_nargu NamedRefs
// long. See parse_procedure() below, which walks the first six lists
// (needed only to find where arguments starts) without keeping them.
//
// Everything from offset 64 to the end of the body is kept verbatim in
// `specifics` rather than decoded, except for what the functions below
// decode.
struct Entity {
  std::string name;
  DeclRecKind decl_rec_kind = DeclRecKind::UseStmt;
  std::uint64_t flags = 0;
  std::uint32_t sym_id = 0;
  SymbolClass klass = SymbolClass::Unass;
  SymbolKind skind = SymbolKind::Undef;
  std::vector<std::uint8_t> specifics;
};

// A name that may carry an inline symbol-id back-reference, as seen
// throughout the "specifics" region wherever a list of related symbols is
// enumerated (a derived type's field list; a subroutine/function's
// uses/types/params/commons/intrinsics/arguments lists): a plain
// NUL-terminated name, or (when the name is preceded by a `#` byte) a
// name followed by a u32 sym_id pointing at that name's own directory
// entry. `sym_id`, when present, can be cross-referenced against
// Module::entities (matching Entity::sym_id) to look up that name's own
// type/kind.
struct NamedRef {
  std::string name;
  std::optional<std::uint32_t> sym_id;
};

// How a dummy argument is passed. This is an ABI/calling-convention
// detail the vendor's own compiler decided, not a Fortran source
// attribute (Fortran source has no way to
// request "pass by descriptor", and only the explicit VALUE attribute
// requests "by value"); kept for completeness, not needed to emit a
// valid declaration.
enum class PassedBy : std::uint8_t {
  Reference = 0,
  Value = 1,
  Descriptor = 2,
};

// decl_rec_kind == SimpleScalarArg: a dummy argument of an intrinsic,
// non-CHARACTER scalar type.
struct DummyArgInfo {
  ScalarType type = ScalarType::None;
  PassedBy passed_by = PassedBy::Reference;
};

// decl_rec_kind == CharArg: a dummy argument of CHARACTER type. Only two
// shapes are decoded -- see parse_char_arg() below -- an explicit,
// compile-time-constant length (`length` holds it, `is_assumed_length`
// false) or assumed length, `character(*)` (confirmed via the
// IS_ASSUMED_LENGTH flag and a "STAR" leaf, tag 305, in the "len
// expression" region -- `length` is meaningless when this is set). A
// length that's itself a symbol reference isn't decoded.
struct CharArgInfo {
  std::uint32_t length = 0;
  bool is_assumed_length = false;
  PassedBy passed_by = PassedBy::Reference;
};

// decl_rec_kind == StructArg: a dummy argument of a derived type.
struct StructArgInfo {
  std::uint32_t nbytes = 0;
  PassedBy passed_by = PassedBy::Reference;
  std::optional<NamedRef> type_name;
};

// decl_rec_kind == SimpleScalarArrayArg: a dummy argument that's a
// fixed-size array of an intrinsic type. Byte-for-byte the same shape as
// SimpleArrayInfo (size@64, nbytes@72, type@80) with `passed_by`
// inserted before ndimen (which lands at offset 84, not 81 as it does
// for a plain array variable) and the same dim_info bounds encoding
// after it.
struct SimpleArrayArgInfo {
  std::uint32_t size = 0;   // element count
  std::uint32_t nbytes = 0; // per-element byte size
  ScalarType type = ScalarType::None;
  PassedBy passed_by = PassedBy::Reference;
  std::uint8_t ndimen = 0;
};

// decl_rec_kind == StructArrayArg: a dummy argument that's a fixed-size
// array of a derived type.
struct StructArrayArgInfo {
  std::uint32_t size = 0;   // element count
  std::uint32_t nbytes = 0; // per-element byte size
  PassedBy passed_by = PassedBy::Reference;
  std::uint8_t ndimen = 0;
  std::optional<NamedRef> type_name;
};

// decl_rec_kind == CharArrayArg: a dummy argument that's a fixed-size
// array of CHARACTER. Only a constant (non-symbolic) length is decoded --
// like CharArgInfo's assumed-length case, a length that's itself a symbol
// reference isn't understood yet, so callers should refuse rather than
// guess.
struct CharArrayArgInfo {
  std::uint32_t size = 0; // element count
  std::uint32_t length = 0;
  std::uint8_t ndimen = 0;
};

// decl_rec_kind == Subr or FuncRetSimpleScalar: a procedure's signature.
// `return_type` is unset for a Subr. `arguments` is the ordered dummy
// argument list; look each one up by `sym_id` (via Module::entities) and
// decode it with parse_dummy_arg() for its own type.
struct ProcedureInfo {
  std::optional<ScalarType> return_type;
  std::vector<NamedRef> arguments;
};

// decl_rec_kind == FuncRetChar: a function returning CHARACTER. The
// return length sits at the usual CHARACTER position (specifics[0], like
// Char/CharArg/CharField), then 16 reserved zero bytes, then a dummy-
// argument count (u32, confirmed 2026-09-17 by comparing a zero- and a
// one-argument sample byte-for-byte), then that many `#`-tagged
// `NamedRef`s -- each resolves (via Module::entities) to its own
// CharArg/StructArg/SimpleScalarArrayArg/.../plain dummy-arg entity,
// exactly like a Subr/FuncRetSimpleScalar's own "arguments" list. A
// symbolic (non-constant) return length -- e.g. `character(len=n)`
// where `n` is itself a dummy argument -- would show up in a "len
// expression" that immediately follows the arguments list; not sampled,
// so `parse_func_ret_char()` doesn't attempt it (length stays whatever
// the fixed field says, which may be stale/meaningless in that case --
// callers relying on `length` should treat a symbolic-length function as
// an open risk until that's confirmed too).
struct FuncRetCharInfo {
  std::uint32_t length = 0;
  std::vector<NamedRef> arguments;
};

// decl_rec_kind == FuncRetStruct: a function returning a derived type.
// nbytes sits at the usual Struct position (specifics[0]), then 16
// reserved zero bytes, then a dummy-argument count and list -- same
// shape and same confirmation as FuncRetCharInfo above -- immediately
// followed by `type_name`. A function-local (as opposed to module-
// scope) result type isn't sampled and isn't distinguished here.
struct FuncRetStructInfo {
  std::uint32_t nbytes = 0;
  std::vector<NamedRef> arguments;
  std::optional<NamedRef> type_name;
};

// decl_rec_kind == FuncRetSimpleScalarArray: a function returning a
// fixed-size array of an intrinsic type. size/nbytes sit at the usual
// SimpleArrayInfo position (specifics[0]/[8]); what was once described
// as "an 18-byte gap, first byte 1, rest 0" turned out (confirmed
// 2026-09-17, same before/after comparison as FuncRetCharInfo) to be: a
// constant byte (still unexplained) + 13 reserved zero bytes + a dummy-
// argument count (u32) -- i.e. the "rest 0" was just "zero arguments" in
// disguise. `type`/`ndimen` immediately follow the count field at their
// usual fixed position either way; then, padded to the next 8-byte
// boundary, the arguments list (if any), then `dim_info` right after
// (wherever that ends up).
struct FuncRetSimpleScalarArrayInfo {
  std::uint32_t size = 0;   // element count
  std::uint32_t nbytes = 0; // per-element byte size
  ScalarType type = ScalarType::None;
  std::uint8_t ndimen = 0;
  std::vector<NamedRef> arguments;
};

// decl_rec_kind == FuncRetStructArray: a function returning a fixed-size
// array of a derived type. Same family, same confirmed argument-count
// fix as FuncRetSimpleScalarArrayInfo -- size/nbytes at the usual
// position, then the constant byte + 13 reserved zero bytes + argument
// count, then `ndimen` (no separate type byte -- always a derived type),
// then (padded to 8 bytes) the arguments list, then `type_name`
// immediately after that (in place of a ScalarType), then `dim_info`.
struct FuncRetStructArrayInfo {
  std::uint32_t size = 0;   // element count
  std::uint32_t nbytes = 0; // per-element byte size (the element type's own width)
  std::uint8_t ndimen = 0;
  std::vector<NamedRef> arguments;
  std::optional<NamedRef> type_name;
};

// decl_rec_kind == DerivedType: a type definition's field list.
// `fields[i].sym_id`, when present, resolves (via Module::entities) to
// that field's own SimpleScalarField entry. nullopt overall means at
// least one field used a `%`-tagged inline default-value expression,
// which isn't modeled -- refused rather than guessed at partially, same
// rule as everywhere else in this library.
using DerivedTypeFields = std::vector<NamedRef>;

// decl_rec_kind == SimpleScalarField: one component of a derived type.
struct SimpleScalarFieldInfo {
  std::uint32_t offset = 0; // byte offset within the type
  ScalarType type = ScalarType::None;
  // A pre-formatted Fortran literal for the component's own default
  // value (a `type :: T; integer :: a = 42; end type T`-style
  // initializer), when present and decodable -- same confirmed value
  // encoding and per-type support as parse_array_constant()'s PARAMETER-
  // array values (INTEGER/LOGICAL/REAL4/REAL8; not REAL16/COMPLEX/
  // CHARACTER), narrowly scoped the same way to a single value (`count
  // == 1 && ncopies == 1` in the vendor diagnostic's own terms; not an
  // array-valued or RESHAPE-style default). unset when there's no
  // default value at all.
  std::optional<std::string> default_value;
  // True if the component has a default value whose encoding didn't
  // match the confirmed shape above -- callers should treat the type as
  // not fully decodable when this is set (an initializer exists but its
  // value can't be trusted).
  bool has_undecoded_initializer = false;
};

// decl_rec_kind == TypeBoundProcField: a type-bound procedure binding
// (`procedure :: NAME` or `procedure, nopass :: NAME` inside a
// `type ... contains ... end type`) -- the directory name is
// "OWNERTYPE%BINDINGNAME" like any other field, and that BINDINGNAME
// (not `interface_symbol`'s own name) is what `T%BINDINGNAME(...)` calls
// resolve to; distinct from `interface_symbol` only when the source used
// `procedure :: BINDINGNAME => IMPLNAME`. `passed_arg` (confirmed
// 2026-09-17 by compiling both ways with real ifx) determines whether
// the implementing procedure's own passed-object dummy argument (its
// FIRST argument, PASS(name) forms aren't sampled) must be declared
// `class(OWNERTYPE)` rather than `type(OWNERTYPE)` -- required by the
// language for any extensible type (ifx itself refuses a `type(...)`
// passed-object argument with error #8264). A GENERIC binding uses a
// different decl_rec_kind entirely (TypeBoundGenericField, see below),
// not this one.
struct TypeBoundProcFieldInfo {
  bool passed_arg = false;
  std::optional<NamedRef> interface_symbol;
};

// decl_rec_kind == TypeBoundGenericField: a GENERIC type-bound binding
// (`generic :: NAME => proc1, proc2` inside a `type ... contains ...
// end type`). The directory name is "OWNERTYPE%NAME" like any other
// field, giving the binding's own name directly. This entity's own
// specifics carry almost nothing (just `offset`/`seqno`, matching
// TypeBoundProcField's own header shape but nothing past it) -- the
// actual generic-procedure list lives in a SEPARATE entity, confirmed
// to always be the very next one in the module's own entity list
// (decl_rec_kind == TbpGennode, directory name "gen@BINDINGNAME", own
// sym_id always 0 -- i.e. NOT independently resolvable by sym_id, only
// by this positional adjacency). See TbpGennodeInfo below.
struct TypeBoundGenericFieldInfo {
  std::uint32_t seqno = 0;
};

// decl_rec_kind == TbpGennode: the specific-procedure list for the
// TypeBoundGenericField entity immediately preceding it (see its
// comment above for how the two are paired -- positionally, not by
// sym_id). `generic_operator`/`gen_stacod` are decoded but only the one
// confirmed combination is accepted: a plain named generic (not an
// operator overload like `OPERATOR(+)` or `ASSIGNMENT(=)`, unconfirmed)
// -- `generic_operator == kGenericOperatorPlainName`. `gen_stacod`
// (confirmed value: a "generic function" marker) is decoded but not
// gated on, since Fortran's `generic ::` binding statement syntax is
// identical regardless of whether the specifics are functions or
// subroutines. `gen_procs` holds each specific procedure's own BINDING
// name (already what `generic :: NAME => ...` needs verbatim, no sym_id
// resolution required -- these reference other bindings on the same
// type, which are emitted independently).
struct TbpGennodeInfo {
  std::uint16_t generic_operator = 0;
  std::uint16_t gen_stacod = 0;
  std::optional<NamedRef> parent;
  std::vector<NamedRef> gen_procs;
};

// TbpGennodeInfo::generic_operator's only confirmed value: a plain named
// generic (`generic :: NAME => ...`), as opposed to an operator overload
// (`generic :: operator(+) => ...`) or ASSIGNMENT(=), neither sampled.
constexpr std::uint16_t kGenericOperatorPlainName = 373;

// decl_rec_kind == CharField: a derived type's own CHARACTER component.
struct CharFieldInfo {
  std::uint32_t offset = 0; // byte offset within the type
  std::uint32_t length = 0;
  // A pre-formatted Fortran literal for the component's own default
  // value (e.g. `character(len=5) :: s = 'hello'`), when present and
  // decodable -- same confirmed value encoding, initializer wrapper
  // shape, and narrow single-value scope as SimpleScalarFieldInfo's own
  // `default_value` (see its comment), just starting at a different
  // fixed offset (CharField's own header is shorter). unset when there's
  // no default value at all.
  std::optional<std::string> default_value;
  // True if the component has a default value whose encoding didn't
  // match the confirmed shape above.
  bool has_undecoded_initializer = false;
};

// decl_rec_kind == Struct: a variable of a derived type. `type_name` is
// unset only when the variable is a PARAMETER (skind == Param) -- that
// path calls a connode reader whose struct-shaped layout isn't confirmed
// this way (see parse_scalar_constant(), which only handles Char and
// SimpleScalar).
struct StructVariableInfo {
  std::uint32_t nbytes = 0;
  std::optional<NamedRef> type_name;
};

// decl_rec_kind == ExternFunc: a module-scope `TYPE, EXTERNAL :: NAME`
// declaration -- an external (non-module) function referenced only by a
// plain EXTERNAL/type declaration, with no dummy-argument info at all
// (Fortran's EXTERNAL attribute doesn't require or carry one; the call
// site's own actual arguments are what get checked, or not, against an
// implicit interface). `sym_nargu` is decoded but not otherwise used --
// always seen 0, meaning isn't confirmed since a plain EXTERNAL
// declaration has nothing that would set it either way.
struct ExternFuncInfo {
  std::uint32_t nbytes = 0; // the return type's own byte width
  std::uint16_t sym_nargu = 0;
  ScalarType type = ScalarType::None;
};

// decl_rec_kind == ExternDerivedTypeFunc: same as ExternFuncInfo, for a
// `TYPE(T), EXTERNAL :: NAME` declaration (a derived-type return type).
struct ExternDerivedTypeFuncInfo {
  std::uint32_t nbytes = 0;
  std::uint16_t sym_nargu = 0;
  std::optional<NamedRef> type_name;
};

// decl_rec_kind == StructField: a derived type's own component that is
// itself of another derived type (nested derived types).
struct StructFieldInfo {
  std::uint32_t offset = 0; // byte offset within the owning type
  std::uint32_t nbytes = 0;
  std::optional<NamedRef> type_name;
};

// decl_rec_kind == SimpleScalarArrayField: a derived type's own
// component that is a fixed-size array of an intrinsic type. Unlike
// SimpleScalarField/CharField/StructField, no `offset` is present in the
// confirmed byte layout.
struct SimpleScalarArrayFieldInfo {
  std::uint32_t size = 0;   // element count
  std::uint32_t nbytes = 0; // per-element byte size
  ScalarType type = ScalarType::None;
  std::uint8_t ndimen = 0;
};

// decl_rec_kind == StructArrayField: a derived type's own component that
// is a fixed-size array of another derived type.
struct StructArrayFieldInfo {
  std::uint32_t size = 0;   // element count
  std::uint32_t offset = 0; // byte offset within the owning type
  std::uint32_t nbytes = 0; // per-element byte size
  std::uint8_t ndimen = 0;
  std::optional<NamedRef> type_name;
};

// decl_rec_kind == Common: a COMMON block's own directory entry (whose
// name is the compound "MODULE!BLOCKNAME"). The member list; resolve
// each by sym_id (via Module::entities) and decode with
// parse_common_variable() for its own type.
using CommonBlockVariables = std::vector<NamedRef>;

// decl_rec_kind == SimpleScalarCommon: one COMMON block member.
struct CommonVariableInfo {
  std::uint32_t offset = 0; // byte offset within the block
  ScalarType type = ScalarType::None;
};

// decl_rec_kind == CharCommon: a COMMON block's own CHARACTER member.
// Byte-for-byte the same shape as CharFieldInfo.
struct CharCommonInfo {
  std::uint32_t length = 0;
  std::uint32_t offset = 0; // byte offset within the block
};

// decl_rec_kind == StructCommon: a COMMON block member of a derived type.
struct StructCommonInfo {
  std::uint32_t nbytes = 0;
  std::uint32_t offset = 0; // byte offset within the block
  std::optional<NamedRef> type_name;
};

// decl_rec_kind == SimpleScalarArrayCommon: a COMMON block member that's
// a fixed-size array of an intrinsic type.
struct SimpleScalarArrayCommonInfo {
  std::uint32_t size = 0;   // element count
  std::uint32_t nbytes = 0; // per-element byte size
  std::uint32_t offset = 0; // byte offset within the block
  ScalarType type = ScalarType::None;
  std::uint8_t ndimen = 0;
};

// decl_rec_kind == StructArrayCommon: a COMMON block member that's a
// fixed-size array of a derived type.
struct StructArrayCommonInfo {
  std::uint32_t size = 0;   // element count
  std::uint32_t nbytes = 0; // per-element byte size
  std::uint32_t offset = 0; // byte offset within the block
  std::uint8_t ndimen = 0;
  std::optional<NamedRef> type_name;
};

// decl_rec_kind == CharArrayCommon: a COMMON block member that's a
// fixed-size array of CHARACTER. Only a constant (non-symbolic) length
// is decoded, same caveat as CharArgInfo/CharArrayArgInfo.
struct CharArrayCommonInfo {
  std::uint32_t size = 0; // element count
  std::uint32_t length = 0;
  std::uint32_t offset = 0; // byte offset within the block
  std::uint8_t ndimen = 0;
};

// decl_rec_kind == CharArrayField: a derived type's own component that's
// a fixed-size array of CHARACTER. Byte-for-byte the same shape as
// SimpleScalarArrayFieldInfo (`nbytes` here is the CHARACTER length) --
// no `offset` present, same as that kind.
struct CharArrayFieldInfo {
  std::uint32_t size = 0; // element count
  std::uint32_t length = 0;
  std::uint8_t ndimen = 0;
};

// decl_rec_kind == Overlay: one EQUIVALENCE group, from the point of view
// of a single "owner" member -- confirmed (2-way and 3-way, and a chain
// of pairwise `equivalence` statements that ifx transitively closes into
// one group) to list every OTHER member of the same storage group,
// making each member's own Overlay entity redundant with every other
// member's (a fully-connected clique, not a chain of pairs). Unlike every
// other decl_rec_kind, this entity's body carries no common envelope at
// all (body_len is too short, same special case as FileName) -- no
// sym_id/class/skind of its own; the owning variable's identity is only
// recoverable from this entity's own compound directory name
// ("#OWNERNAME-ovl#OWNERSYMID#N", not yet documented above with the
// other compound-name conventions since it's the only one with a leading
// '#'). Only `offset == 0` for every member is confirmed (a partial
// overlap via a nonzero offset -- e.g. two differently-sized types, or
// an array member -- isn't).
struct OverlayInfo {
  std::uint32_t offset = 0;
  std::vector<NamedRef> ovl_syms; // every OTHER member of the group
};

// decl_rec_kind == NamelistGroup: a NAMELIST group's member list, in
// declaration order. Each member is independently declared elsewhere
// (its own ordinary SimpleScalar/Char/... entity, at module scope, no
// component-style compound name) -- this is only the `namelist /name/
// ...` statement's own member-name list.
using NamelistVariables = std::vector<NamedRef>;

// decl_rec_kind == SimpleScalarArray: the fixed-width header fields only
// -- see the file comment above for why per-dimension bounds aren't
// decoded yet. `is_parameter` mirrors skind == Param (the array has a
// connode value, itself not decoded for arrays -- parse_scalar_constant()
// only handles Char and SimpleScalar).
struct SimpleArrayInfo {
  std::uint32_t size = 0; // element count
  std::uint32_t element_bytes = 0;
  ScalarType type = ScalarType::None;
  std::uint8_t ndimen = 0;
  bool is_parameter = false;
};

// decl_rec_kind == StructArray: a fixed-size array of a derived type.
// Same shape as SimpleArrayInfo but with `type_name` (a NamedRef to the
// element's own derived type) in place of a ScalarType -- the vendor's
// own module-dump diagnostics mislabel one field here (offset 64 is
// printed as both "size" and, confusingly, "type:LOG1" -- a real quirk
// in that tool, not something this reader repeats; byte-for-byte tracing
// confirms 64 is genuinely `size`, matching SimpleArrayInfo). Only
// ndimen == 1 is decoded (parse_struct_array() refuses otherwise) --
// past `dimen`'s own chain and a redundant `sizeOfDims` repeat sits a
// fixed 16-byte gap before `type_name`, confirmed zero for one real
// sample but not yet checked against a rank > 1 one to know whether that
// gap scales with rank the way `dimen`/`sizeOfDims` do.
struct StructArrayInfo {
  std::uint32_t size = 0;          // element count
  std::uint32_t element_bytes = 0; // the element type's own byte width
  std::uint8_t ndimen = 0;
  std::optional<NamedRef> type_name;
};

// decl_rec_kind == CharArray: a fixed-size array of CHARACTER. Byte-for-
// byte the same layout as SimpleArrayInfo (size@64, nbytes@72 -- here
// the per-element CHARACTER length -- type@80 always CHAR and not
// exposed separately, ndimen@81) and the same dim_info bounds encoding
// -- see parse_char_array_bounds() below.
struct CharArrayInfo {
  std::uint32_t size = 0;   // element count
  std::uint32_t length = 0; // per-element CHARACTER length
  std::uint8_t ndimen = 0;
  bool is_parameter = false;
};

// Renders a ScalarType as a Fortran type-spec, e.g. "integer(4)", "real(8)",
// "complex(8)", "character". CMPX8/CMPX16 name their own total byte
// width (matching parse_scalar_constant()'s two-float32/two-float64
// reading), so they map to Fortran COMPLEX kinds 4 and 8 (each half that
// width) respectively -- confirmed against tests/fixtures/ifx/
// constructs/t8.mod. Returns nullopt for a ScalarType with no Fortran
// spelling here (Void and anything not in the table).
std::optional<std::string> to_fortran_type(ScalarType type);

std::optional<DummyArgInfo> parse_dummy_arg(const Entity &e);
std::optional<ProcedureInfo> parse_procedure(const Entity &e);
std::optional<FuncRetCharInfo> parse_func_ret_char(const Entity &e);
std::optional<FuncRetStructInfo> parse_func_ret_struct(const Entity &e);
std::optional<FuncRetSimpleScalarArrayInfo>
parse_func_ret_simple_scalar_array(const Entity &e);
std::optional<FuncRetStructArrayInfo> parse_func_ret_struct_array(const Entity &e);
std::optional<DerivedTypeFields> parse_derived_type(const Entity &e);
std::optional<SimpleScalarFieldInfo> parse_simple_scalar_field(const Entity &e);
std::optional<StructVariableInfo> parse_struct_variable(const Entity &e);
std::optional<ExternFuncInfo> parse_extern_func(const Entity &e);
std::optional<ExternDerivedTypeFuncInfo>
parse_extern_derived_type_func(const Entity &e);
std::optional<StructFieldInfo> parse_struct_field(const Entity &e);
std::optional<SimpleScalarArrayFieldInfo>
parse_simple_scalar_array_field(const Entity &e);
std::optional<StructArrayFieldInfo> parse_struct_array_field(const Entity &e);
std::optional<SimpleArrayInfo> parse_simple_array(const Entity &e);
std::optional<StructArrayInfo> parse_struct_array(const Entity &e);
std::optional<CharArrayInfo> parse_char_array(const Entity &e);

// decl_rec_kind == Common: a COMMON block's member list, in declaration
// order. Each entry's sym_id resolves (via Module::entities) to that
// member's own SimpleScalarCommon entry.
std::optional<CommonBlockVariables> parse_common_block(const Entity &e);

// decl_rec_kind == SimpleScalarCommon: one COMMON block member.
std::optional<CommonVariableInfo> parse_common_variable(const Entity &e);
std::optional<CharCommonInfo> parse_char_common(const Entity &e);
std::optional<StructCommonInfo> parse_struct_common(const Entity &e);
std::optional<SimpleScalarArrayCommonInfo>
parse_simple_scalar_array_common(const Entity &e);
std::optional<StructArrayCommonInfo> parse_struct_array_common(const Entity &e);
std::optional<CharArrayCommonInfo> parse_char_array_common(const Entity &e);

// decl_rec_kind == NamelistGroup: the group's member list.
std::optional<NamelistVariables> parse_namelist_group(const Entity &e);

// decl_rec_kind == Overlay: see OverlayInfo above.
std::optional<OverlayInfo> parse_overlay(const Entity &e);

// decl_rec_kind == CharArg: see CharArgInfo above for which two shapes
// are decoded. Refuses (nullopt) for anything else -- a length that's
// itself a symbol reference isn't understood yet.
std::optional<CharArgInfo> parse_char_arg(const Entity &e);
std::optional<StructArgInfo> parse_struct_arg(const Entity &e);
std::optional<SimpleArrayArgInfo> parse_simple_array_arg(const Entity &e);
std::optional<StructArrayArgInfo> parse_struct_array_arg(const Entity &e);
std::optional<CharArrayArgInfo> parse_char_array_arg(const Entity &e);

// decl_rec_kind == CharField: a derived type's own CHARACTER component.
std::optional<CharFieldInfo> parse_char_field(const Entity &e);
std::optional<CharArrayFieldInfo> parse_char_array_field(const Entity &e);

// decl_rec_kind == TypeBoundProcField: see TypeBoundProcFieldInfo above.
std::optional<TypeBoundProcFieldInfo>
parse_type_bound_proc_field(const Entity &e);

// decl_rec_kind == TypeBoundGenericField: see TypeBoundGenericFieldInfo
// above.
std::optional<TypeBoundGenericFieldInfo>
parse_type_bound_generic_field(const Entity &e);

// decl_rec_kind == TbpGennode: see TbpGennodeInfo above.
std::optional<TbpGennodeInfo> parse_tbp_gennode(const Entity &e);

// One dimension's bounds.
struct ArrayBound {
  std::int64_t lower = 1;
  std::int64_t upper = 0;
};

// decl_rec_kind == SimpleScalarArray's bounds, one entry per dimension
// in declaration order (dims(0) is the fastest-varying, matching
// Fortran's own column-major convention). nullopt for a PARAMETER array
// (see the file comment above) or if the "dimen" chain doesn't match the
// confirmed shape.
std::optional<std::vector<ArrayBound>> parse_array_bounds(const Entity &e);

// decl_rec_kind == SimpleScalarArray, skind == Param: the array's own
// PARAMETER value, one pre-formatted Fortran literal per element in
// declaration order -- same rendering rules as parse_scalar_constant()
// (reuses format_integer_literal()/format_real_literal() via
// render_contok_value()). Confirmed shape: a "parameter_expression"
// (AREX(CARRCON(1,[CONTOK...]),SZLCON([N]))) sits where dim_info would
// otherwise start (body offset 136, a fixed position after a reserved
// gap that doesn't vary with element count or value), and dim_info
// itself starts wherever that expression ends -- its length varies with
// each element's value (specifically the decimal-digit count of its
// ASCII text for a numeric type, or the string's own byte length for
// CHARACTER). INTEGER (Int1/Int2/Int4/Int8), LOGICAL (Log1/Log2/Log4/
// Log8), REAL4/REAL8 and CHARACTER element types are understood, any
// rank -- refuses (nullopt) for anything else (REAL16/COMPLEX elements
// aren't sampled). A CHARACTER PARAMETER array is confirmed to use this
// same decl_rec_kind (15) rather than CharArray (43) -- unlike a
// non-PARAMETER CHARACTER array variable, which does use CharArray; the
// array's own declared length is `element_bytes` (from
// parse_simple_array()), the same field a non-CHARACTER element's
// per-element byte size occupies.
std::optional<std::vector<std::string>> parse_array_constant(const Entity &e);

// decl_rec_kind == SimpleScalarArray, skind == Param: the array's own
// bounds -- same "dim_info" shape as parse_array_bounds() above, just
// starting after the "parameter_expression" (see parse_array_constant()'s
// comment) instead of at the fixed offset a non-PARAMETER array uses.
std::optional<std::vector<ArrayBound>>
parse_array_constant_bounds(const Entity &e);

// decl_rec_kind == StructArray's bounds -- same "dim_info" shape as
// parse_array_bounds() above (dimen alone is read; StructArray's
// trailing "sizeOfDims" repeat, same shape, is walked past but its
// values aren't needed here either).
std::optional<std::vector<ArrayBound>> parse_struct_array_bounds(const Entity &e);

// decl_rec_kind == CharArray's bounds -- byte-for-byte the same as
// parse_array_bounds() above.
std::optional<std::vector<ArrayBound>> parse_char_array_bounds(const Entity &e);

// decl_rec_kind == FuncRetSimpleScalarArray's bounds -- byte-for-byte
// the same as parse_array_bounds() above.
std::optional<std::vector<ArrayBound>>
parse_func_ret_simple_scalar_array_bounds(const Entity &e);

// decl_rec_kind == FuncRetStructArray's bounds -- same "dim_info" shape,
// starting right after `type_name`.
std::optional<std::vector<ArrayBound>>
parse_func_ret_struct_array_bounds(const Entity &e);

// decl_rec_kind == SimpleScalarArrayArg's bounds -- same "dim_info"
// shape as parse_array_bounds() above.
std::optional<std::vector<ArrayBound>>
parse_simple_array_arg_bounds(const Entity &e);

// decl_rec_kind == SimpleScalarArrayField's bounds -- same "dim_info"
// shape as parse_array_bounds() above.
std::optional<std::vector<ArrayBound>>
parse_simple_scalar_array_field_bounds(const Entity &e);

// decl_rec_kind == StructArrayField's bounds -- same "dim_info" shape,
// starting right after `type_name` (same ordering as
// parse_func_ret_struct_array_bounds() above); the trailing "sizeOfDims"
// repeat is walked past but not needed here either.
std::optional<std::vector<ArrayBound>>
parse_struct_array_field_bounds(const Entity &e);

// decl_rec_kind == StructArrayArg's bounds -- same "dim_info" shape as
// parse_array_bounds() above (dim_info comes before `type_name` here,
// unlike StructArrayField).
std::optional<std::vector<ArrayBound>>
parse_struct_array_arg_bounds(const Entity &e);

// decl_rec_kind == CharArrayArg's bounds -- same "dim_info" shape as
// parse_array_bounds() above.
std::optional<std::vector<ArrayBound>>
parse_char_array_arg_bounds(const Entity &e);

// decl_rec_kind == SimpleScalarArrayCommon's bounds -- same "dim_info"
// shape as parse_array_bounds() above.
std::optional<std::vector<ArrayBound>>
parse_simple_scalar_array_common_bounds(const Entity &e);

// decl_rec_kind == StructArrayCommon's bounds -- same "dim_info" shape as
// parse_array_bounds() above; type_name follows (like StructArray).
std::optional<std::vector<ArrayBound>>
parse_struct_array_common_bounds(const Entity &e);

// decl_rec_kind == CharArrayCommon's bounds -- same "dim_info" shape as
// parse_array_bounds() above.
std::optional<std::vector<ArrayBound>>
parse_char_array_common_bounds(const Entity &e);

// decl_rec_kind == CharArrayField's bounds -- same "dim_info" shape as
// parse_array_bounds() above.
std::optional<std::vector<ArrayBound>>
parse_char_array_field_bounds(const Entity &e);

struct Module {
  std::string path;
  // Only "13" is accepted -- see is_supported_major_id().
  int major_id = 0;
  int minor_id = 0;
  std::uint32_t target_arch = 0; // raw index into a string table this
                                  // reader does not resolve
  std::uint32_t compiler_edit = 0;
  std::string gemver;
  std::string compiler_ver;
  std::vector<Entity> entities;

  std::string name() const;
};

// decl_rec_kind == SimpleScalar's own declared type (`stype`, body
// offset 64), independent of whether it's a PARAMETER -- unlike the
// connode read by parse_scalar_constant() below, which only exists for
// one. Returns nullopt for anything but SimpleScalar.
std::optional<ScalarType> parse_scalar_type(const Entity &e);

// decl_rec_kind == Char's own declared length (`num_nbytes`, body offset
// 64), independent of whether it's a PARAMETER -- present unconditionally,
// unlike the connode read by parse_scalar_constant() below. Returns
// nullopt for anything but Char.
std::optional<std::uint32_t> parse_char_length(const Entity &e);

// Renders a scalar constant's value as Fortran source text -- e.g. `42`,
// `.true.`, `3.14159`, `'hello'`, `(1.5,2.5)`. Only meaningful for
// decl_rec_kind Char or SimpleScalar; returns nullopt for anything else,
// or for a ScalarType this doesn't decode -- refuse rather than guess, same
// rule as everywhere else in this library.
//
// Traced against real .mod files for every type below. Offsets are
// relative to the "connode" substructure (this library's own term), which
// both decl_rec_kind Char
// and SimpleScalar place at body offset 72, i.e. specifics[8] (specifics
// starts at body offset 64):
//
//   connode+13 (specifics[21], u8): the value's own type code, a ScalarType
//     (same table as the envelope's stype). Handled: Log1/Log2/Log4/Log8,
//     Int1/Int2/Int4/Int8 (all read as the same 8-byte int64 regardless
//     of declared width), Real4 (a raw float32), Real8 (a raw float64),
//     Real16 (binary128, narrowed to double -- see f128_to_double()),
//     Cmpx8 (two back-to-back float32, real then imaginary), Cmpx16 (two
//     back-to-back float64), Char.
//   connode+32 (specifics[40]): the binary value for LOG*/INT* (int64),
//     REAL4 (float32), REAL8 (float64), or REAL16 (16 bytes, binary128).
//     For COMPLEX8/16 this is the real part, with the imaginary part
//     immediately following (at +4 or +8 respectively).
//   connode+24 (specifics[32], low 32 bits of what the vendor code reads
//     as a full word): for CHAR only, a length that includes a 1-byte
//     encoding tag.
//   connode+48 (specifics[56]): for CHAR only, the tag byte ('C' for a
//     plain character constant) followed by (length-1) raw, unescaped
//     string bytes.
std::optional<std::string> parse_scalar_constant(const Entity &e);

bool is_supported_major_id(int major_id);

// True if the file has the 8-byte magic ("k820309\0") at byte offset 12.
// Cheap sniff, no further validation.
bool looks_like_ifx_module(const std::string &path);

// Reads and validates the header and entity directory. Throws
// fxmod::Error for anything structurally wrong (too short, bad magic,
// directory doesn't exactly span the rest of the file) and
// UnsupportedError for an unsupported major_id.
Module read_module(const std::string &path);

} // namespace fxmod::ifx
