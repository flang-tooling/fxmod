// Generic tokenizer + tree parser for the S-expression body found inside a
// gunzipped gfortran module file. Deliberately version-independent: the
// gfortran module schema differs only in which atoms appear where, never in
// the lexical grammar itself (parens, single-quoted strings with '' escaping,
// decimal integers, bare names).
#pragma once

#include <cstdint>
#include <string>
#include <variant>
#include <vector>

#include <fxmod/error.hpp>

namespace fxmod::sexpr {

// Thrown for anything the reader cannot make sense of. Callers should treat
// this as "refuse, don't guess" rather than trying to recover.
class FormatError : public fxmod::Error {
public:
  using fxmod::Error::Error;
};

// A single-quoted string atom, kept distinct from a bare name so callers can
// tell 'DERIVED' (a name/keyword) apart from an actual string value.
struct Str {
  std::string value;
};

// A bare identifier atom, e.g. DERIVED or UNKNOWN-INTENT.
struct Name {
  std::string value;
};

class Node;
using List = std::vector<Node>;

// A parsed node is one of: string atom, name atom, integer atom, or a list
// of further nodes. Mirrors the Python prototype's Str | Name | int | list.
class Node {
public:
  Node() : data_(std::int64_t{0}) {}
  Node(Str s) : data_(std::move(s)) {}
  Node(Name n) : data_(std::move(n)) {}
  Node(std::int64_t i) : data_(i) {}
  Node(List l) : data_(std::move(l)) {}

  bool is_str() const { return std::holds_alternative<Str>(data_); }
  bool is_name() const { return std::holds_alternative<Name>(data_); }
  bool is_int() const { return std::holds_alternative<std::int64_t>(data_); }
  bool is_list() const { return std::holds_alternative<List>(data_); }

  const std::string &str() const { return std::get<Str>(data_).value; }
  const std::string &name() const { return std::get<Name>(data_).value; }
  std::int64_t integer() const { return std::get<std::int64_t>(data_); }
  const List &list() const { return std::get<List>(data_); }

  // True if this is a Name atom equal to `n`.
  bool is_name(const std::string &n) const {
    return is_name() && name() == n;
  }

private:
  std::variant<Str, Name, std::int64_t, List> data_;
};

// Parse a whole token stream (the body of a module file, after its header
// line) into a list of top-level items. Throws FormatError on malformed
// input (unbalanced parens, unrecognised characters).
List parse_forest(const std::string &text);

// Render a node back to the same textual notation gfortran writes, for
// debugging/dump output. Not used for round-tripping module files.
std::string render(const Node &node, int indent = 0);

} // namespace fxmod::sexpr
