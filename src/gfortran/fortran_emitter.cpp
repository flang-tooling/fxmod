#include "fortran_emitter.hpp"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <regex>
#include <set>

#include "symbols.hpp"

namespace fxmod::gfortran {

namespace {

struct Analysis {
  std::vector<std::string> body;
  std::vector<std::string> problems;
  NeededUses uses;
};

bool has_attr(const Symbol &s, const std::string &attr) {
  return std::find(s.attributes.begin(), s.attributes.end(), attr) !=
         s.attributes.end();
}

std::string lower(const std::string &s) {
  std::string out = s;
  std::transform(out.begin(), out.end(), out.begin(), ::tolower);
  return out;
}

// A REAL constant's value as gfortran writes it: mpfr_get_str() in base 16
// (mio_gmp_real() in module.cc), i.e. an optional sign, "0.", hexadecimal
// mantissa digits, then "@" and the power of 16 in decimal -- "-0.199999a@0"
// is -0x0.199999a, "0.78@31" is 0x0.78 * 16^31. Rendered as a decimal
// literal with enough digits to round-trip at `kind` (the value is exactly
// representable there, having been rounded to it by gfortran), kind
// suffix included. Kinds without a C++ double to carry them (10, 16) and
// the non-finite spellings are refused.
std::optional<std::string> real_literal(const std::string &text, int kind) {
  static const std::regex kMpfr(R"((-?)0\.([0-9a-f]+)@(-?\d+))");
  std::smatch m;
  if ((kind != 4 && kind != 8) || !std::regex_match(text, m, kMpfr))
    return std::nullopt;
  // The same number as a C99 hexadecimal float: each hex digit is 4 bits.
  const long exponent = std::stol(m[3].str()) * 4;
  const std::string hex = m[1].str() + "0x0." + m[2].str() + "p" +
                           std::to_string(exponent);
  errno = 0;
  const double value = std::strtod(hex.c_str(), nullptr);
  if (errno != 0 || !std::isfinite(value))
    return std::nullopt;
  char buf[64];
  std::snprintf(buf, sizeof buf, kind == 4 ? "%.9g" : "%.17g", value);
  std::string out = buf;
  if (out.find_first_of(".e") == std::string::npos)
    out += ".0"; // "42" would be an INTEGER literal
  return out + "_" + std::to_string(kind);
}

// A character constant's value, from gfortran's escaped spelling, as a
// Fortran expression: printable runs quoted, anything else as achar().
std::optional<std::string> character_literal(const std::string &text) {
  std::vector<std::string> parts;
  std::string run;
  auto flush = [&]() {
    if (!run.empty())
      parts.push_back("'" + run + "'");
    run.clear();
  };
  for (std::size_t i = 0; i < text.size(); ++i) {
    unsigned code;
    if (text[i] == '\\' && i + 1 < text.size() && text[i + 1] == '\\') {
      code = '\\';
      ++i;
    } else if (text[i] == '\\' && i + 9 < text.size() && text[i + 1] == 'U') {
      code = static_cast<unsigned>(std::stoul(text.substr(i + 2, 8), nullptr, 16));
      i += 9;
    } else {
      code = static_cast<unsigned char>(text[i]);
    }
    if (code > 255)
      return std::nullopt; // not a kind=1 character
    if (code >= 0x20 && code < 0x7f) {
      run += static_cast<char>(code);
      if (code == '\'')
        run += '\''; // doubled inside a quoted run
    } else {
      flush();
      parts.push_back("achar(" + std::to_string(code) + ")");
    }
  }
  flush();
  if (parts.empty())
    return "''";
  std::string out;
  for (std::size_t i = 0; i < parts.size(); ++i)
    out += (i ? "//" : "") + parts[i];
  return out;
}

// Renders a raw value expression node (see mio_expr() in module.cc) as
// Fortran text, recursively for a structure constructor's component
// values. Only two expression kinds are supported: CONSTANT (a scalar
// literal -- integer, real, complex, logical, or character) and STRUCTURE (a derived-
// type constant, e.g. a TYPE(foo) PARAMETER initialised to foo(1, 2)).
// Returns nullopt for anything else (a general expression, an array
// constructor, ...) so the caller can refuse rather than guess. Deferred
// to emission time (rather than done once in parse_symbols()) because a
// structure constructor names its type by symbol number, which may not
// exist yet in the pool map being built at parse time -- see the
// symbols.hpp file comment.
std::optional<std::string> render_value(const sexpr::Node &node,
                                         const std::map<int, Symbol> &symbols,
                                         const std::string &current_module,
                                         NeededUses &uses) {
  if (!node.is_list() || node.list().empty())
    return std::nullopt;
  const sexpr::List &l = node.list();
  if (!l[0].is_name())
    return std::nullopt;
  const std::string &kind = l[0].name();

  if (kind == "CONSTANT") {
    if (l.size() < 4)
      return std::nullopt;
    std::optional<TypeSpec> ts = parse_typespec(l[1]);
    if (!ts.has_value())
      return std::nullopt;
    const sexpr::Node &raw = l[3];
    if (ts->base == "INTEGER") {
      // gfortran writes the mpz value as a decimal string.
      static const std::regex kDecimal(R"(-?\d+)");
      if (raw.is_str() && std::regex_match(raw.str(), kDecimal))
        return raw.str();
      if (raw.is_int())
        return std::to_string(raw.integer());
    } else if (ts->base == "LOGICAL") {
      if (raw.is_int())
        return raw.integer() != 0 ? ".true." : ".false.";
      if (raw.is_str() && (raw.str() == "0" || raw.str() == "1"))
        return raw.str() == "1" ? ".true." : ".false.";
    } else if (ts->base == "REAL" && ts->kind) {
      if (raw.is_str())
        return real_literal(raw.str(), *ts->kind);
    } else if (ts->base == "COMPLEX" && ts->kind) {
      // Real and imaginary part, each written like a REAL constant.
      if (raw.is_str() && l.size() > 4 && l[4].is_str()) {
        std::optional<std::string> re = real_literal(raw.str(), *ts->kind);
        std::optional<std::string> im = real_literal(l[4].str(), *ts->kind);
        if (re && im)
          return "(" + *re + ", " + *im + ")";
      }
    } else if (ts->base == "CHARACTER") {
      // (CONSTANT <ts> <rank> <length> '<string>'), the string with
      // gfortran's escapes (quote_string() in module.cc): \\ for a
      // backslash, \Uxxxxxxxx for any character that is not printable.
      if (l.size() > 4 && l[4].is_str())
        return character_literal(l[4].str());
    }
    return std::nullopt;
  }

  if (kind == "STRUCTURE") {
    // (STRUCTURE <typespec> <rank> <constructor-list> <shape> ...) --
    // see mio_constructor()/mio_shape() in module.cc. The constructor
    // list has one (value, iterator) entry per component of the derived
    // type, in declaration order, including internal ones -- skip those
    // in lockstep with how emit_derived() skips them when declaring the
    // type, so the positional structure-constructor call this renders
    // matches what was actually declared.
    if (l.size() < 4)
      return std::nullopt;
    std::optional<TypeSpec> ts = parse_typespec(l[1]);
    if (!ts.has_value() || ts->base != "DERIVED" || !ts->derived_ref.has_value())
      return std::nullopt;
    auto type_it = symbols.find(*ts->derived_ref);
    if (type_it == symbols.end())
      return std::nullopt;
    const Symbol &type_sym = type_it->second;
    bool foreign = lower(type_sym.module_name) != lower(current_module);
    if (foreign) {
      if (type_sym.components.empty())
        // A stub: this module's pool didn't serialize the foreign type's
        // component list, so there's no reliable way to tell how many of
        // the constructor's own entries are real fields versus internal
        // vtable machinery (see emit_derived()'s identical concern for
        // re-exporting one of these) -- refuse rather than guess.
        return std::nullopt;
      record_use(uses, type_sym.module_name, type_sym.name, type_sym.name);
    }

    if (!l[3].is_list())
      return std::nullopt;
    const sexpr::List &ctor = l[3].list();
    if (ctor.size() != type_sym.components.size())
      return std::nullopt;

    std::vector<std::string> rendered;
    for (std::size_t i = 0; i < type_sym.components.size(); ++i) {
      if (type_sym.components[i].is_internal())
        continue; // vtable machinery, no source spelling
      if (!ctor[i].is_list() || ctor[i].list().empty())
        return std::nullopt;
      std::optional<std::string> comp_value =
          render_value(ctor[i].list()[0], symbols, current_module, uses);
      if (!comp_value.has_value())
        return std::nullopt;
      rendered.push_back(*comp_value);
    }

    std::string out = type_sym.name + "(";
    for (std::size_t i = 0; i < rendered.size(); ++i) {
      if (i)
        out += ", ";
      out += rendered[i];
    }
    out += ")";
    return out;
  }

  return std::nullopt; // EXPR_OP, EXPR_FUNCTION, EXPR_ARRAY, ... refused
}

// Renders just the "function/subroutine NAME(...) ... end function/
// subroutine NAME" body for a procedure symbol -- no outer interface/end
// interface wrapper, since that differs by context: a standalone
// procedure gets its own (see emit_interface()), but a specific inside a
// generic block must NOT have one -- Fortran doesn't allow an INTERFACE
// statement nested inside an INTERFACE block. The formal argument list is
// a list of symbol numbers; each referenced symbol carries the dummy's
// name, type, intent and (if an array) its shape.
std::vector<std::string> emit_interface_body(const std::string &public_name,
                                              const Symbol &sym,
                                              const std::map<int, Symbol> &symbols,
                                              const std::string &current_module,
                                              NeededUses &uses) {
  // No is_generic() guard here, deliberately: this renders one already-
  // resolved specific (its symbol number came from the caller looking it
  // up in the generic-interfaces section), and that specific's own
  // attributes can still carry GENERIC even when it isn't one in any
  // useful sense -- confirmed on real gfortran-compiled MPI bindings,
  // where e.g. `mpi_init`'s generic-interfaces entry lists symbol 201 as
  // its own single specific: the classic (non-F08) MPI wrapper subroutine
  // is both the generic name and its own (only) specific. Refusing that
  // outright would incorrectly drop core routines (mpi_init, mpi_finalize,
  // mpi_comm_rank/size, mpi_reduce, ...) that have a perfectly good
  // formal_args/typespec of their own to render from. Anything that
  // genuinely lacks a usable signature is still refused below by the
  // ordinary unresolvable-type / missing-result checks, unconditionally.
  std::vector<const Symbol *> dummies;
  for (int ref : sym.formal_args) {
    auto it = symbols.find(ref);
    if (it == symbols.end())
      throw UnsupportedError("dummy argument symbol " + std::to_string(ref) +
                              " is not in the pool");
    const Symbol &dummy = it->second;
    // A procedure dummy may be untyped: a subroutine, or one declared by
    // its interface alone.
    if (!dummy.typespec.has_value() && dummy.flavor() != "PROCEDURE")
      throw UnsupportedError("dummy argument '" + dummy.name +
                              "' has an unrecoverable type");
    dummies.push_back(&dummy);
  }

  const char *kind = sym.is_function() ? "function" : "subroutine";
  if (sym.is_function() && !sym.typespec.has_value())
    throw UnsupportedError("function result type is unrecoverable");

  std::string names;
  for (std::size_t i = 0; i < dummies.size(); ++i) {
    if (i)
      names += ", ";
    names += dummies[i]->name;
  }

  std::vector<std::string> lines;
  lines.push_back(std::string("  ") + kind + " " + public_name + "(" + names + ")");
  // A bare (unnamed) interface body does NOT get automatic host
  // association with the enclosing module's own entities -- unlike a
  // CONTAINed procedure, it needs an explicit IMPORT to see this module's
  // derived types (that's exactly what these procedures reference: gfortran
  // marks every one of them MODULE-PROC). A blanket `import` (no name
  // list) is always legal even when nothing is actually imported.
  lines.push_back("    import");

  if (sym.is_function()) {
    // The result's attributes sit on the function symbol, or on a result
    // variable of its own.
    const Symbol *result = &sym;
    if (sym.result_ref)
      if (auto it = symbols.find(*sym.result_ref); it != symbols.end())
        result = &it->second;
    const std::optional<TypeSpec> &type =
        result->typespec ? result->typespec : sym.typespec;
    std::string decl = type->to_fortran(symbols, current_module, uses);
    if (result->array_spec)
      decl += ", dimension" + result->array_spec->to_fortran(symbols);
    for (const char *attr : {"POINTER", "ALLOCATABLE"})
      if (has_attr(*result, attr) || has_attr(sym, attr))
        decl += ", " + lower(attr);
    if (std::string ca = class_attribute(*type, symbols); !ca.empty() &&
        decl.find(", " + ca) == std::string::npos)
      decl += ", " + ca;
    lines.push_back("    " + decl + " :: " + public_name);
  }

  for (const Symbol *d : dummies) {
    std::string decl;
    if (d->flavor() == "PROCEDURE") {
      // procedure(iface), or an implicit-interface procedure: typed (a
      // function) or not (a subroutine, or not known to be either).
      if (d->typespec && d->typespec->interface_ref) {
        auto iface = symbols.find(*d->typespec->interface_ref);
        if (iface == symbols.end())
          throw UnsupportedError("procedure dummy '" + d->name +
                                  "' has an unknown interface");
        if (lower(iface->second.module_name) != lower(current_module))
          record_use(uses, iface->second.module_name, iface->second.name,
                     iface->second.name);
        decl = "procedure(" + iface->second.name + ")";
      } else if (d->typespec) {
        decl = d->typespec->to_fortran(symbols, current_module, uses) +
               ", external";
      } else {
        decl = "external";
      }
      if (has_attr(*d, "PROC_POINTER") || has_attr(*d, "POINTER")) {
        decl += ", pointer";
        if (auto intent = d->intent())
          decl += ", intent(" + *intent + ")";
      }
    } else {
      decl = d->typespec->to_fortran(symbols, current_module, uses);
      if (d->array_spec)
        decl += ", dimension" + d->array_spec->to_fortran(symbols);
      if (auto intent = d->intent())
        decl += ", intent(" + *intent + ")";
      for (const char *attr : {"VALUE", "POINTER", "ALLOCATABLE", "TARGET",
                               "CONTIGUOUS"})
        if (has_attr(*d, attr))
          decl += ", " + lower(attr);
      if (std::string ca = class_attribute(*d->typespec, symbols); !ca.empty() &&
          decl.find(", " + ca) == std::string::npos)
        decl += ", " + ca;
    }
    if (has_attr(*d, "OPTIONAL"))
      decl += ", optional";
    lines.push_back("    " + decl + " :: " + d->name);
  }
  // gfortran's NO_ARG_CHECK (type, kind and rank of the actual go
  // unchecked, e.g. MPI and OpenACC choice buffers) in both compilers'
  // spellings -- each ignores the other's directive. Without it a generic
  // over such specifics can be ambiguous by the standard's rules.
  for (const Symbol *d : dummies)
    if (d->ext_attr & Symbol::kExtAttrNoArgCheck) {
      lines.push_back("    !GCC$ ATTRIBUTES NO_ARG_CHECK :: " + d->name);
      lines.push_back("    !DIR$ IGNORE_TKR (tkr) " + d->name);
    }

  lines.push_back(std::string("  end ") + kind + " " + public_name);
  return lines;
}

// A standalone explicit interface for one procedure: emit_interface_body()
// wrapped in its own interface/end interface block.
std::vector<std::string> emit_interface(const std::string &public_name,
                                         const Symbol &sym,
                                         const std::map<int, Symbol> &symbols,
                                         const std::string &current_module,
                                         NeededUses &uses) {
  std::vector<std::string> lines;
  lines.push_back(has_attr(sym, "ABSTRACT") ? "abstract interface" : "interface");
  std::vector<std::string> body =
      emit_interface_body(public_name, sym, symbols, current_module, uses);
  lines.insert(lines.end(), body.begin(), body.end());
  lines.push_back("end interface");
  return lines;
}

// Renders a generic interface block: each specific's *bare* body (no
// individual interface/end interface wrapper -- Fortran doesn't allow
// nesting those), wrapped once under the generic's public name.
//
// `specifics_already_declared` is shared mutable state across every
// generic in the module: a specific declared once (as part of whichever
// generic reaches it first) can't be declared *again* under a second
// public name that also happens to claim it -- gfortran allows one
// underlying procedure to be a specific of two different generics (real
// example: OpenACC's `acc_wait` and its deprecated alias
// `acc_async_wait` both list the same acc_wait_h), but Fortran itself
// does not allow the same explicit interface to be declared twice. The
// second generic names it in a `procedure ::` statement instead, which
// any procedure with an explicit interface may appear in. So does a
// generic of this module whose specific comes from another module that
// this module also re-exports by name (`reexported`): the `use` statement
// doing that provides its interface. Any other foreign specific has its
// interface inlined like a local one -- its module need not be installed
// at all (gfortran's openacc.mod takes its specifics from an
// openacc_internal whose module file it never ships).
//
// `public_name` is the generic-spec: a name, or an "operator(...)" /
// "assignment(=)" spelling for a defined operator.
std::vector<std::string> emit_generic(const std::string &public_name,
                                       const std::vector<int> &specifics,
                                       const std::map<int, Symbol> &symbols,
                                       const std::string &current_module,
                                       NeededUses &uses,
                                       std::set<int> &specifics_already_declared,
                                       const std::set<int> &reexported) {
  // Built up locally and only merged into the shared set once the whole
  // generic succeeds -- a generic that fails partway through (any one
  // specific unsupported) must not "claim" the specifics it did manage to
  // render, or a later, otherwise-fine generic sharing one of them would
  // be wrongly refused as a false duplicate.
  std::vector<int> newly_claimed;
  NeededUses foreign_uses; // merged into `uses` only on success, as above

  std::vector<std::string> lines;
  lines.push_back("interface " + public_name);
  std::set<int> in_this_block;
  for (int specific_num : specifics) {
    if (!in_this_block.insert(specific_num).second)
      continue;
    auto it = symbols.find(specific_num);
    if (it == symbols.end())
      throw UnsupportedError("specific procedure symbol " +
                              std::to_string(specific_num) +
                              " is not in the pool");
    if (it->second.is_artificial())
      throw UnsupportedError("specific procedure '" + it->second.name +
                              "' is an internal compiler-generated symbol "
                              "with no valid Fortran spelling");
    if (reexported.count(specific_num)) {
      record_use(foreign_uses, it->second.module_name, it->second.name,
                 it->second.name);
      lines.push_back("  procedure :: " + it->second.name);
      continue;
    }
    if (specifics_already_declared.count(specific_num)) {
      lines.push_back("  procedure :: " + it->second.name);
      continue;
    }
    std::vector<std::string> body = emit_interface_body(
        it->second.name, it->second, symbols, current_module, uses);
    lines.insert(lines.end(), body.begin(), body.end());
    newly_claimed.push_back(specific_num);
  }
  lines.push_back("end interface " + public_name);
  specifics_already_declared.insert(newly_claimed.begin(), newly_claimed.end());
  for (auto &[key, mu] : foreign_uses) {
    ModuleUse &into = uses[key];
    into.display_name = mu.display_name;
    into.intrinsic = mu.intrinsic;
    into.only_clauses.insert(mu.only_clauses.begin(), mu.only_clauses.end());
  }
  return lines;
}

// gfortran auto-generates a structure-constructor generic for every derived
// type, and it is *that* symbol the symtree points at -- the type itself is
// only reachable through the pool. Index the real types by name so a public
// name resolves to the type whichever of the two it names.
std::map<std::string, const Symbol *>
derived_types_by_name(const std::map<int, Symbol> &symbols) {
  std::map<std::string, const Symbol *> out;
  for (const auto &[number, sym] : symbols) {
    (void)number;
    if (sym.flavor() == "DERIVED" && !sym.is_artificial()) {
      std::string lower = sym.name;
      std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
      out[lower] = &sym;
    }
  }
  return out;
}

Analysis analyse(const std::map<int, Symbol> &symbols,
                  const std::map<std::string, int> &symtree,
                  const std::map<std::string, GenericInterface> &generics,
                  const std::vector<OperatorInterface> &operators,
                  const std::string &current_module) {
  Analysis a;
  std::set<int> emitted;
  std::set<std::string> handled_generic_names;
  std::map<std::string, const Symbol *> derived_by_name =
      derived_types_by_name(symbols);

  // A specific procedure under a generic often *also* has its own public
  // symtree entry (real modules export both, e.g. OpenACC's `acc_wait_h`
  // is both directly callable and a specific of the `acc_wait` generic).
  // Precomputed up front, independent of symtree's alphabetical iteration
  // order, so the standalone-procedure branch below can skip it -- it's
  // rendered once, inside its generic's interface block, never as a
  // second freestanding declaration (which gfortran rejects outright:
  // "already has an explicit interface").
  std::set<int> specifics_already_declared;
  std::set<int> specifics_used_in_generics;
  for (const auto &[key, gi] : generics) {
    (void)key;
    for (int s : gi.specifics)
      specifics_used_in_generics.insert(s);
  }
  for (const OperatorInterface &op : operators)
    for (int s : op.specifics)
      specifics_used_in_generics.insert(s);
  // Public names this module passes on from another one; see the
  // re-export branch below.
  std::set<int> reexported;
  for (const auto &[public_name, number] : symtree) {
    (void)public_name;
    auto it = symbols.find(number);
    if (it != symbols.end() && !it->second.module_name.empty() &&
        it->second.flavor() != "MODULE" && !it->second.is_artificial() &&
        lower(it->second.module_name) != lower(current_module))
      reexported.insert(number);
  }

  auto emit_derived = [&](const std::string &public_name, const Symbol &sym) {
    if (emitted.count(sym.number))
      return;
    if (lower(sym.module_name) != lower(current_module)) {
      // A USE-associated type re-exported under a public name here (e.g.
      // `public :: C_ptr` after `use iso_c_binding`) -- satisfied by
      // re-exporting via `use <module>, only: <name>` instead of
      // redeclaring it locally from this module's own (possibly
      // incomplete) copy of its component list.
      record_use(a.uses, sym.module_name, sym.name, public_name);
      emitted.insert(sym.number);
      return;
    }
    std::vector<std::string> lines;
    lines.push_back("type :: " + public_name);
    for (const Component &comp : sym.components) {
      if (comp.is_internal())
        continue; // vtable machinery, no source spelling
      if (!comp.typespec.has_value()) {
        a.problems.push_back("type '" + public_name + "': component '" +
                              comp.name + "' has a type that cannot be "
                              "expressed");
        return;
      }
      try {
        lines.push_back(
            "  " + comp.typespec->to_fortran(symbols, current_module, a.uses) +
            " :: " + comp.name);
      } catch (const sexpr::FormatError &exc) {
        a.problems.push_back("type '" + public_name + "': " +
                              std::string(exc.what()));
        return;
      }
    }
    lines.push_back("end type " + public_name);
    a.body.insert(a.body.end(), lines.begin(), lines.end());
    emitted.insert(sym.number);
  };

  // Derived types first; later declarations may refer to them.
  for (const auto &[public_name, number] : symtree) {
    (void)number;
    auto it = derived_by_name.find(lower(public_name));
    if (it != derived_by_name.end())
      emit_derived(public_name, *it->second);
  }

  // Then abstract interfaces: a procedure(iface) declaration anywhere below
  // may name one, and must come after it.
  for (const auto &[public_name, number] : symtree) {
    auto sit = symbols.find(number);
    if (sit == symbols.end() || derived_by_name.count(lower(public_name)) ||
        sit->second.flavor() != "PROCEDURE" || !has_attr(sit->second, "ABSTRACT") ||
        lower(sit->second.module_name) != lower(current_module))
      continue;
    try {
      std::vector<std::string> lines = emit_interface(
          public_name, sit->second, symbols, current_module, a.uses);
      a.body.insert(a.body.end(), lines.begin(), lines.end());
      emitted.insert(number);
    } catch (const fxmod::Error &exc) {
      a.problems.push_back("abstract interface '" + public_name + "': " +
                            exc.what());
      emitted.insert(number); // reported once, not again below
    }
  }

  for (const auto &[public_name, number] : symtree) {
    if (derived_by_name.count(lower(public_name)))
      continue; // already handled above
    auto sit = symbols.find(number);
    if (sit == symbols.end() || sit->second.is_artificial() ||
        emitted.count(number))
      continue;
    const Symbol &sym = sit->second;
    if (sym.flavor() == "MODULE")
      continue; // a use-associated module, not a declaration of our own
    if (!sym.module_name.empty() &&
        lower(sym.module_name) != lower(current_module)) {
      // Re-exported from the module that defines it (iso_c_binding's
      // constants and procedures, or a whole module USEd and passed on):
      // this module's copy is only a reference, so `use` the original
      // rather than redeclaring it -- a redeclaration would be a distinct
      // entity, and clash with the original wherever both are visible.
      record_use(a.uses, sym.module_name, sym.name, public_name);
      emitted.insert(number);
      continue;
    }

    if (sym.flavor() == "PARAMETER") {
      if (!sym.typespec.has_value() || !sym.value_node.has_value()) {
        a.problems.push_back("named constant '" + public_name +
                              "': value not recoverable");
        continue;
      }
      std::optional<std::string> value =
          render_value(*sym.value_node, symbols, current_module, a.uses);
      if (!value.has_value()) {
        a.problems.push_back("named constant '" + public_name +
                              "': value not recoverable");
        continue;
      }
      try {
        std::string decl =
            sym.typespec->to_fortran(symbols, current_module, a.uses);
        a.body.push_back(decl + ", parameter :: " + public_name + " = " +
                          *value);
        emitted.insert(number);
      } catch (const sexpr::FormatError &exc) {
        a.problems.push_back("named constant '" + public_name +
                              "': " + exc.what());
      }
    } else if (sym.flavor() == "VARIABLE" && sym.typespec.has_value()) {
      try {
        std::string decl =
            sym.typespec->to_fortran(symbols, current_module, a.uses);
        if (sym.array_spec)
          decl += ", dimension" + sym.array_spec->to_fortran(symbols);
        // Deferred shapes and lengths are only valid on these.
        for (const char *attr : {"ALLOCATABLE", "POINTER", "TARGET"})
          if (has_attr(sym, attr))
            decl += ", " + lower(attr);
        if (std::string ca = class_attribute(*sym.typespec, symbols);
            !ca.empty() && decl.find(", " + ca) == std::string::npos)
          decl += ", " + ca;
        a.body.push_back(decl + " :: " + public_name);
        emitted.insert(number);
      } catch (const sexpr::FormatError &exc) {
        a.problems.push_back("variable '" + public_name + "': " + exc.what());
      }
    } else if (sym.flavor() == "PROCEDURE" && sym.is_generic()) {
      std::string key = lower(public_name);
      auto git = generics.find(key);
      if (git == generics.end()) {
        // Recognised as a generic, but its specifics aren't listed in the
        // generic-interfaces section under this name -- refuse rather
        // than guess which specific it might mean.
        a.problems.push_back("procedure '" + public_name +
                              "': generic interfaces are not translated yet");
        continue;
      }
      handled_generic_names.insert(key);
      try {
        std::vector<std::string> lines = emit_generic(
            public_name, git->second.specifics, symbols, current_module,
            a.uses, specifics_already_declared, reexported);
        a.body.insert(a.body.end(), lines.begin(), lines.end());
        emitted.insert(number);
      } catch (const fxmod::Error &exc) {
        a.problems.push_back("generic '" + public_name + "': " + exc.what());
      }
    } else if (sym.flavor() == "PROCEDURE" && specifics_used_in_generics.count(number)) {
      continue; // already rendered once, inside its generic's block
    } else if (sym.flavor() == "PROCEDURE") {
      try {
        std::vector<std::string> lines =
            emit_interface(public_name, sym, symbols, current_module, a.uses);
        a.body.insert(a.body.end(), lines.begin(), lines.end());
        emitted.insert(number);
      } catch (const fxmod::Error &exc) {
        // Catches both emit_interface()'s own explicit UnsupportedError
        // refusals and the plain sexpr::FormatError TypeSpec::to_fortran()
        // /ArraySpec::to_fortran() throw for something they can't render
        // -- siblings under fxmod::Error, not one derived from the other,
        // so catching only UnsupportedError here would let the latter
        // escape uncaught even under strict=false.
        a.problems.push_back("procedure '" + public_name + "': " + exc.what());
      }
    } else {
      a.problems.push_back("symbol '" + public_name + "': flavor " +
                            sym.flavor() + " is outside the translated "
                            "subset");
    }
  }

  // Generics that exist only in the generic-interfaces section, with no
  // symbol-pool entry of their own -- an explicit `interface NAME ... end
  // interface` block in the source, as opposed to a symbol separately
  // marked GENERIC (handled above, in symtree order like everything
  // else). Runs after the main pass so any derived types it might
  // reference are already declared.
  for (const auto &[key, gi] : generics) {
    if (handled_generic_names.count(key) || derived_by_name.count(key))
      continue;
    if (!gi.module.empty() && lower(gi.module) != lower(current_module)) {
      // A generic USEd from its defining module and passed on.
      record_use(a.uses, gi.module, gi.name, gi.name);
      continue;
    }
    try {
      std::vector<std::string> lines = emit_generic(
          gi.name, gi.specifics, symbols, current_module, a.uses,
          specifics_already_declared, reexported);
      a.body.insert(a.body.end(), lines.begin(), lines.end());
    } catch (const fxmod::Error &exc) {
      a.problems.push_back("generic '" + gi.name + "': " + exc.what());
    }
  }

  // Defined operators and assignment. The module file records no owner for
  // these, only their specifics: those defined elsewhere are reached by
  // using the operator from each specific's own module, the rest get an
  // interface block of this module's own.
  for (const OperatorInterface &op : operators) {
    std::vector<int> local;
    for (int s : op.specifics) {
      auto it = symbols.find(s);
      if (it != symbols.end() && !it->second.module_name.empty() &&
          lower(it->second.module_name) != lower(current_module))
        record_use(a.uses, it->second.module_name, op.spelling, op.spelling);
      else
        local.push_back(s);
    }
    if (local.empty())
      continue;
    try {
      std::vector<std::string> lines =
          emit_generic(op.spelling, local, symbols, current_module, a.uses,
                       specifics_already_declared, reexported);
      a.body.insert(a.body.end(), lines.begin(), lines.end());
    } catch (const fxmod::Error &exc) {
      a.problems.push_back(op.spelling + ": " + exc.what());
    }
  }

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
  std::map<int, Symbol> symbols = parse_symbols(module);
  std::map<std::string, int> symtree = parse_symtree(module);
  if (symtree.empty())
    throw UnsupportedError(module.path + ": no public symbols found");
  std::map<std::string, GenericInterface> generics = parse_generic_interfaces(module);
  std::vector<OperatorInterface> operators = parse_operator_interfaces(module);
  std::string name = module.name();

  Analysis a = analyse(symbols, symtree, generics, operators, name);
  if (!a.problems.empty() && strict) {
    throw UnsupportedError(module.path + ": " +
                            std::to_string(a.problems.size()) +
                            " untranslatable symbol(s): " +
                            summarise_problems(a.problems));
  }

  std::string source = "module " + name + "\n";
  for (const auto &[key, mu] : a.uses) {
    (void)key;
    source += mu.intrinsic ? "use, intrinsic :: " : "use ";
    source += mu.display_name;
    if (!mu.only_clauses.empty()) {
      source += ", only: ";
      bool first = true;
      for (const std::string &clause : mu.only_clauses) {
        if (!first)
          source += ", ";
        source += clause;
        first = false;
      }
    }
    source += "\n";
  }
  source += "implicit none\n";
  for (const std::string &line : a.body)
    source += line + "\n";
  source += "end module " + name + "\n";

  return EmitResult{std::move(source), std::move(a.problems)};
}

} // namespace fxmod::gfortran
