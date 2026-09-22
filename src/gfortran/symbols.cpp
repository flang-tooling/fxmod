#include "symbols.hpp"

#include <algorithm>
#include <regex>

namespace fxmod::gfortran {

using sexpr::List;
using sexpr::Node;

const char *const kArtificialPrefixes[] = {"__def_init_", "__copy_",
                                            "__vtab_", "__vtype_"};
const std::size_t kArtificialPrefixCount =
    sizeof(kArtificialPrefixes) / sizeof(kArtificialPrefixes[0]);
const char *const kIntrinsicModule = "(intrinsic)";

namespace {

bool contains(const std::vector<std::string> &v, const std::string &s) {
  return std::find(v.begin(), v.end(), s) != v.end();
}

bool starts_with(const std::string &s, const std::string &prefix) {
  return s.size() >= prefix.size() &&
         s.compare(0, prefix.size(), prefix) == 0;
}

std::string lower_copy(const std::string &s) {
  std::string out = s;
  std::transform(out.begin(), out.end(), out.begin(), ::tolower);
  return out;
}

// Fortran module names are case-insensitive; the module file preserves
// whatever case the symbol's own defining module used, which need not
// match the current module's own spelling.
bool same_module(const std::string &a, const std::string &b) {
  return lower_copy(a) == lower_copy(b);
}

} // namespace

ResolvedModuleName resolve_module_name(const std::string &module_name) {
  if (starts_with(module_name, "__"))
    return {module_name.substr(2), true};
  return {module_name, false};
}

void record_use(NeededUses &uses, const std::string &module_name,
                 const std::string &real_name, const std::string &local_name) {
  ResolvedModuleName resolved = resolve_module_name(module_name);
  std::string key = lower_copy(resolved.name);
  ModuleUse &mu = uses[key];
  mu.display_name = resolved.name;
  mu.intrinsic = resolved.intrinsic;
  mu.only_clauses.insert(same_module(real_name, local_name)
                              ? local_name
                              : local_name + " => " + real_name);
}

namespace {

std::vector<Component> parse_components(const Node &node) {
  std::vector<Component> components;
  if (!node.is_list())
    return components;
  for (const Node &entry : node.list()) {
    if (entry.is_list() && entry.list().size() >= 3 &&
        entry.list()[0].is_int() && entry.list()[1].is_str()) {
      Component c;
      c.name = entry.list()[1].str();
      c.typespec = parse_typespec(entry.list()[2]);
      components.push_back(std::move(c));
    }
  }
  return components;
}

// A bound expression, from mio_expr() -- see module.cc. Only two shapes
// are supported: a CONSTANT (rendered immediately; never references
// another symbol) and a VARIABLE (a plain reference to another symbol by
// number, e.g. a dummy argument used as an extent -- kept unresolved, see
// the symbols.hpp file comment). An absent bound is `()`. Anything else
// (EXPR_OP, EXPR_FUNCTION, ...) is refused: general expressions in array
// bounds aren't decoded.
ArrayBound parse_bound(const Node &node) {
  ArrayBound b;
  if (!node.is_list() || node.list().empty())
    return b; // Kind::Absent
  const List &l = node.list();
  if (!l[0].is_name())
    throw sexpr::FormatError("array bound is not a recognised expression");
  const std::string &kind = l[0].name();

  if (kind == "CONSTANT") {
    // Same shape as a symbol's PARAMETER value: (CONSTANT <typespec> <rank>
    // <value> ...). Bounds are always INTEGER in valid Fortran.
    if (l.size() < 4 || !l[3].is_str())
      throw sexpr::FormatError("unsupported constant array bound");
    static const std::regex kDecimal(R"(-?\d+)");
    if (!std::regex_match(l[3].str(), kDecimal))
      throw sexpr::FormatError("unsupported constant array bound");
    b.kind = ArrayBound::Kind::Constant;
    b.constant_text = l[3].str();
    return b;
  }
  if (kind == "VARIABLE") {
    // (VARIABLE <typespec> <rank> <symtree-ref> <ref-list> ...) --
    // mio_symtree_ref() writes a bare symbol number.
    if (l.size() < 4 || !l[3].is_int())
      throw sexpr::FormatError("unsupported variable array bound");
    b.kind = ArrayBound::Kind::SymbolRef;
    b.symbol_ref = static_cast<int>(l[3].integer());
    return b;
  }
  throw sexpr::FormatError("array bound expression '" + kind +
                            "' is not decoded (only constants and plain "
                            "symbol references are)");
}

std::string bound_text(const ArrayBound &b, const std::map<int, Symbol> &symbols) {
  switch (b.kind) {
  case ArrayBound::Kind::Absent:
    return "";
  case ArrayBound::Kind::Constant:
    return b.constant_text;
  case ArrayBound::Kind::SymbolRef: {
    auto it = symbols.find(b.symbol_ref);
    if (it == symbols.end())
      throw sexpr::FormatError("array bound references unknown symbol " +
                                std::to_string(b.symbol_ref));
    return it->second.name;
  }
  }
  return "";
}

// Returns (symbol pool, symtree): the last two top-level items in the
// module body, which is more robust than indexing from the front should a
// future version add a leading section.
std::pair<const List &, const List &> sections(const Module &module) {
  if (module.forest.size() < 2 || !module.forest[module.forest.size() - 2].is_list() ||
      !module.forest[module.forest.size() - 1].is_list()) {
    throw sexpr::FormatError(module.path +
                              ": expected a symbol pool and symtree at the "
                              "end of the module body, found " +
                              std::to_string(module.forest.size()) +
                              " top-level items");
  }
  return {module.forest[module.forest.size() - 2].list(),
          module.forest[module.forest.size() - 1].list()};
}

} // namespace

std::optional<TypeSpec> parse_typespec(const Node &node) {
  if (!node.is_list() || node.list().empty())
    return std::nullopt;
  const List &l = node.list();
  const Node &head = l[0];
  if (!head.is_name())
    return std::nullopt;
  const std::string &base = head.name();
  if (base == "UNKNOWN")
    return std::nullopt;

  auto second_as_int = [&]() -> std::optional<int> {
    if (l.size() > 1 && l[1].is_int())
      return static_cast<int>(l[1].integer());
    return std::nullopt;
  };

  if (base == "DERIVED" || base == "CLASS") {
    TypeSpec ts;
    ts.base = "DERIVED";
    ts.derived_ref = second_as_int();
    return ts;
  }
  TypeSpec ts;
  ts.base = base;
  ts.kind = second_as_int();
  return ts;
}

std::string TypeSpec::to_fortran(const std::map<int, Symbol> &symbols,
                                  const std::string &current_module,
                                  NeededUses &uses) const {
  if (base == "DERIVED") {
    if (!derived_ref.has_value())
      throw sexpr::FormatError("derived typespec without a symbol ref");
    auto it = symbols.find(*derived_ref);
    if (it == symbols.end())
      throw sexpr::FormatError("derived typespec references unknown symbol " +
                                std::to_string(*derived_ref));
    if (!same_module(it->second.module_name, current_module))
      record_use(uses, it->second.module_name, it->second.name,
                 it->second.name);
    return "type(" + it->second.name + ")";
  }
  if (base == "INTEGER" || base == "REAL" || base == "LOGICAL" ||
      base == "COMPLEX") {
    if (!kind.has_value())
      throw sexpr::FormatError(base + " typespec has no kind");
    std::string lower = base;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
    return lower + "(" + std::to_string(*kind) + ")";
  }
  if (base == "CHARACTER") {
    if (!kind.has_value())
      throw sexpr::FormatError("character typespec has no kind");
    return "character(kind=" + std::to_string(*kind) + ")";
  }
  throw sexpr::FormatError("unsupported base type " + base);
}

std::optional<ArraySpec> parse_array_spec(const Node &node) {
  if (!node.is_list() || node.list().empty())
    return std::nullopt;
  const List &l = node.list();
  if (l.size() < 3 || !l[0].is_int() || !l[1].is_int() || !l[2].is_name())
    throw sexpr::FormatError("malformed array spec");

  int rank = static_cast<int>(l[0].integer());
  int corank = static_cast<int>(l[1].integer());
  if (corank != 0)
    throw sexpr::FormatError("coarray dimension is not decoded");

  ArraySpec as;
  as.type = l[2].name();
  as.rank = rank;

  static const std::vector<std::string> kKnownTypes = {
      "EXPLICIT", "ASSUMED_SHAPE", "DEFERRED", "ASSUMED_SIZE", "ASSUMED_RANK"};
  if (std::find(kKnownTypes.begin(), kKnownTypes.end(), as.type) ==
      kKnownTypes.end())
    throw sexpr::FormatError("unrecognised array spec type '" + as.type + "'");

  if (as.type == "ASSUMED_RANK")
    return as; // no per-dimension bounds on disk (rank was written as 0)

  std::size_t needed = 3 + static_cast<std::size_t>(rank) * 2;
  if (l.size() < needed)
    throw sexpr::FormatError("array spec is missing dimension bounds");
  for (int i = 0; i < rank; ++i) {
    ArrayBound lower = parse_bound(l[3 + 2 * i]);
    ArrayBound upper = parse_bound(l[3 + 2 * i + 1]);
    as.dims.emplace_back(lower, upper);
  }
  return as;
}

std::string ArraySpec::to_fortran(const std::map<int, Symbol> &symbols) const {
  if (type == "ASSUMED_RANK")
    return "(..)";

  std::string out = "(";
  for (std::size_t i = 0; i < dims.size(); ++i) {
    if (i)
      out += ", ";
    const ArrayBound &lo = dims[i].first;
    const ArrayBound &hi = dims[i].second;
    std::string lo_text = bound_text(lo, symbols);
    bool last = (i + 1 == dims.size());

    if (type == "EXPLICIT") {
      out += (lo_text.empty() || lo_text == "1" ? "" : lo_text + ":") +
             bound_text(hi, symbols);
    } else if (type == "ASSUMED_SHAPE") {
      out += (lo_text.empty() || lo_text == "1" ? "" : lo_text) + ":";
    } else if (type == "DEFERRED") {
      out += ":";
    } else if (type == "ASSUMED_SIZE") {
      if (last)
        out += (lo_text.empty() || lo_text == "1" ? "*" : lo_text + ":*");
      else
        out += (lo_text.empty() || lo_text == "1" ? "" : lo_text + ":") +
               bound_text(hi, symbols);
    }
  }
  out += ")";
  return out;
}

std::optional<std::string> Symbol::intent() const {
  if (attributes.size() < 2)
    return std::nullopt;
  const std::string &a = attributes[1];
  if (a == "IN")
    return "in";
  if (a == "OUT")
    return "out";
  if (a == "INOUT")
    return "inout";
  return std::nullopt;
}

bool Symbol::is_dummy() const { return contains(attributes, "DUMMY"); }
bool Symbol::is_generic() const { return contains(attributes, "GENERIC"); }
bool Symbol::is_function() const { return contains(attributes, "FUNCTION"); }

std::string Symbol::flavor() const {
  return attributes.empty() ? "UNKNOWN" : attributes[0];
}

bool Symbol::is_artificial() const {
  if (contains(attributes, "ARTIFICIAL"))
    return true;
  if (module_name == kIntrinsicModule)
    return true;
  for (std::size_t i = 0; i < kArtificialPrefixCount; ++i)
    if (starts_with(name, kArtificialPrefixes[i]))
      return true;
  return !name.empty() && name.front() == '_';
}

std::map<int, Symbol> parse_symbols(const Module &module) {
  std::map<int, Symbol> symbols;
  const List &flat = sections(module).first;

  std::size_t i = 0;
  while (flat.size() >= 6 && i <= flat.size() - 6) {
    if (flat[i].is_int() && flat[i + 1].is_str() && flat[i + 2].is_str() &&
        flat[i + 3].is_str() && flat[i + 4].is_int() && flat[i + 5].is_list()) {
      int number = static_cast<int>(flat[i].integer());
      std::string name = flat[i + 1].str();
      std::string mod = flat[i + 2].str();
      const List &body = flat[i + 5].list();

      std::vector<std::string> attrs;
      if (!body.empty() && body[0].is_list())
        for (const Node &a : body[0].list())
          if (a.is_name())
            attrs.push_back(a.name());

      const Node *raw_components =
          body.size() > 1 ? &body[1] : nullptr;
      std::vector<Component> components =
          raw_components ? parse_components(*raw_components)
                          : std::vector<Component>{};
      // The component-access atom is written only when the component list
      // is non-empty, which shifts everything after it by one.
      bool has_components = raw_components && raw_components->is_list() &&
                             !raw_components->list().empty();
      std::size_t ts_index = has_components ? 3 : 2;
      std::optional<TypeSpec> typespec =
          body.size() > ts_index ? parse_typespec(body[ts_index])
                                  : std::optional<TypeSpec>{};

      // mio_symbol writes, after the typespec:
      //   formal_ns, common_next, formal_arglist,
      //   [value -- only when the flavor is PARAMETER],
      //   array_spec, result, ...
      // so everything after the arglist shifts for named constants.
      auto at = [&](std::size_t offset) -> const Node * {
        std::size_t idx = ts_index + offset;
        return body.size() > idx ? &body[idx] : nullptr;
      };

      const Node *formal_node = at(3);
      std::vector<int> formal_args;
      if (formal_node && formal_node->is_list())
        for (const Node &x : formal_node->list())
          if (x.is_int())
            formal_args.push_back(static_cast<int>(x.integer()));

      bool is_parameter = contains(attrs, "PARAMETER");
      std::optional<Node> value_node;
      if (is_parameter) {
        if (const Node *v = at(4))
          value_node = *v;
      }
      const Node *array_node = is_parameter ? at(5) : at(4);
      std::optional<ArraySpec> array_spec;
      if (array_node) {
        try {
          array_spec = parse_array_spec(*array_node);
        } catch (const sexpr::FormatError &) {
          // A genuine array spec this reader can't decode (a coarray, or
          // an unsupported bound expression) -- leave array_spec unset so
          // the symbol is treated the same as "not an array" for now, and
          // is refused later (at emission time) via its absence rather
          // than crashing parse_symbols() itself. parse_array_spec()
          // already only throws for a *recognised* but unsupported spec,
          // never for something that plainly isn't one.
        }
      }

      Symbol sym;
      sym.number = number;
      sym.name = std::move(name);
      sym.module_name = std::move(mod);
      sym.attributes = std::move(attrs);
      sym.typespec = std::move(typespec);
      sym.components = std::move(components);
      sym.value_node = std::move(value_node);
      sym.formal_args = std::move(formal_args);
      sym.array_spec = std::move(array_spec);
      symbols[number] = std::move(sym);

      i += 6;
      continue;
    }
    ++i;
  }
  return symbols;
}

std::map<std::string, int> parse_symtree(const Module &module) {
  std::map<std::string, int> tree;
  const List &flat = sections(module).second;
  if (flat.size() % 3 != 0)
    throw sexpr::FormatError(module.path + ": symtree has " +
                              std::to_string(flat.size()) +
                              " atoms, not a multiple of 3");
  for (std::size_t i = 0; i < flat.size(); i += 3) {
    const Node &name = flat[i];
    const Node &number = flat[i + 2];
    if (!name.is_str() || !number.is_int())
      throw sexpr::FormatError(module.path +
                                ": malformed symtree entry at index " +
                                std::to_string(i));
    tree[name.str()] = static_cast<int>(number.integer());
  }
  return tree;
}

std::map<std::string, GenericInterface> parse_generic_interfaces(const Module &module) {
  std::map<std::string, GenericInterface> out;
  // forest[2] -- the third top-level section, counting from the front.
  // Unlike the symbol pool and symtree (sections(), counted from the
  // back), this is a fixed front-relative index, verified directly
  // against real compiled output for both "15" and "16" rather than
  // assumed from the original prototype's documented layout.
  if (module.forest.size() <= 2 || !module.forest[2].is_list())
    return out;
  for (const Node &entry : module.forest[2].list()) {
    if (!entry.is_list() || entry.list().size() < 3)
      continue;
    const List &e = entry.list();
    if (!e[0].is_str())
      continue;
    GenericInterface gi;
    gi.name = e[0].str();
    for (std::size_t i = 2; i < e.size(); ++i)
      if (e[i].is_int())
        gi.specifics.push_back(static_cast<int>(e[i].integer()));
    if (!gi.specifics.empty()) {
      std::string key = gi.name;
      std::transform(key.begin(), key.end(), key.begin(), ::tolower);
      out[key] = std::move(gi);
    }
  }
  return out;
}

} // namespace fxmod::gfortran
