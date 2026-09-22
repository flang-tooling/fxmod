#include "sexpr.hpp"

#include <cctype>
#include <sstream>
#include <vector>

namespace fxmod::sexpr {

namespace {

// Token kinds. LParen/RParen are structural; the rest carry a Node payload.
enum class TokKind { LParen, RParen, Str_, Name_, Int };

struct Token {
  TokKind kind;
  Node node; // unused for LParen/RParen
};

bool is_boundary(char c) {
  return c == '(' || c == ')' || c == '\'' || std::isspace((unsigned char)c);
}

// Tokenizes exactly the grammar the gfortran module writer produces:
// parens, single-quoted strings with '' as an escaped quote, decimal
// integers, and bare names (anything else up to the next boundary
// character). Integers are matched greedily but do NOT require a trailing
// boundary -- "16s" tokenizes as the integer 16 followed by the name "s" --
// which matches how gfortran's own module body never mixes digits and
// letters in one atom, so this only ever matters for malformed input.
std::vector<Token> tokenize(const std::string &text) {
  std::vector<Token> tokens;
  std::size_t pos = 0;
  const std::size_t end = text.size();

  while (pos < end) {
    char c = text[pos];
    if (std::isspace((unsigned char)c)) {
      ++pos;
      continue;
    }
    if (c == '(') {
      tokens.push_back({TokKind::LParen, Node{}});
      ++pos;
      continue;
    }
    if (c == ')') {
      tokens.push_back({TokKind::RParen, Node{}});
      ++pos;
      continue;
    }
    if (c == '\'') {
      std::size_t i = pos + 1;
      std::string value;
      bool closed = false;
      while (i < end) {
        if (text[i] == '\'') {
          if (i + 1 < end && text[i + 1] == '\'') {
            value.push_back('\'');
            i += 2;
            continue;
          }
          closed = true;
          ++i;
          break;
        }
        value.push_back(text[i]);
        ++i;
      }
      if (!closed) {
        throw FormatError("unterminated quoted string at offset " +
                           std::to_string(pos));
      }
      tokens.push_back({TokKind::Str_, Node{Str{std::move(value)}}});
      pos = i;
      continue;
    }

    // Try an integer: optional '-' then one or more digits, greedily.
    if (std::isdigit((unsigned char)c) ||
        (c == '-' && pos + 1 < end && std::isdigit((unsigned char)text[pos + 1]))) {
      std::size_t i = pos;
      bool neg = false;
      if (text[i] == '-') {
        neg = true;
        ++i;
      }
      std::size_t digits_start = i;
      while (i < end && std::isdigit((unsigned char)text[i]))
        ++i;
      if (i > digits_start) {
        std::string digits = text.substr(digits_start, i - digits_start);
        std::int64_t value = std::stoll(digits);
        if (neg)
          value = -value;
        tokens.push_back({TokKind::Int, Node{value}});
        pos = i;
        continue;
      }
      // '-' with no digits after it: fall through to name matching below.
    }

    // Bare name: run up to the next boundary character.
    std::size_t i = pos;
    while (i < end && !is_boundary(text[i]))
      ++i;
    if (i == pos) {
      throw FormatError(std::string("unrecognised character '") + c +
                         "' at offset " + std::to_string(pos));
    }
    tokens.push_back({TokKind::Name_, Node{Name{text.substr(pos, i - pos)}}});
    pos = i;
  }

  return tokens;
}

} // namespace

List parse_forest(const std::string &text) {
  std::vector<Token> tokens = tokenize(text);

  std::vector<List> stack;
  stack.emplace_back(); // top-level list

  for (const Token &tok : tokens) {
    switch (tok.kind) {
    case TokKind::LParen:
      stack.emplace_back();
      break;
    case TokKind::RParen: {
      if (stack.size() == 1)
        throw FormatError("unbalanced ')' in module body");
      List finished = std::move(stack.back());
      stack.pop_back();
      stack.back().emplace_back(std::move(finished));
      break;
    }
    default:
      stack.back().push_back(tok.node);
      break;
    }
  }

  if (stack.size() != 1)
    throw FormatError("unterminated '(' in module body");
  return std::move(stack.front());
}

namespace {
void quote_into(std::ostream &os, const std::string &s) {
  os << '\'';
  for (char c : s) {
    if (c == '\'')
      os << "''";
    else
      os << c;
  }
  os << '\'';
}
} // namespace

std::string render(const Node &node, int indent) {
  std::ostringstream os;
  std::string pad(static_cast<std::size_t>(indent) * 2, ' ');
  if (node.is_list()) {
    const List &l = node.list();
    if (l.empty()) {
      os << pad << "()";
      return os.str();
    }
    bool all_flat = l.size() <= 12;
    for (const Node &c : l)
      if (c.is_list())
        all_flat = false;
    if (all_flat) {
      os << pad << "(";
      for (std::size_t i = 0; i < l.size(); ++i) {
        if (i)
          os << ' ';
        if (l[i].is_str())
          quote_into(os, l[i].str());
        else if (l[i].is_name())
          os << l[i].name();
        else if (l[i].is_int())
          os << l[i].integer();
      }
      os << ")";
      return os.str();
    }
    os << pad << "(\n";
    for (std::size_t i = 0; i < l.size(); ++i) {
      os << render(l[i], indent + 1);
      if (i + 1 != l.size())
        os << "\n";
    }
    os << "\n" << pad << ")";
    return os.str();
  }
  if (node.is_str()) {
    os << pad;
    quote_into(os, node.str());
  } else if (node.is_name()) {
    os << pad << node.name();
  } else if (node.is_int()) {
    os << pad << node.integer();
  }
  return os.str();
}

} // namespace fxmod::sexpr
