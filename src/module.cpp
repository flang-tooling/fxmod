#include <fxmod/module.hpp>

#include <variant>

#include "detect.hpp"
#include "flang/module.hpp"
#include "gfortran/fortran_emitter.hpp"
#include "gfortran/module.hpp"
#include "gfortran/symbols.hpp"
#include "ifx/fortran_emitter.hpp"
#include "ifx/module.hpp"

namespace fxmod {

struct ModuleFile::Impl {
  std::variant<gfortran::Module, flang::Module, ifx::Module> data;
};

ModuleFile::ModuleFile() = default;
ModuleFile::~ModuleFile() = default;
ModuleFile::ModuleFile(ModuleFile &&) noexcept = default;
ModuleFile &ModuleFile::operator=(ModuleFile &&) noexcept = default;

ModuleFile ModuleFile::open(const std::string &path) {
  Family family = detect_family(path);

  ModuleFile mf;
  mf.family_ = family;
  mf.impl_ = std::make_unique<Impl>();

  switch (family) {
  case Family::Gfortran: {
    gfortran::Module m = gfortran::read_module(path);
    mf.version_ = m.version;
    mf.name_ = m.name();
    mf.impl_->data = std::move(m);
    return mf;
  }
  case Family::Flang: {
    flang::Module m = flang::read_module(path);
    mf.version_ = "v1";
    // Flang derives the module name from the filename too (see
    // flang/docs/ModFiles.md, "Name"); do the same rather than parsing the
    // body's `module <name>` statement, which would mean parsing Fortran.
    std::string stem = path;
    std::size_t slash = stem.find_last_of("/\\");
    if (slash != std::string::npos)
      stem = stem.substr(slash + 1);
    std::size_t dot = stem.rfind(".mod");
    if (dot != std::string::npos && dot == stem.size() - 4)
      stem = stem.substr(0, dot);
    mf.name_ = stem;
    mf.impl_->data = std::move(m);
    return mf;
  }
  case Family::Ifx: {
    ifx::Module m = ifx::read_module(path); // throws for an unverified major_id
    mf.version_ = std::to_string(m.major_id) + "." + std::to_string(m.minor_id);
    mf.name_ = m.name();
    mf.impl_->data = std::move(m);
    return mf;
  }
  case Family::Unknown:
    throw Error(path + ": not a recognized gfortran, Flang, or ifx module file");
  }
  throw Error(path + ": not a recognized gfortran, Flang, or ifx module file");
}

EmitResult ModuleFile::emit_fortran_source(bool strict) const {
  EmitOptions options;
  options.strict = strict;
  return emit_fortran_source(options);
}

EmitResult ModuleFile::emit_fortran_source(const EmitOptions &options) const {
  const bool strict = options.strict;
  if (Family::Gfortran == family_) {
    const auto &m = std::get<gfortran::Module>(impl_->data);
    gfortran::EmitResult r =
        gfortran::emit_fortran_source(m, strict, options.unsupported_kinds);
    return EmitResult{std::move(r.source), std::move(r.problems)};
  }
  if (Family::Flang == family_) {
    // The body already is Fortran source (see flang/docs/ModFiles.md);
    // nothing to translate. !need$ dependency lines are left in place --
    // they're valid Fortran comments, and stripping them would lose real
    // information about what the module depends on.
    const auto &m = std::get<flang::Module>(impl_->data);
    return EmitResult{m.body, {}};
  }
  if (Family::Ifx == family_) {
    const auto &m = std::get<ifx::Module>(impl_->data);
    ifx::EmitResult r = ifx::emit_fortran_source(m, strict);
    return EmitResult{std::move(r.source), std::move(r.problems)};
  }
  throw Error("emit_fortran_source: module family is not open");
}

std::vector<ModuleFile::SymbolSummary> ModuleFile::gfortran_public_symbols() const {
  if (family_ != Family::Gfortran)
    throw Error("gfortran_public_symbols() is only valid for gfortran modules");
  const auto &m = std::get<gfortran::Module>(impl_->data);
  std::map<int, gfortran::Symbol> symbols = gfortran::parse_symbols(m);
  std::map<std::string, int> symtree = gfortran::parse_symtree(m);

  std::vector<SymbolSummary> out;
  out.reserve(symtree.size());
  for (const auto &[public_name, number] : symtree) {
    SymbolSummary s;
    s.name = public_name;
    auto it = symbols.find(number);
    if (it == symbols.end()) {
      s.flavor = "<missing>";
    } else {
      s.flavor = it->second.flavor();
      s.is_artificial = it->second.is_artificial();
    }
    out.push_back(std::move(s));
  }
  return out;
}

std::vector<ModuleFile::IfxEntitySummary> ModuleFile::ifx_entities() const {
  if (family_ != Family::Ifx)
    throw Error("ifx_entities() is only valid for ifx modules");
  const auto &m = std::get<ifx::Module>(impl_->data);
  std::vector<IfxEntitySummary> out;
  out.reserve(m.entities.size());
  for (const ifx::Entity &e : m.entities) {
    IfxEntitySummary s;
    s.name = e.name;
    s.decl_rec_kind = static_cast<std::uint32_t>(e.decl_rec_kind);
    s.sym_id = e.sym_id;
    s.klass = static_cast<std::uint8_t>(e.klass);
    s.skind = static_cast<std::uint8_t>(e.skind);
    s.value = ifx::parse_scalar_constant(e);

    if (std::optional<ifx::DummyArgInfo> arg = ifx::parse_dummy_arg(e))
      s.dummy_arg_type = ifx::to_fortran_type(arg->type);
    if (std::optional<ifx::CharArgInfo> char_arg = ifx::parse_char_arg(e))
      s.dummy_arg_type = char_arg->is_assumed_length
                              ? "character(*)"
                              : "character(len=" +
                                    std::to_string(char_arg->length) + ")";

    if (std::optional<ifx::SimpleScalarFieldInfo> field =
            ifx::parse_simple_scalar_field(e)) {
      if (!field->has_undecoded_initializer)
        s.field_type = ifx::to_fortran_type(field->type);
    }
    if (std::optional<ifx::CharFieldInfo> char_field = ifx::parse_char_field(e))
      s.field_type = "character(len=" + std::to_string(char_field->length) + ")";
    if (std::optional<ifx::StructFieldInfo> struct_field = ifx::parse_struct_field(e)) {
      if (struct_field->type_name)
        s.field_type = "type(" + struct_field->type_name->name + ")";
    }
    if (std::optional<ifx::CommonVariableInfo> cv = ifx::parse_common_variable(e))
      s.field_type = ifx::to_fortran_type(cv->type);

    if (std::optional<ifx::StructVariableInfo> sv =
            ifx::parse_struct_variable(e)) {
      if (sv->type_name)
        s.struct_type_name = sv->type_name->name;
    }

    if (std::optional<ifx::SimpleArrayInfo> arr = ifx::parse_simple_array(e)) {
      s.array_element_type = ifx::to_fortran_type(arr->type);
      s.array_size = arr->size;
      s.array_ndimen = arr->ndimen;
      if (std::optional<std::vector<ifx::ArrayBound>> bounds =
              ifx::parse_array_bounds(e)) {
        std::string joined;
        for (std::size_t i = 0; i < bounds->size(); ++i) {
          if (i)
            joined += ", ";
          joined += std::to_string((*bounds)[i].lower) + ":" +
                     std::to_string((*bounds)[i].upper);
        }
        s.array_bounds = std::move(joined);
      }
    }
    if (std::optional<ifx::StructArrayInfo> sarr = ifx::parse_struct_array(e)) {
      if (sarr->type_name)
        s.array_element_type = "type(" + sarr->type_name->name + ")";
      s.array_size = sarr->size;
      s.array_ndimen = sarr->ndimen;
      if (std::optional<std::vector<ifx::ArrayBound>> bounds =
              ifx::parse_struct_array_bounds(e)) {
        std::string joined;
        for (std::size_t i = 0; i < bounds->size(); ++i) {
          if (i)
            joined += ", ";
          joined += std::to_string((*bounds)[i].lower) + ":" +
                     std::to_string((*bounds)[i].upper);
        }
        s.array_bounds = std::move(joined);
      }
    }

    if (std::optional<ifx::ProcedureInfo> proc = ifx::parse_procedure(e)) {
      s.is_function = proc->return_type.has_value();
      if (proc->return_type)
        s.return_type = ifx::to_fortran_type(*proc->return_type);
      for (const ifx::NamedRef &arg : proc->arguments)
        s.argument_names.push_back(arg.name);
    }

    if (std::optional<ifx::DerivedTypeFields> fields =
            ifx::parse_derived_type(e)) {
      for (const ifx::NamedRef &field : *fields)
        s.field_names.push_back(field.name);
    }

    if (std::optional<ifx::CommonBlockVariables> vars =
            ifx::parse_common_block(e)) {
      for (const ifx::NamedRef &v : *vars)
        s.common_variable_names.push_back(v.name);
    }

    out.push_back(std::move(s));
  }
  return out;
}

} // namespace fxmod
