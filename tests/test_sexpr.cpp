#include "../src/sexpr.hpp"
#include "test_util.hpp"

using namespace fxmod::sexpr;

int main() {
  // Flat list of mixed atom kinds.
  {
    List f = parse_forest("(FOO 'bar' 42 -7)");
    FXMOD_CHECK_EQ(f.size(), 1u);
    FXMOD_CHECK(f[0].is_list());
    const List &inner = f[0].list();
    FXMOD_CHECK_EQ(inner.size(), 4u);
    FXMOD_CHECK(inner[0].is_name());
    FXMOD_CHECK_EQ(inner[0].name(), std::string("FOO"));
    FXMOD_CHECK(inner[1].is_str());
    FXMOD_CHECK_EQ(inner[1].str(), std::string("bar"));
    FXMOD_CHECK(inner[2].is_int());
    FXMOD_CHECK_EQ(inner[2].integer(), 42);
    FXMOD_CHECK(inner[3].is_int());
    FXMOD_CHECK_EQ(inner[3].integer(), -7);
  }

  // '' is an escaped single quote inside a string atom.
  {
    List f = parse_forest("('it''s here')");
    const std::string &s = f[0].list()[0].str();
    FXMOD_CHECK_EQ(s, std::string("it's here"));
  }

  // Nesting.
  {
    List f = parse_forest("(A (B (C) D) E)");
    const List &top = f[0].list();
    FXMOD_CHECK_EQ(top.size(), 3u);
    FXMOD_CHECK(top[1].is_list());
    FXMOD_CHECK_EQ(top[1].list().size(), 3u);
    FXMOD_CHECK(top[1].list()[1].is_list());
    FXMOD_CHECK_EQ(top[1].list()[1].list().size(), 1u);
    FXMOD_CHECK(top[1].list()[1].list()[0].is_name());
    FXMOD_CHECK_EQ(top[1].list()[1].list()[0].name(), std::string("C"));
  }

  // Multiple top-level items (as in a module body: several sections).
  {
    List f = parse_forest("(A) (B)");
    FXMOD_CHECK_EQ(f.size(), 2u);
  }

  // Unbalanced parens must throw, never silently truncate.
  {
    bool threw = false;
    try {
      parse_forest("(A (B)");
    } catch (const FormatError &) {
      threw = true;
    }
    FXMOD_CHECK(threw);
  }
  {
    bool threw = false;
    try {
      parse_forest("(A))");
    } catch (const FormatError &) {
      threw = true;
    }
    FXMOD_CHECK(threw);
  }

  std::puts("test_sexpr: OK");
  return 0;
}
