#include "fortran_emitter.hpp"

#include <algorithm>
#include <map>
#include <set>
#include <string_view>

#include <fxmod/error.hpp>

namespace fxmod::ifx {

namespace {

// A cross-module (USE-associated) entity's own directory name carries a
// `+SOURCE_MODULE` suffix -- confirmed on real omp_lib.mod (e.g.
// "OMP_ALLOCTRAIT+OMP_LIB_KINDS"). Unlike a name resolved purely for its
// own (always-intrinsic) scalar type -- a field or dummy argument, which
// carries reliable data regardless of which module owns the symbol, see
// resolve() below -- a *type name* pulled from a foreign entity needs a
// `use` statement rather than a bare local reference, since this
// module's own output never defines that type. split_foreign() below
// does the split; record_use() records the need.
struct ForeignRef {
  std::string module_name;
  std::string local_name;
};
std::optional<ForeignRef> split_foreign(const std::string &name) {
  std::size_t pos = name.find('+');
  if (pos == std::string::npos)
    return std::nullopt;
  return ForeignRef{name.substr(pos + 1), name.substr(0, pos)};
}

bool is_foreign_name(const std::string &name) {
  return name.find('+') != std::string::npos;
}

// True for a component/argument directory entry (e.g. "ADD2%A",
// "POINT%X") -- only ever addressed through its owner's own field/
// argument list, never declared independently at module scope.
bool is_component(const std::string &name) {
  return name.find('%') != std::string::npos;
}

// An Overlay entity's own directory name is "#OWNERNAME-ovl#OWNERSYMID#N"
// (see OverlayInfo's comment in module.hpp) -- the only compound name
// with a leading '#'. Extracts OWNERSYMID, the one piece not otherwise
// recoverable from the entity itself (it carries no sym_id of its own).
std::optional<std::uint32_t> parse_overlay_owner_sym_id(const std::string &name) {
  constexpr std::string_view kMarker = "-ovl#";
  std::size_t pos = name.find(kMarker);
  if (pos == std::string::npos)
    return std::nullopt;
  pos += kMarker.size();
  std::size_t end = name.find('#', pos);
  if (end == std::string::npos || end == pos)
    return std::nullopt;
  std::uint32_t value = 0;
  for (std::size_t i = pos; i < end; ++i) {
    if (name[i] < '0' || name[i] > '9')
      return std::nullopt;
    value = value * 10 + static_cast<std::uint32_t>(name[i] - '0');
  }
  return value;
}

// Modules this output needs a `use MODULE, only: name1, name2, ...` line
// for, keyed by module name.
using NeededUses = std::map<std::string, std::set<std::string>>;

void record_use(NeededUses &uses, const ForeignRef &ref) {
  uses[ref.module_name].insert(ref.local_name);
}

struct Analysis {
  std::vector<std::string> body;
  std::vector<std::string> problems;
  NeededUses uses;
};

void add_problem(Analysis &a, const std::string &kind, const std::string &name,
                  const std::string &reason) {
  a.problems.push_back(kind + " '" + name + "': " + reason);
}

// Resolves a NamedRef (a component or dummy-argument reference from a
// derived type's field list or a procedure's argument list) to its own
// directory entry. Returns nullptr -- without adding a problem itself,
// the caller decides the message -- if the reference has no sym_id or
// the sym_id isn't in the directory. Deliberately does NOT refuse a
// foreign (cross-module) entity here: a field's or dummy argument's own
// type is always a plain intrinsic ScalarType (SimpleScalarField/
// SimpleScalarArg), self-contained regardless of which module the
// symbol itself belongs to -- confirmed reliable on real omp_lib.mod,
// where a foreign field (e.g. "OMP_ALLOCTRAIT%KEY+OMP_LIB_KINDS") is
// fully populated, not a stub. Only a *type name* (see
// emit_struct_variable()) needs the foreign/local distinction, since
// that's the one place a `use` statement is or isn't required.
const Entity *resolve(const NamedRef &ref,
                       const std::map<std::uint32_t, const Entity *> &by_sym_id) {
  if (!ref.sym_id.has_value())
    return nullptr;
  auto it = by_sym_id.find(*ref.sym_id);
  return it == by_sym_id.end() ? nullptr : it->second;
}

void emit_scalar(Analysis &a, const std::string &name, const Entity &e) {
  bool is_param = e.skind == SymbolKind::Param;
  std::string type_str;
  if (e.decl_rec_kind == DeclRecKind::Char) {
    if (is_param) {
      // `character(len=*)` on a named constant infers the length from
      // the initializer -- sidesteps needing CHARACTER's own declared
      // length separately.
      type_str = "character(len=*)";
    } else {
      std::optional<std::uint32_t> len = parse_char_length(e);
      if (!len.has_value()) {
        add_problem(a, "variable", name, "CHARACTER length not decoded");
        return;
      }
      type_str = "character(len=" + std::to_string(*len) + ")";
    }
  } else {
    std::optional<ScalarType> t = parse_scalar_type(e);
    std::optional<std::string> ts = t ? to_fortran_type(*t) : std::nullopt;
    if (!ts.has_value()) {
      add_problem(a, "variable", name, "type not decoded");
      return;
    }
    type_str = *ts;
  }

  if (!is_param) {
    a.body.push_back(type_str + " :: " + name);
    return;
  }
  std::optional<std::string> value = parse_scalar_constant(e);
  if (!value.has_value()) {
    add_problem(a, "named constant", name, "value not decoded");
    return;
  }
  a.body.push_back(type_str + ", parameter :: " + name + " = " + *value);
}

void emit_array(Analysis &a, const std::string &name, const Entity &e) {
  std::optional<SimpleArrayInfo> info = parse_simple_array(e);
  if (!info.has_value()) {
    add_problem(a, "array", name, "header not decoded");
    return;
  }
  std::optional<std::string> ts = to_fortran_type(info->type);
  if (!ts.has_value()) {
    add_problem(a, "array", name, "element type not decoded");
    return;
  }
  if (info->is_parameter) {
    std::optional<std::vector<ArrayBound>> bounds = parse_array_constant_bounds(e);
    std::optional<std::vector<std::string>> values = parse_array_constant(e);
    if (!bounds.has_value() || !values.has_value()) {
      add_problem(a, "array", name, "PARAMETER array constant value is not decoded");
      return;
    }
    // to_fortran_type(Char) is bare "character" -- a PARAMETER array of
    // CHARACTER (confirmed to use this same decl_rec_kind, not CharArray)
    // needs its own fixed length spelled out, same as emit_char_array()
    // does for a non-PARAMETER CHARACTER array.
    std::string elem_type_str =
        info->type == ScalarType::Char
            ? "character(len=" + std::to_string(info->element_bytes) + ")"
            : *ts;
    std::string dims;
    for (std::size_t i = 0; i < bounds->size(); ++i) {
      if (i)
        dims += ", ";
      dims += std::to_string((*bounds)[i].lower) + ":" +
              std::to_string((*bounds)[i].upper);
    }
    std::string init;
    for (std::size_t i = 0; i < values->size(); ++i) {
      if (i)
        init += ", ";
      init += (*values)[i];
    }
    // A `[...]` array constructor is always rank 1 -- a rank > 1
    // PARAMETER needs RESHAPE to fan a flat element list (the CONTOK
    // list's own declaration order, confirmed column-major -- matching
    // Fortran's own convention -- against a real rank-2 sample) back out
    // to its declared shape.
    std::string init_expr = "[" + init + "]";
    if (bounds->size() > 1) {
      std::string shape;
      for (std::size_t i = 0; i < bounds->size(); ++i) {
        if (i)
          shape += ", ";
        shape += std::to_string((*bounds)[i].upper - (*bounds)[i].lower + 1);
      }
      init_expr = "reshape(" + init_expr + ", [" + shape + "])";
    }
    a.body.push_back(elem_type_str + ", parameter, dimension(" + dims + ") :: " +
                      name + " = " + init_expr);
    return;
  }
  std::optional<std::vector<ArrayBound>> bounds = parse_array_bounds(e);
  if (!bounds.has_value()) {
    add_problem(a, "array", name, "bounds not decoded");
    return;
  }
  std::string dims;
  for (std::size_t i = 0; i < bounds->size(); ++i) {
    if (i)
      dims += ", ";
    dims += std::to_string((*bounds)[i].lower) + ":" +
            std::to_string((*bounds)[i].upper);
  }
  a.body.push_back(*ts + ", dimension(" + dims + ") :: " + name);
}

void emit_struct_array(Analysis &a, const std::string &name, const Entity &e,
                        const std::map<std::uint32_t, const Entity *> &by_sym_id) {
  std::optional<StructArrayInfo> info = parse_struct_array(e);
  if (!info.has_value() || !info->type_name.has_value()) {
    add_problem(a, "array", name, "header or derived type reference not decoded");
    return;
  }
  const Entity *type_entity = resolve(*info->type_name, by_sym_id);
  if (!type_entity) {
    add_problem(a, "array", name,
                "derived type '" + info->type_name->name + "' is unresolvable");
    return;
  }
  std::optional<std::vector<ArrayBound>> bounds = parse_struct_array_bounds(e);
  if (!bounds.has_value()) {
    add_problem(a, "array", name, "bounds not decoded");
    return;
  }
  std::string type_name = type_entity->name;
  if (std::optional<ForeignRef> fr = split_foreign(type_name)) {
    record_use(a.uses, *fr);
    type_name = fr->local_name;
  }
  std::string dims;
  for (std::size_t i = 0; i < bounds->size(); ++i) {
    if (i)
      dims += ", ";
    dims += std::to_string((*bounds)[i].lower) + ":" +
            std::to_string((*bounds)[i].upper);
  }
  a.body.push_back("type(" + type_name + "), dimension(" + dims + ") :: " + name);
}

void emit_char_array(Analysis &a, const std::string &name, const Entity &e) {
  std::optional<CharArrayInfo> info = parse_char_array(e);
  if (!info.has_value()) {
    add_problem(a, "array", name, "header not decoded");
    return;
  }
  if (info->is_parameter) {
    add_problem(a, "array", name, "PARAMETER array constant value is not decoded");
    return;
  }
  std::optional<std::vector<ArrayBound>> bounds = parse_char_array_bounds(e);
  if (!bounds.has_value()) {
    add_problem(a, "array", name, "bounds not decoded");
    return;
  }
  std::string dims;
  for (std::size_t i = 0; i < bounds->size(); ++i) {
    if (i)
      dims += ", ";
    dims += std::to_string((*bounds)[i].lower) + ":" +
            std::to_string((*bounds)[i].upper);
  }
  a.body.push_back("character(len=" + std::to_string(info->length) +
                    "), dimension(" + dims + ") :: " + name);
}

void emit_derived_type(
    Analysis &a, const std::string &name, const Entity &e,
    const std::map<std::uint32_t, const Entity *> &by_sym_id, bool needs_sequence,
    const std::map<std::uint32_t, const Entity *> &generic_gennode_by_field_sym_id) {
  std::optional<DerivedTypeFields> fields = parse_derived_type(e);
  if (!fields.has_value()) {
    add_problem(a, "type", name,
                "field list not decoded (a field has an inline default-"
                "value expression)");
    return;
  }
  std::vector<std::string> lines;
  std::vector<std::string> bindings;
  for (const NamedRef &f : *fields) {
    const Entity *field_entity = resolve(f, by_sym_id);
    if (!field_entity) {
      add_problem(a, "type", name,
                  "component '" + f.name + "' is unresolvable");
      return;
    }
    if (field_entity->decl_rec_kind == DeclRecKind::TypeBoundProcField) {
      std::optional<TypeBoundProcFieldInfo> tbi =
          parse_type_bound_proc_field(*field_entity);
      if (!tbi.has_value() || !tbi->interface_symbol.has_value()) {
        add_problem(a, "type", name,
                    "type-bound procedure '" + f.name + "' is not decoded");
        return;
      }
      const Entity *impl_entity = resolve(*tbi->interface_symbol, by_sym_id);
      if (!impl_entity) {
        add_problem(a, "type", name,
                    "type-bound procedure '" + f.name + "' implementation '" +
                        tbi->interface_symbol->name + "' is unresolvable");
        return;
      }
      std::string binding = "  procedure";
      if (!tbi->passed_arg)
        binding += ", nopass";
      binding += " :: " + f.name;
      if (impl_entity->name != f.name)
        binding += " => " + impl_entity->name;
      bindings.push_back(binding);
      continue;
    }
    if (field_entity->decl_rec_kind == DeclRecKind::TypeBoundGenericField) {
      auto git = generic_gennode_by_field_sym_id.find(field_entity->sym_id);
      std::optional<TbpGennodeInfo> gen =
          git != generic_gennode_by_field_sym_id.end()
              ? parse_tbp_gennode(*git->second)
              : std::nullopt;
      if (!gen.has_value() ||
          gen->generic_operator != kGenericOperatorPlainName) {
        add_problem(a, "type", name,
                    "GENERIC type-bound binding '" + f.name + "' is not "
                    "decoded");
        return;
      }
      std::string binding = "  generic :: " + f.name + " => ";
      for (std::size_t i = 0; i < gen->gen_procs.size(); ++i) {
        if (i)
          binding += ", ";
        binding += gen->gen_procs[i].name;
      }
      bindings.push_back(binding);
      continue;
    }
    std::string field_type_str;
    std::string field_init_str;
    if (field_entity->decl_rec_kind == DeclRecKind::CharField) {
      std::optional<CharFieldInfo> cfi = parse_char_field(*field_entity);
      if (!cfi.has_value() || cfi->has_undecoded_initializer) {
        add_problem(a, "type", name,
                    "component '" + f.name + "' has an undecoded CHARACTER "
                    "length or default value");
        return;
      }
      field_type_str = "character(len=" + std::to_string(cfi->length) + ")";
      if (cfi->default_value.has_value())
        field_init_str = " = " + *cfi->default_value;
    } else if (field_entity->decl_rec_kind == DeclRecKind::CharArrayField) {
      std::optional<CharArrayFieldInfo> cafi =
          parse_char_array_field(*field_entity);
      std::optional<std::vector<ArrayBound>> bounds =
          parse_char_array_field_bounds(*field_entity);
      if (!cafi.has_value() || !bounds.has_value()) {
        add_problem(a, "type", name,
                    "component '" + f.name + "' has an undecoded CHARACTER "
                    "length or array bounds");
        return;
      }
      std::string dims;
      for (std::size_t i = 0; i < bounds->size(); ++i) {
        if (i)
          dims += ", ";
        dims += std::to_string((*bounds)[i].lower) + ":" +
                std::to_string((*bounds)[i].upper);
      }
      field_type_str = "character(len=" + std::to_string(cafi->length) +
                       "), dimension(" + dims + ")";
    } else if (field_entity->decl_rec_kind == DeclRecKind::StructField) {
      std::optional<StructFieldInfo> sfi = parse_struct_field(*field_entity);
      if (!sfi.has_value() || !sfi->type_name.has_value()) {
        add_problem(a, "type", name,
                    "component '" + f.name + "' has an undecoded derived "
                    "type reference");
        return;
      }
      const Entity *nested_type = resolve(*sfi->type_name, by_sym_id);
      if (!nested_type) {
        add_problem(a, "type", name,
                    "component '" + f.name + "' derived type '" +
                        sfi->type_name->name + "' is unresolvable");
        return;
      }
      std::string type_name = nested_type->name;
      if (std::optional<ForeignRef> fr = split_foreign(type_name)) {
        record_use(a.uses, *fr);
        type_name = fr->local_name;
      }
      field_type_str = "type(" + type_name + ")";
    } else if (field_entity->decl_rec_kind == DeclRecKind::SimpleScalarArrayField) {
      std::optional<SimpleScalarArrayFieldInfo> afi =
          parse_simple_scalar_array_field(*field_entity);
      std::optional<std::vector<ArrayBound>> bounds =
          parse_simple_scalar_array_field_bounds(*field_entity);
      std::optional<std::string> ts =
          afi ? to_fortran_type(afi->type) : std::nullopt;
      if (!ts.has_value() || !bounds.has_value()) {
        add_problem(a, "type", name,
                    "component '" + f.name + "' has an undecoded array type "
                    "or bounds");
        return;
      }
      std::string dims;
      for (std::size_t i = 0; i < bounds->size(); ++i) {
        if (i)
          dims += ", ";
        dims += std::to_string((*bounds)[i].lower) + ":" +
                std::to_string((*bounds)[i].upper);
      }
      field_type_str = *ts + ", dimension(" + dims + ")";
    } else if (field_entity->decl_rec_kind == DeclRecKind::StructArrayField) {
      std::optional<StructArrayFieldInfo> safi =
          parse_struct_array_field(*field_entity);
      if (!safi.has_value() || !safi->type_name.has_value()) {
        add_problem(a, "type", name,
                    "component '" + f.name + "' has an undecoded derived "
                    "type reference or array bounds");
        return;
      }
      const Entity *nested_type = resolve(*safi->type_name, by_sym_id);
      if (!nested_type || is_component(nested_type->name)) {
        add_problem(a, "type", name,
                    "component '" + f.name + "' derived type '" +
                        safi->type_name->name + "' is unresolvable");
        return;
      }
      std::optional<std::vector<ArrayBound>> bounds =
          parse_struct_array_field_bounds(*field_entity);
      if (!bounds.has_value()) {
        add_problem(a, "type", name,
                    "component '" + f.name + "' array bounds not decoded");
        return;
      }
      std::string type_name = nested_type->name;
      if (std::optional<ForeignRef> fr = split_foreign(type_name)) {
        record_use(a.uses, *fr);
        type_name = fr->local_name;
      }
      std::string dims;
      for (std::size_t i = 0; i < bounds->size(); ++i) {
        if (i)
          dims += ", ";
        dims += std::to_string((*bounds)[i].lower) + ":" +
                std::to_string((*bounds)[i].upper);
      }
      field_type_str =
          "type(" + type_name + "), dimension(" + dims + ")";
    } else {
      std::optional<SimpleScalarFieldInfo> fi =
          parse_simple_scalar_field(*field_entity);
      if (!fi.has_value() || fi->has_undecoded_initializer) {
        add_problem(a, "type", name,
                    "component '" + f.name + "' has an undecoded type or "
                    "default value");
        return;
      }
      std::optional<std::string> ts = to_fortran_type(fi->type);
      if (!ts.has_value()) {
        add_problem(a, "type", name,
                    "component '" + f.name + "' has an unrenderable type");
        return;
      }
      field_type_str = *ts;
      if (fi->default_value.has_value())
        field_init_str = " = " + *fi->default_value;
    }
    lines.push_back("  " + field_type_str + " :: " + f.name + field_init_str);
  }
  a.body.push_back("type :: " + name);
  // gfortran refuses a derived-type variable in COMMON unless the type
  // itself has SEQUENCE (or BIND(C)) -- real ifx allows it either way,
  // but portability across compilers is the whole point of this project.
  if (needs_sequence)
    a.body.push_back("  sequence");
  a.body.insert(a.body.end(), lines.begin(), lines.end());
  if (!bindings.empty()) {
    a.body.push_back("contains");
    a.body.insert(a.body.end(), bindings.begin(), bindings.end());
  }
  a.body.push_back("end type " + name);
}

void emit_struct_variable(Analysis &a, const std::string &name, const Entity &e,
                           const std::map<std::uint32_t, const Entity *> &by_sym_id) {
  std::optional<StructVariableInfo> sv = parse_struct_variable(e);
  if (!sv.has_value() || !sv->type_name.has_value()) {
    add_problem(a, "variable", name,
                "derived-type PARAMETER value or type reference not decoded");
    return;
  }
  const Entity *type_entity = resolve(*sv->type_name, by_sym_id);
  if (!type_entity) {
    add_problem(a, "variable", name,
                "derived type '" + sv->type_name->name + "' is unresolvable");
    return;
  }
  std::string type_name = type_entity->name;
  if (std::optional<ForeignRef> fr = split_foreign(type_name)) {
    record_use(a.uses, *fr);
    type_name = fr->local_name;
  }
  a.body.push_back("type(" + type_name + ") :: " + name);
}

void emit_extern_func(Analysis &a, const std::string &name, const Entity &e) {
  std::optional<ExternFuncInfo> info = parse_extern_func(e);
  std::optional<std::string> ts = info ? to_fortran_type(info->type) : std::nullopt;
  if (!ts.has_value()) {
    add_problem(a, "external function", name, "return type not decoded");
    return;
  }
  a.body.push_back(*ts + ", external :: " + name);
}

void emit_extern_derived_type_func(
    Analysis &a, const std::string &name, const Entity &e,
    const std::map<std::uint32_t, const Entity *> &by_sym_id) {
  std::optional<ExternDerivedTypeFuncInfo> info = parse_extern_derived_type_func(e);
  if (!info.has_value() || !info->type_name.has_value()) {
    add_problem(a, "external function", name, "return type not decoded");
    return;
  }
  const Entity *type_entity = resolve(*info->type_name, by_sym_id);
  if (!type_entity) {
    add_problem(a, "external function", name,
                "derived type '" + info->type_name->name + "' is unresolvable");
    return;
  }
  std::string type_name = type_entity->name;
  if (std::optional<ForeignRef> fr = split_foreign(type_name)) {
    record_use(a.uses, *fr);
    type_name = fr->local_name;
  }
  a.body.push_back("type(" + type_name + "), external :: " + name);
}

std::optional<std::vector<std::string>>
build_procedure_body(Analysis &a, const std::string &owner_name,
                      const std::string &name, const Entity &e,
                      const std::map<std::uint32_t, const Entity *> &by_sym_id,
                      const std::string &indent,
                      const std::set<std::uint32_t> &class_pass_procs);

// Renders each of `arguments`'s own declaration line(s) into `arg_lines`
// (indented `indent + "    "`) and its bare name into `arg_names`, both
// in declaration order -- shared by build_procedure_body()
// (Subr/FuncRetSimpleScalar) and every emit_func_ret_*() function
// (FuncRetChar/FuncRetStruct/FuncRetSimpleScalarArray/FuncRetStructArray,
// confirmed 2026-09-17 to carry this exact same "arguments" list shape).
// Recurses into build_procedure_body() for a dummy PROCEDURE argument:
// confirmed on a real callback-argument sample that this needs no
// separate decl_rec_kind at all -- it's simply another Subr/
// FuncRetSimpleScalar entity (class DUMMY, skind ESUBR/EFUNC instead of
// the module-level MSUBR/MFUNC), carrying its own "arguments" list
// exactly like any other procedure -- `class_pass_procs` is forwarded
// as-is for that recursive call. When `force_class_first_arg` is set
// (the caller already determined, via `class_pass_procs`, that this
// procedure is a type-bound procedure's PASS-bound implementation), the
// FIRST argument -- if it's a StructArg -- is rendered `class(...)`
// rather than `type(...)`, required by the language for any extensible
// type (confirmed 2026-09-17: real ifx refuses `type(...)` there with
// error #8264). Returns false (having already recorded a problem against
// `owner_name`, the outermost procedure's own name, so the message is
// attributed sensibly regardless of nesting depth) if any argument isn't
// decoded.
bool build_argument_decls(Analysis &a, const std::string &owner_name,
                           const std::string &name,
                           const std::vector<NamedRef> &arguments,
                           const std::map<std::uint32_t, const Entity *> &by_sym_id,
                           const std::string &indent,
                           std::vector<std::string> &arg_names,
                           std::vector<std::string> &arg_lines,
                           bool force_class_first_arg,
                           const std::set<std::uint32_t> &class_pass_procs) {
  bool first = true;
  for (const NamedRef &arg : arguments) {
    bool is_first_arg = first;
    first = false;
    const Entity *arg_entity = resolve(arg, by_sym_id);
    if (!arg_entity) {
      add_problem(a, "procedure", owner_name,
                  "'" + name + "' argument '" + arg.name + "' is unresolvable");
      return false;
    }
    arg_names.push_back(arg.name);
    if (arg_entity->decl_rec_kind == DeclRecKind::Subr ||
        arg_entity->decl_rec_kind == DeclRecKind::FuncRetSimpleScalar) {
      std::optional<std::vector<std::string>> nested = build_procedure_body(
          a, owner_name, arg.name, *arg_entity, by_sym_id, indent + "    ",
          class_pass_procs);
      if (!nested.has_value())
        return false;
      arg_lines.push_back(indent + "    interface");
      for (const std::string &l : *nested)
        arg_lines.push_back(l);
      arg_lines.push_back(indent + "    end interface");
      continue;
    }
    std::string arg_type_str;
    if (arg_entity->decl_rec_kind == DeclRecKind::CharArg) {
      std::optional<CharArgInfo> cai = parse_char_arg(*arg_entity);
      if (!cai.has_value()) {
        add_problem(a, "procedure", owner_name,
                    "'" + name + "' argument '" + arg.name + "' has an "
                    "undecoded CHARACTER length");
        return false;
      }
      arg_type_str = cai->is_assumed_length
                         ? "character(*)"
                         : "character(len=" + std::to_string(cai->length) + ")";
    } else if (arg_entity->decl_rec_kind == DeclRecKind::StructArg) {
      std::optional<StructArgInfo> sai = parse_struct_arg(*arg_entity);
      if (!sai.has_value() || !sai->type_name.has_value()) {
        add_problem(a, "procedure", owner_name,
                    "'" + name + "' argument '" + arg.name + "' has an "
                    "undecoded derived type reference");
        return false;
      }
      const Entity *type_entity = resolve(*sai->type_name, by_sym_id);
      if (!type_entity || is_component(type_entity->name)) {
        add_problem(a, "procedure", owner_name,
                    "'" + name + "' argument '" + arg.name + "' derived "
                    "type '" + sai->type_name->name + "' is unresolvable");
        return false;
      }
      std::string type_name = type_entity->name;
      if (std::optional<ForeignRef> fr = split_foreign(type_name)) {
        record_use(a.uses, *fr);
        type_name = fr->local_name;
      }
      arg_type_str = (force_class_first_arg && is_first_arg ? "class(" : "type(") +
                      type_name + ")";
    } else if (arg_entity->decl_rec_kind == DeclRecKind::SimpleScalarArrayArg) {
      std::optional<SimpleArrayArgInfo> aai = parse_simple_array_arg(*arg_entity);
      std::optional<std::vector<ArrayBound>> bounds =
          parse_simple_array_arg_bounds(*arg_entity);
      std::optional<std::string> ts =
          aai ? to_fortran_type(aai->type) : std::nullopt;
      if (!ts.has_value() || !bounds.has_value()) {
        add_problem(a, "procedure", owner_name,
                    "'" + name + "' argument '" + arg.name + "' has an "
                    "undecoded array type or bounds");
        return false;
      }
      std::string dims;
      for (std::size_t i = 0; i < bounds->size(); ++i) {
        if (i)
          dims += ", ";
        dims += std::to_string((*bounds)[i].lower) + ":" +
                std::to_string((*bounds)[i].upper);
      }
      arg_type_str = *ts + ", dimension(" + dims + ")";
    } else if (arg_entity->decl_rec_kind == DeclRecKind::StructArrayArg) {
      std::optional<StructArrayArgInfo> saai = parse_struct_array_arg(*arg_entity);
      if (!saai.has_value() || !saai->type_name.has_value()) {
        add_problem(a, "procedure", owner_name,
                    "'" + name + "' argument '" + arg.name + "' has an "
                    "undecoded derived type reference or array bounds");
        return false;
      }
      const Entity *type_entity = resolve(*saai->type_name, by_sym_id);
      if (!type_entity || is_component(type_entity->name)) {
        add_problem(a, "procedure", owner_name,
                    "'" + name + "' argument '" + arg.name + "' derived "
                    "type '" + saai->type_name->name + "' is unresolvable");
        return false;
      }
      std::optional<std::vector<ArrayBound>> bounds =
          parse_struct_array_arg_bounds(*arg_entity);
      if (!bounds.has_value()) {
        add_problem(a, "procedure", owner_name,
                    "'" + name + "' argument '" + arg.name + "' array "
                    "bounds not decoded");
        return false;
      }
      std::string type_name = type_entity->name;
      if (std::optional<ForeignRef> fr = split_foreign(type_name)) {
        record_use(a.uses, *fr);
        type_name = fr->local_name;
      }
      std::string dims;
      for (std::size_t i = 0; i < bounds->size(); ++i) {
        if (i)
          dims += ", ";
        dims += std::to_string((*bounds)[i].lower) + ":" +
                std::to_string((*bounds)[i].upper);
      }
      arg_type_str = "type(" + type_name + "), dimension(" + dims + ")";
    } else if (arg_entity->decl_rec_kind == DeclRecKind::CharArrayArg) {
      std::optional<CharArrayArgInfo> caai = parse_char_array_arg(*arg_entity);
      std::optional<std::vector<ArrayBound>> bounds =
          parse_char_array_arg_bounds(*arg_entity);
      if (!caai.has_value() || !bounds.has_value()) {
        add_problem(a, "procedure", owner_name,
                    "'" + name + "' argument '" + arg.name + "' has an "
                    "undecoded CHARACTER length or array bounds");
        return false;
      }
      std::string dims;
      for (std::size_t i = 0; i < bounds->size(); ++i) {
        if (i)
          dims += ", ";
        dims += std::to_string((*bounds)[i].lower) + ":" +
                std::to_string((*bounds)[i].upper);
      }
      arg_type_str = "character(len=" + std::to_string(caai->length) +
                     "), dimension(" + dims + ")";
    } else {
      std::optional<DummyArgInfo> ai = parse_dummy_arg(*arg_entity);
      std::optional<std::string> ts =
          ai ? to_fortran_type(ai->type) : std::nullopt;
      if (!ts.has_value()) {
        add_problem(a, "procedure", owner_name,
                    "'" + name + "' argument '" + arg.name + "' has an "
                    "undecoded type (e.g. an array)");
        return false;
      }
      arg_type_str = *ts;
    }
    arg_lines.push_back(indent + "    " + arg_type_str + " :: " + arg.name);
  }
  return true;
}

// Builds "function/subroutine NAME(args) ... end function/subroutine
// NAME" body lines (no outer interface/end interface wrapper -- that
// differs by nesting depth, added by the caller) for a Subr/
// FuncRetSimpleScalar procedure entity. Every line is indented by
// `indent` spaces. Returns nullopt (having already recorded a problem)
// if anything below isn't decoded.
std::optional<std::vector<std::string>>
build_procedure_body(Analysis &a, const std::string &owner_name,
                      const std::string &name, const Entity &e,
                      const std::map<std::uint32_t, const Entity *> &by_sym_id,
                      const std::string &indent,
                      const std::set<std::uint32_t> &class_pass_procs) {
  std::optional<ProcedureInfo> proc = parse_procedure(e);
  if (!proc.has_value()) {
    add_problem(a, "procedure", owner_name,
                "'" + name + "': signature not decoded");
    return std::nullopt;
  }
  bool is_func = proc->return_type.has_value();
  std::vector<std::string> arg_names;
  std::vector<std::string> arg_lines;
  bool force_class = class_pass_procs.count(e.sym_id) != 0;
  if (!build_argument_decls(a, owner_name, name, proc->arguments, by_sym_id,
                             indent, arg_names, arg_lines, force_class,
                             class_pass_procs))
    return std::nullopt;
  std::string return_type_str;
  if (is_func) {
    std::optional<std::string> ts = to_fortran_type(*proc->return_type);
    if (!ts.has_value()) {
      add_problem(a, "procedure", owner_name,
                  "'" + name + "' return type not decoded");
      return std::nullopt;
    }
    return_type_str = *ts;
  }

  const char *kind = is_func ? "function" : "subroutine";
  std::string names;
  for (std::size_t i = 0; i < arg_names.size(); ++i) {
    if (i)
      names += ", ";
    names += arg_names[i];
  }
  std::vector<std::string> body;
  body.push_back(indent + kind + " " + name + "(" + names + ")");
  // No host association inside an interface body -- see the identical
  // note in src/gfortran/fortran_emitter.cpp's emit_interface_body().
  body.push_back(indent + "  import");
  if (is_func)
    body.push_back(indent + "  " + return_type_str + " :: " + name);
  for (const std::string &l : arg_lines)
    body.push_back(l);
  body.push_back(indent + "end " + kind + " " + name);
  return body;
}

void emit_procedure(Analysis &a, const std::string &name, const Entity &e,
                     const std::map<std::uint32_t, const Entity *> &by_sym_id,
                     const std::set<std::uint32_t> &class_pass_procs) {
  std::optional<std::vector<std::string>> body =
      build_procedure_body(a, name, name, e, by_sym_id, "  ", class_pass_procs);
  if (!body.has_value())
    return;
  a.body.push_back("interface");
  for (const std::string &l : *body)
    a.body.push_back(l);
  a.body.push_back("end interface");
}

void emit_func_ret_char(Analysis &a, const std::string &name, const Entity &e,
                         const std::map<std::uint32_t, const Entity *> &by_sym_id,
                         const std::set<std::uint32_t> &class_pass_procs) {
  std::optional<FuncRetCharInfo> info = parse_func_ret_char(e);
  if (!info.has_value()) {
    add_problem(a, "procedure", name,
                "CHARACTER-returning function's signature is not decoded");
    return;
  }
  std::vector<std::string> arg_names, arg_lines;
  if (!build_argument_decls(a, name, name, info->arguments, by_sym_id, "  ",
                             arg_names, arg_lines,
                             class_pass_procs.count(e.sym_id) != 0, class_pass_procs))
    return;
  std::string names;
  for (std::size_t i = 0; i < arg_names.size(); ++i) {
    if (i)
      names += ", ";
    names += arg_names[i];
  }
  a.body.push_back("interface");
  a.body.push_back("  function " + name + "(" + names + ")");
  // No host association inside an interface body -- see the identical
  // note in the gfortran emitter's emit_interface_body().
  a.body.push_back("    import");
  a.body.push_back("    character(len=" + std::to_string(info->length) +
                    ") :: " + name);
  for (const std::string &l : arg_lines)
    a.body.push_back(l);
  a.body.push_back("  end function " + name);
  a.body.push_back("end interface");
}

void emit_func_ret_struct(Analysis &a, const std::string &name, const Entity &e,
                           const std::map<std::uint32_t, const Entity *> &by_sym_id,
                           const std::set<std::uint32_t> &class_pass_procs) {
  std::optional<FuncRetStructInfo> info = parse_func_ret_struct(e);
  if (!info.has_value() || !info->type_name.has_value()) {
    add_problem(a, "procedure", name,
                "derived-type-returning function with a function-local "
                "result type is not decoded");
    return;
  }
  const Entity *type_entity = resolve(*info->type_name, by_sym_id);
  if (!type_entity) {
    add_problem(a, "procedure", name,
                "derived type '" + info->type_name->name + "' is unresolvable");
    return;
  }
  std::string type_name = type_entity->name;
  if (std::optional<ForeignRef> fr = split_foreign(type_name)) {
    record_use(a.uses, *fr);
    type_name = fr->local_name;
  }
  std::vector<std::string> arg_names, arg_lines;
  if (!build_argument_decls(a, name, name, info->arguments, by_sym_id, "  ",
                             arg_names, arg_lines,
                             class_pass_procs.count(e.sym_id) != 0, class_pass_procs))
    return;
  std::string names;
  for (std::size_t i = 0; i < arg_names.size(); ++i) {
    if (i)
      names += ", ";
    names += arg_names[i];
  }
  a.body.push_back("interface");
  a.body.push_back("  function " + name + "(" + names + ")");
  // No host association inside an interface body -- see the identical
  // note in the gfortran emitter's emit_interface_body().
  a.body.push_back("    import");
  a.body.push_back("    type(" + type_name + ") :: " + name);
  for (const std::string &l : arg_lines)
    a.body.push_back(l);
  a.body.push_back("  end function " + name);
  a.body.push_back("end interface");
}

void emit_func_ret_simple_scalar_array(
    Analysis &a, const std::string &name, const Entity &e,
    const std::map<std::uint32_t, const Entity *> &by_sym_id,
    const std::set<std::uint32_t> &class_pass_procs) {
  std::optional<FuncRetSimpleScalarArrayInfo> info =
      parse_func_ret_simple_scalar_array(e);
  if (!info.has_value()) {
    add_problem(a, "procedure", name,
                "array-returning function's signature is not decoded");
    return;
  }
  std::optional<std::vector<ArrayBound>> bounds =
      parse_func_ret_simple_scalar_array_bounds(e);
  std::optional<std::string> ts = to_fortran_type(info->type);
  if (!bounds.has_value() || !ts.has_value()) {
    add_problem(a, "procedure", name, "return bounds or element type not decoded");
    return;
  }
  std::string dims;
  for (std::size_t i = 0; i < bounds->size(); ++i) {
    if (i)
      dims += ", ";
    dims += std::to_string((*bounds)[i].lower) + ":" +
            std::to_string((*bounds)[i].upper);
  }
  std::vector<std::string> arg_names, arg_lines;
  if (!build_argument_decls(a, name, name, info->arguments, by_sym_id, "  ",
                             arg_names, arg_lines,
                             class_pass_procs.count(e.sym_id) != 0, class_pass_procs))
    return;
  std::string names;
  for (std::size_t i = 0; i < arg_names.size(); ++i) {
    if (i)
      names += ", ";
    names += arg_names[i];
  }
  a.body.push_back("interface");
  a.body.push_back("  function " + name + "(" + names + ")");
  // No host association inside an interface body -- see the identical
  // note in the gfortran emitter's emit_interface_body().
  a.body.push_back("    import");
  a.body.push_back("    " + *ts + ", dimension(" + dims + ") :: " + name);
  for (const std::string &l : arg_lines)
    a.body.push_back(l);
  a.body.push_back("  end function " + name);
  a.body.push_back("end interface");
}

void emit_func_ret_struct_array(Analysis &a, const std::string &name, const Entity &e,
                                 const std::map<std::uint32_t, const Entity *> &by_sym_id,
                                 const std::set<std::uint32_t> &class_pass_procs) {
  std::optional<FuncRetStructArrayInfo> info = parse_func_ret_struct_array(e);
  if (!info.has_value() || !info->type_name.has_value()) {
    add_problem(a, "procedure", name,
                "derived-type-array-returning function's signature is not "
                "decoded");
    return;
  }
  const Entity *type_entity = resolve(*info->type_name, by_sym_id);
  if (!type_entity) {
    add_problem(a, "procedure", name,
                "derived type '" + info->type_name->name + "' is unresolvable");
    return;
  }
  std::optional<std::vector<ArrayBound>> bounds =
      parse_func_ret_struct_array_bounds(e);
  if (!bounds.has_value()) {
    add_problem(a, "procedure", name, "return bounds not decoded");
    return;
  }
  std::string type_name = type_entity->name;
  if (std::optional<ForeignRef> fr = split_foreign(type_name)) {
    record_use(a.uses, *fr);
    type_name = fr->local_name;
  }
  std::string dims;
  for (std::size_t i = 0; i < bounds->size(); ++i) {
    if (i)
      dims += ", ";
    dims += std::to_string((*bounds)[i].lower) + ":" +
            std::to_string((*bounds)[i].upper);
  }
  std::vector<std::string> arg_names, arg_lines;
  if (!build_argument_decls(a, name, name, info->arguments, by_sym_id, "  ",
                             arg_names, arg_lines,
                             class_pass_procs.count(e.sym_id) != 0, class_pass_procs))
    return;
  std::string names;
  for (std::size_t i = 0; i < arg_names.size(); ++i) {
    if (i)
      names += ", ";
    names += arg_names[i];
  }
  a.body.push_back("interface");
  a.body.push_back("  function " + name + "(" + names + ")");
  // No host association inside an interface body -- see the identical
  // note in the gfortran emitter's emit_interface_body().
  a.body.push_back("    import");
  a.body.push_back("    type(" + type_name + "), dimension(" + dims +
                    ") :: " + name);
  for (const std::string &l : arg_lines)
    a.body.push_back(l);
  a.body.push_back("  end function " + name);
  a.body.push_back("end interface");
}

void emit_common_block(Analysis &a, const std::string &name, const Entity &e,
                        const std::map<std::uint32_t, const Entity *> &by_sym_id) {
  std::optional<CommonBlockVariables> vars = parse_common_block(e);
  if (!vars.has_value()) {
    add_problem(a, "common block", name, "member list not decoded");
    return;
  }
  // The directory name is "MODULE!BLOCKNAME" -- only the block name
  // itself belongs in a `common /.../ ...` statement.
  std::string block_name = name;
  if (std::size_t bang = name.find('!'); bang != std::string::npos)
    block_name = name.substr(bang + 1);

  std::vector<std::string> decls;
  std::vector<std::string> var_names;
  for (const NamedRef &v : *vars) {
    const Entity *var_entity = resolve(v, by_sym_id);
    if (!var_entity) {
      add_problem(a, "common block", name,
                  "member '" + v.name + "' is unresolvable");
      return;
    }
    std::string decl_str;
    if (var_entity->decl_rec_kind == DeclRecKind::CharCommon) {
      std::optional<CharCommonInfo> cci = parse_char_common(*var_entity);
      if (!cci.has_value()) {
        add_problem(a, "common block", name,
                    "member '" + v.name + "' has an undecoded CHARACTER "
                    "length");
        return;
      }
      decl_str = "character(len=" + std::to_string(cci->length) + ")";
    } else if (var_entity->decl_rec_kind == DeclRecKind::StructCommon) {
      std::optional<StructCommonInfo> sci = parse_struct_common(*var_entity);
      if (!sci.has_value() || !sci->type_name.has_value()) {
        add_problem(a, "common block", name,
                    "member '" + v.name + "' has an undecoded derived type "
                    "reference");
        return;
      }
      const Entity *type_entity = resolve(*sci->type_name, by_sym_id);
      if (!type_entity || is_component(type_entity->name)) {
        add_problem(a, "common block", name,
                    "member '" + v.name + "' derived type '" +
                        sci->type_name->name + "' is unresolvable");
        return;
      }
      std::string type_name = type_entity->name;
      if (std::optional<ForeignRef> fr = split_foreign(type_name)) {
        record_use(a.uses, *fr);
        type_name = fr->local_name;
      }
      decl_str = "type(" + type_name + ")";
    } else if (var_entity->decl_rec_kind == DeclRecKind::SimpleScalarArrayCommon) {
      std::optional<SimpleScalarArrayCommonInfo> saci =
          parse_simple_scalar_array_common(*var_entity);
      std::optional<std::vector<ArrayBound>> bounds =
          parse_simple_scalar_array_common_bounds(*var_entity);
      std::optional<std::string> ts =
          saci ? to_fortran_type(saci->type) : std::nullopt;
      if (!ts.has_value() || !bounds.has_value()) {
        add_problem(a, "common block", name,
                    "member '" + v.name + "' has an undecoded array type "
                    "or bounds");
        return;
      }
      std::string dims;
      for (std::size_t i = 0; i < bounds->size(); ++i) {
        if (i)
          dims += ", ";
        dims += std::to_string((*bounds)[i].lower) + ":" +
                std::to_string((*bounds)[i].upper);
      }
      decl_str = *ts + ", dimension(" + dims + ")";
    } else if (var_entity->decl_rec_kind == DeclRecKind::StructArrayCommon) {
      std::optional<StructArrayCommonInfo> saci =
          parse_struct_array_common(*var_entity);
      if (!saci.has_value() || !saci->type_name.has_value()) {
        add_problem(a, "common block", name,
                    "member '" + v.name + "' has an undecoded derived type "
                    "reference or array bounds");
        return;
      }
      const Entity *type_entity = resolve(*saci->type_name, by_sym_id);
      if (!type_entity || is_component(type_entity->name)) {
        add_problem(a, "common block", name,
                    "member '" + v.name + "' derived type '" +
                        saci->type_name->name + "' is unresolvable");
        return;
      }
      std::optional<std::vector<ArrayBound>> bounds =
          parse_struct_array_common_bounds(*var_entity);
      if (!bounds.has_value()) {
        add_problem(a, "common block", name,
                    "member '" + v.name + "' array bounds not decoded");
        return;
      }
      std::string type_name = type_entity->name;
      if (std::optional<ForeignRef> fr = split_foreign(type_name)) {
        record_use(a.uses, *fr);
        type_name = fr->local_name;
      }
      std::string dims;
      for (std::size_t i = 0; i < bounds->size(); ++i) {
        if (i)
          dims += ", ";
        dims += std::to_string((*bounds)[i].lower) + ":" +
                std::to_string((*bounds)[i].upper);
      }
      decl_str = "type(" + type_name + "), dimension(" + dims + ")";
    } else if (var_entity->decl_rec_kind == DeclRecKind::CharArrayCommon) {
      std::optional<CharArrayCommonInfo> caci =
          parse_char_array_common(*var_entity);
      std::optional<std::vector<ArrayBound>> bounds =
          parse_char_array_common_bounds(*var_entity);
      if (!caci.has_value() || !bounds.has_value()) {
        add_problem(a, "common block", name,
                    "member '" + v.name + "' has an undecoded CHARACTER "
                    "length or array bounds");
        return;
      }
      std::string dims;
      for (std::size_t i = 0; i < bounds->size(); ++i) {
        if (i)
          dims += ", ";
        dims += std::to_string((*bounds)[i].lower) + ":" +
                std::to_string((*bounds)[i].upper);
      }
      decl_str = "character(len=" + std::to_string(caci->length) +
                 "), dimension(" + dims + ")";
    } else {
      std::optional<CommonVariableInfo> cvi = parse_common_variable(*var_entity);
      std::optional<std::string> ts =
          cvi ? to_fortran_type(cvi->type) : std::nullopt;
      if (!ts.has_value()) {
        add_problem(a, "common block", name,
                    "member '" + v.name + "' has an undecoded type");
        return;
      }
      decl_str = *ts;
    }
    decls.push_back(decl_str + " :: " + v.name);
    var_names.push_back(v.name);
  }
  for (const std::string &d : decls)
    a.body.push_back(d);
  std::string common_line = "common /" + block_name + "/ ";
  for (std::size_t i = 0; i < var_names.size(); ++i) {
    if (i)
      common_line += ", ";
    common_line += var_names[i];
  }
  a.body.push_back(common_line);
}

void emit_namelist_group(Analysis &a, const std::string &name, const Entity &e,
                          const std::map<std::uint32_t, const Entity *> &by_sym_id) {
  std::optional<NamelistVariables> vars = parse_namelist_group(e);
  if (!vars.has_value()) {
    add_problem(a, "namelist", name, "member list not decoded");
    return;
  }
  std::vector<std::string> var_names;
  for (const NamedRef &v : *vars) {
    if (!resolve(v, by_sym_id)) {
      add_problem(a, "namelist", name, "member '" + v.name + "' is unresolvable");
      return;
    }
    var_names.push_back(v.name);
  }
  // Members are independently declared elsewhere at module scope (they
  // have no component-style compound name of their own) -- only the
  // `namelist` statement itself is emitted here.
  std::string line = "namelist /" + name + "/ ";
  for (std::size_t i = 0; i < var_names.size(); ++i) {
    if (i)
      line += ", ";
    line += var_names[i];
  }
  a.body.push_back(line);
}

// The sym_ids of `e`'s own StructField components that reference another
// *local* (non-foreign) derived type -- used only to order derived-type
// emission so a type composed of another local type is never emitted
// before the type it depends on (Fortran allows no forward reference
// between module-level derived types, unlike procedures' `import`).
std::vector<std::uint32_t>
local_struct_field_deps(const Entity &e,
                         const std::map<std::uint32_t, const Entity *> &by_sym_id) {
  std::vector<std::uint32_t> deps;
  std::optional<DerivedTypeFields> fields = parse_derived_type(e);
  if (!fields.has_value())
    return deps;
  for (const NamedRef &f : *fields) {
    const Entity *field_entity = resolve(f, by_sym_id);
    if (!field_entity || field_entity->decl_rec_kind != DeclRecKind::StructField)
      continue;
    std::optional<StructFieldInfo> sfi = parse_struct_field(*field_entity);
    if (!sfi.has_value() || !sfi->type_name.has_value())
      continue;
    const Entity *nested_type = resolve(*sfi->type_name, by_sym_id);
    if (nested_type && !is_foreign_name(nested_type->name))
      deps.push_back(nested_type->sym_id);
  }
  return deps;
}

// Each decl_rec_kind == Overlay entity lists every OTHER member of its
// own EQUIVALENCE group (a fully-connected clique -- see OverlayInfo's
// comment), so the group is redundantly represented once per member.
// Emits exactly one `equivalence(...)` per group by only acting on the
// member whose own sym_id is the group's minimum, and only for a group
// where every member is a plain intrinsic scalar variable (the only
// shape confirmed -- arrays or derived-type members, or a nonzero
// `offset`, refuse rather than guess).
void emit_equivalences(Analysis &a, const std::vector<const Entity *> &overlays,
                        const std::map<std::uint32_t, const Entity *> &by_sym_id) {
  for (const Entity *ov : overlays) {
    std::optional<std::uint32_t> owner_sym_id =
        parse_overlay_owner_sym_id(ov->name);
    std::optional<OverlayInfo> info = parse_overlay(*ov);
    if (!owner_sym_id.has_value() || !info.has_value() || info->offset != 0) {
      add_problem(a, "equivalence", ov->name, "group not decoded");
      continue;
    }
    std::vector<std::uint32_t> group_ids;
    group_ids.push_back(*owner_sym_id);
    bool ok = true;
    for (const NamedRef &m : info->ovl_syms) {
      if (!m.sym_id.has_value()) {
        ok = false;
        break;
      }
      group_ids.push_back(*m.sym_id);
    }
    if (!ok) {
      add_problem(a, "equivalence", ov->name, "a group member is unresolvable");
      continue;
    }
    if (*owner_sym_id != *std::min_element(group_ids.begin(), group_ids.end()))
      continue; // this group's canonical line is emitted by another member
    std::sort(group_ids.begin(), group_ids.end());
    std::vector<std::string> names;
    bool all_plain_scalar = true;
    for (std::uint32_t id : group_ids) {
      auto it = by_sym_id.find(id);
      if (it == by_sym_id.end() ||
          it->second->decl_rec_kind != DeclRecKind::SimpleScalar) {
        all_plain_scalar = false;
        break;
      }
      names.push_back(it->second->name);
    }
    if (!all_plain_scalar) {
      add_problem(a, "equivalence", ov->name,
                  "a group member is not a plain intrinsic scalar variable");
      continue;
    }
    std::string line = "equivalence(";
    for (std::size_t i = 0; i < names.size(); ++i) {
      if (i)
        line += ", ";
      line += names[i];
    }
    line += ")";
    a.body.push_back(line);
  }
}

Analysis analyse(const Module &module) {
  Analysis a;
  std::map<std::uint32_t, const Entity *> by_sym_id;
  for (const Entity &e : module.entities)
    by_sym_id[e.sym_id] = &e;

  // Derived types referenced (directly, via StructCommon/StructArrayCommon)
  // from inside a COMMON block need SEQUENCE -- see emit_derived_type()'s
  // comment on why. Collected up front so the emission pass below can
  // decide per-type without needing a second lookup pass later.
  std::set<std::uint32_t> types_needing_sequence;
  for (const Entity &e : module.entities) {
    if (e.decl_rec_kind != DeclRecKind::Common)
      continue;
    std::optional<CommonBlockVariables> vars = parse_common_block(e);
    if (!vars.has_value())
      continue;
    for (const NamedRef &v : *vars) {
      const Entity *member = resolve(v, by_sym_id);
      if (!member)
        continue;
      std::optional<NamedRef> type_name;
      if (member->decl_rec_kind == DeclRecKind::StructCommon) {
        if (std::optional<StructCommonInfo> sci = parse_struct_common(*member))
          type_name = sci->type_name;
      } else if (member->decl_rec_kind == DeclRecKind::StructArrayCommon) {
        if (std::optional<StructArrayCommonInfo> saci =
                parse_struct_array_common(*member))
          type_name = saci->type_name;
      }
      if (!type_name.has_value())
        continue;
      if (const Entity *type_entity = resolve(*type_name, by_sym_id))
        types_needing_sequence.insert(type_entity->sym_id);
    }
  }

  // A type-bound procedure's own implementation, when PASS-bound (the
  // default -- NOPASS is the only other confirmed case), must declare
  // its own passed-object (first) dummy argument as CLASS rather than
  // TYPE -- confirmed 2026-09-17: real ifx refuses a `type(...)`
  // passed-object argument for any extensible type with error #8264.
  // Collected up front, keyed by the implementing procedure's own
  // sym_id, so build_argument_decls() can decide per-procedure without
  // needing to search every derived type's binding list itself.
  std::set<std::uint32_t> procedures_needing_class_pass;
  for (const Entity &e : module.entities) {
    if (e.decl_rec_kind != DeclRecKind::TypeBoundProcField)
      continue;
    std::optional<TypeBoundProcFieldInfo> tb = parse_type_bound_proc_field(e);
    if (!tb.has_value() || !tb->passed_arg || !tb->interface_symbol.has_value())
      continue;
    if (const Entity *impl = resolve(*tb->interface_symbol, by_sym_id))
      procedures_needing_class_pass.insert(impl->sym_id);
  }

  // A TypeBoundGenericField's own generic-procedure list lives in a
  // separate TbpGennode entity, confirmed to always be the very next one
  // in the module's own entity list (that gennode's own sym_id is always
  // 0, so it isn't independently resolvable via by_sym_id) -- paired up
  // front here, keyed by the TypeBoundGenericField's own sym_id.
  std::map<std::uint32_t, const Entity *> generic_gennode_by_field_sym_id;
  for (std::size_t i = 0; i + 1 < module.entities.size(); ++i) {
    if (module.entities[i].decl_rec_kind == DeclRecKind::TypeBoundGenericField &&
        module.entities[i + 1].decl_rec_kind == DeclRecKind::TbpGennode)
      generic_gennode_by_field_sym_id[module.entities[i].sym_id] =
          &module.entities[i + 1];
  }

  // Derived types first, in dependency order (a type composed of another
  // local type is emitted after that type) -- everything else may
  // reference them freely afterwards regardless of order (procedures via
  // `import`, variables/fields/arguments by direct lookup).
  {
    std::vector<const Entity *> pending;
    for (const Entity &e : module.entities)
      if (e.decl_rec_kind == DeclRecKind::DerivedType && !is_component(e.name) &&
          !is_foreign_name(e.name))
        pending.push_back(&e);
    std::set<std::uint32_t> emitted_sym_ids;
    while (!pending.empty()) {
      bool progress = false;
      for (auto it = pending.begin(); it != pending.end();) {
        std::vector<std::uint32_t> deps = local_struct_field_deps(**it, by_sym_id);
        bool ready = std::all_of(deps.begin(), deps.end(), [&](std::uint32_t id) {
          return emitted_sym_ids.count(id) != 0;
        });
        if (ready) {
          emit_derived_type(a, (*it)->name, **it, by_sym_id,
                             types_needing_sequence.count((*it)->sym_id) != 0,
                             generic_gennode_by_field_sym_id);
          emitted_sym_ids.insert((*it)->sym_id);
          it = pending.erase(it);
          progress = true;
        } else {
          ++it;
        }
      }
      if (!progress) {
        // A circular dependency among the remaining types (or one this
        // reader can't see the shape of) -- emit them anyway rather than
        // loop forever; each independently succeeds or reports its own
        // problem.
        for (const Entity *te : pending)
          emit_derived_type(a, te->name, *te, by_sym_id,
                             types_needing_sequence.count(te->sym_id) != 0,
                             generic_gennode_by_field_sym_id);
        break;
      }
    }
  }

  // NAMELIST groups are deferred to after the main loop, for the same
  // reason derived types are moved first above: a group's own directory
  // entry can precede its member variables' own entities (confirmed on
  // a real sample), and `namelist /.../` must follow their declarations
  // -- gfortran enforces this even though ifx itself doesn't seem to.
  std::vector<const Entity *> pending_namelists;
  // EQUIVALENCE groups (decl_rec_kind == Overlay) are collected the same
  // way and processed after the main loop, once every plain variable has
  // been emitted -- see emit_equivalences() below for why one member's
  // worth of these must be discarded per group.
  std::vector<const Entity *> pending_overlays;

  for (const Entity &e : module.entities) {
    if (e.decl_rec_kind == DeclRecKind::FileName)
      continue; // the synthetic "fn#fn" header entity, not a declaration
    if (e.decl_rec_kind == DeclRecKind::NamelistGroup) {
      pending_namelists.push_back(&e);
      continue;
    }
    if (e.decl_rec_kind == DeclRecKind::DerivedType)
      continue; // already emitted above, in dependency order
    if (is_component(e.name))
      continue; // addressed through its owner's field/argument list
    if (std::optional<ForeignRef> fr = split_foreign(e.name)) {
      // A USE-associated entity re-exported under this module's own
      // public name -- satisfied by `use <module>, only: <name>` rather
      // than redeclared (this module's output never defines it, and
      // Fortran's default accessibility already makes a used name public
      // here, since PUBLIC/PRIVATE statements aren't decoded -- see the
      // PubPriv skip just below).
      record_use(a.uses, *fr);
      continue;
    }
    if (e.decl_rec_kind == DeclRecKind::PubPriv ||
        e.decl_rec_kind == DeclRecKind::UseStmt)
      continue; // bookkeeping, not a symbol
    if (e.decl_rec_kind == DeclRecKind::IntrinsicProc)
      continue; // a reference to a compiler-builtin (e.g. RESHAPE) used
                // somewhere in this module -- nothing to declare for it
    if (e.decl_rec_kind == DeclRecKind::TbpGennode)
      continue; // addressed only through its preceding TypeBoundGenericField
                // (positional pairing, not by sym_id -- see its comment)
    if (e.decl_rec_kind == DeclRecKind::SimpleScalarCommon ||
        e.decl_rec_kind == DeclRecKind::CharCommon ||
        e.decl_rec_kind == DeclRecKind::StructCommon ||
        e.decl_rec_kind == DeclRecKind::SimpleScalarArrayCommon ||
        e.decl_rec_kind == DeclRecKind::StructArrayCommon ||
        e.decl_rec_kind == DeclRecKind::CharArrayCommon)
      continue; // addressed through its owning Common's own member list
    if (e.decl_rec_kind == DeclRecKind::Overlay) {
      pending_overlays.push_back(&e);
      continue;
    }

    switch (e.decl_rec_kind) {
    case DeclRecKind::SimpleScalar:
    case DeclRecKind::Char:
      emit_scalar(a, e.name, e);
      break;
    case DeclRecKind::SimpleScalarArray:
      emit_array(a, e.name, e);
      break;
    case DeclRecKind::StructArray:
      emit_struct_array(a, e.name, e, by_sym_id);
      break;
    case DeclRecKind::CharArray:
      emit_char_array(a, e.name, e);
      break;
    case DeclRecKind::Struct:
      emit_struct_variable(a, e.name, e, by_sym_id);
      break;
    case DeclRecKind::ExternFunc:
      emit_extern_func(a, e.name, e);
      break;
    case DeclRecKind::ExternDerivedTypeFunc:
      emit_extern_derived_type_func(a, e.name, e, by_sym_id);
      break;
    case DeclRecKind::Subr:
    case DeclRecKind::FuncRetSimpleScalar:
      emit_procedure(a, e.name, e, by_sym_id, procedures_needing_class_pass);
      break;
    case DeclRecKind::FuncRetChar:
      emit_func_ret_char(a, e.name, e, by_sym_id, procedures_needing_class_pass);
      break;
    case DeclRecKind::FuncRetStruct:
      emit_func_ret_struct(a, e.name, e, by_sym_id, procedures_needing_class_pass);
      break;
    case DeclRecKind::FuncRetSimpleScalarArray:
      emit_func_ret_simple_scalar_array(a, e.name, e, by_sym_id,
                                         procedures_needing_class_pass);
      break;
    case DeclRecKind::FuncRetStructArray:
      emit_func_ret_struct_array(a, e.name, e, by_sym_id,
                                  procedures_needing_class_pass);
      break;
    case DeclRecKind::Common:
      emit_common_block(a, e.name, e, by_sym_id);
      break;
    default:
      add_problem(a, "entity", e.name, "outside the translated subset");
      break;
    }
  }

  for (const Entity *ng : pending_namelists)
    emit_namelist_group(a, ng->name, *ng, by_sym_id);

  emit_equivalences(a, pending_overlays, by_sym_id);

  return a;
}

std::string summarise_problems(const std::vector<std::string> &problems,
                                std::size_t limit = 6) {
  std::string text;
  std::size_t shown = std::min(limit, problems.size());
  for (std::size_t i = 0; i < shown; ++i) {
    if (i)
      text += "; ";
    text += problems[i];
  }
  if (problems.size() > shown)
    text += "; and " + std::to_string(problems.size() - shown) + " more";
  return text;
}

} // namespace

EmitResult emit_fortran_source(const Module &module, bool strict) {
  Analysis a = analyse(module);
  if (!a.problems.empty() && strict) {
    throw UnsupportedError(module.path + ": " +
                            std::to_string(a.problems.size()) +
                            " untranslatable entity/entities: " +
                            summarise_problems(a.problems));
  }

  std::string name = module.name();
  std::string source = "module " + name + "\n";
  for (const auto &[module_name, names] : a.uses) {
    source += "use " + module_name + ", only: ";
    bool first = true;
    for (const std::string &n : names) {
      if (!first)
        source += ", ";
      source += n;
      first = false;
    }
    source += "\n";
  }
  source += "implicit none\n";
  for (const std::string &line : a.body)
    source += line + "\n";
  source += "end module " + name + "\n";

  return EmitResult{std::move(source), std::move(a.problems)};
}

} // namespace fxmod::ifx
