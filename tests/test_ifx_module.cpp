// Verifies the ifx reader against real ifx-produced .mod files
// (tests/fixtures/ifx/), cross-checked during development against the
// vendor's own module-dump tool output for mymod.mod (see
// tests/fixtures/ifx/mymod.mod_dmp) for the header/directory/envelope
// and scalar constant values, and against real, differenced .mod files
// for procedure/derived-type/dummy-argument/array structure. Also
// confirms the reader refuses non-ifx files, and (mymod.mod) that
// emit_fortran_source() now succeeds for a module fully within the
// decoded subset -- see src/ifx/fortran_emitter.hpp for exactly what
// that subset is and test_gfortran_emit.cpp-style real-compiler
// round-trips for a stronger check than a string comparison.
#include <fxmod/module.hpp>

#include <filesystem>

#include "test_util.hpp"

#ifndef FXMOD_FIXTURES_DIR
#error "FXMOD_FIXTURES_DIR must be defined by the build"
#endif

int main() {
  std::string dir = std::string(FXMOD_FIXTURES_DIR) + "/ifx";

  // mymod.mod: module mymod with two PARAMETERs (answer, pi_approx) and
  // one function (add_answer) taking one dummy argument. The vendor dump
  // tool confirms 5 entities: the synthetic file_name entry, the two
  // parameters, the function, and its dummy argument -- see mymod.mod_dmp.
  {
    fxmod::ModuleFile mf = fxmod::ModuleFile::open(dir + "/mymod.mod");
    FXMOD_CHECK(mf.family() == fxmod::Family::Ifx);
    FXMOD_CHECK_EQ(mf.version(), std::string("13.1"));
    FXMOD_CHECK_EQ(mf.name(), std::string("mymod"));

    auto entities = mf.ifx_entities();
    FXMOD_CHECK_EQ(entities.size(), 5u);

    auto find = [&](const std::string &name) {
      for (auto &e : entities)
        if (e.name == name)
          return e;
      std::fprintf(stderr, "entity '%s' not found\n", name.c_str());
      std::exit(1);
    };

    // decl_rec_kind: confirmed stable across both fixtures --
    // 10 = file_name, 13 = simple_scalar, 28 = simple_scalar_arg,
    // 37 = func_ret_simple_scalar.
    FXMOD_CHECK_EQ(find("fn#fn").decl_rec_kind, 10u);

    auto answer = find("ANSWER");
    FXMOD_CHECK_EQ(answer.decl_rec_kind, 13u);
    FXMOD_CHECK_EQ(answer.sym_id, 1u);
    FXMOD_CHECK_EQ(static_cast<int>(answer.klass), 1); // LOCAL
    FXMOD_CHECK(answer.value.has_value());
    FXMOD_CHECK_EQ(*answer.value, std::string("42"));

    auto pi = find("PI_APPROX");
    FXMOD_CHECK_EQ(pi.decl_rec_kind, 13u);
    FXMOD_CHECK_EQ(pi.sym_id, 2u);
    FXMOD_CHECK(pi.value.has_value());
    // REAL4 bit pattern for the source's 3.14159 literal, rendered back
    // -- not exactly "3.14159" (float32 can't represent it exactly).
    FXMOD_CHECK(pi.value->rfind("3.14159", 0) == 0);

    auto fn = find("ADD_ANSWER");
    FXMOD_CHECK_EQ(fn.decl_rec_kind, 37u);
    FXMOD_CHECK_EQ(fn.sym_id, 3u);

    auto arg = find("ADD_ANSWER%X");
    FXMOD_CHECK_EQ(arg.decl_rec_kind, 28u);
    FXMOD_CHECK_EQ(arg.sym_id, 4u);

    // mymod.mod is fully within the decoded subset (two scalar
    // PARAMETERs, a function returning a scalar with one scalar dummy
    // argument) -- strict emission must succeed, not refuse.
    fxmod::EmitResult r = mf.emit_fortran_source(/*strict=*/true);
    FXMOD_CHECK(r.problems.empty());
    FXMOD_CHECK(r.source.find("parameter :: ANSWER = 42") != std::string::npos);
    FXMOD_CHECK(r.source.find("function ADD_ANSWER(X)") != std::string::npos);
  }

  // testmod.mod: module testmod with one pure function (square) and its
  // dummy argument -- independent second sample, same decl_rec_kind
  // values for the entity kinds it shares with mymod.mod.
  {
    fxmod::ModuleFile mf = fxmod::ModuleFile::open(dir + "/testmod.mod");
    FXMOD_CHECK(mf.family() == fxmod::Family::Ifx);
    auto entities = mf.ifx_entities();
    FXMOD_CHECK_EQ(entities.size(), 3u);
    for (const auto &e : entities) {
      if (e.name == "fn#fn")
        FXMOD_CHECK_EQ(e.decl_rec_kind, 10u);
      else if (e.name == "SQUARE")
        FXMOD_CHECK_EQ(e.decl_rec_kind, 37u);
      else if (e.name == "SQUARE%X")
        FXMOD_CHECK_EQ(e.decl_rec_kind, 28u);
      else {
        std::fprintf(stderr, "unexpected entity name '%s'\n", e.name.c_str());
        return 1;
      }
    }
  }

  // constructs/t1-t3.mod: real ifx 2026.1.1 samples, one construct each,
  // specifically to check parse_scalar_constant() -- a plain (non-
  // PARAMETER) variable must show no value at all, a CHARACTER PARAMETER
  // must recover its exact string (past the confirmed 1-byte encoding
  // tag), and a LOGICAL PARAMETER must render as a Fortran boolean
  // literal, not a raw integer.
  {
    std::string cdir = dir + "/constructs";

    fxmod::ModuleFile t1 = fxmod::ModuleFile::open(cdir + "/t1.mod");
    auto t1e = t1.ifx_entities();
    for (const auto &e : t1e)
      if (e.name == "PLAIN_VAR")
        FXMOD_CHECK(!e.value.has_value());

    fxmod::ModuleFile t2 = fxmod::ModuleFile::open(cdir + "/t2.mod");
    auto t2e = t2.ifx_entities();
    for (const auto &e : t2e)
      if (e.name == "GREET") {
        FXMOD_CHECK(e.value.has_value());
        FXMOD_CHECK_EQ(*e.value, std::string("'hello'"));
      }

    fxmod::ModuleFile t3 = fxmod::ModuleFile::open(cdir + "/t3.mod");
    auto t3e = t3.ifx_entities();
    for (const auto &e : t3e)
      if (e.name == "FLAG") {
        FXMOD_CHECK(e.value.has_value());
        FXMOD_CHECK_EQ(*e.value, std::string(".true."));
      }

    // t8.mod: COMPLEX(4), COMPLEX(8), and REAL(16) PARAMETERs -- checks
    // the f128_to_double() narrowing and the two-float/two-double
    // COMPLEX layouts, none of which mymod.mod's simpler INTEGER/REAL4
    // pair exercises.
    fxmod::ModuleFile t8 = fxmod::ModuleFile::open(cdir + "/t8.mod");
    auto t8e = t8.ifx_entities();
    for (const auto &e : t8e) {
      if (e.name == "C8") {
        FXMOD_CHECK(e.value.has_value());
        FXMOD_CHECK_EQ(*e.value, std::string("(1.5,2.5)"));
      } else if (e.name == "C16") {
        FXMOD_CHECK(e.value.has_value());
        FXMOD_CHECK_EQ(*e.value, std::string("(3.5,4.5)"));
      } else if (e.name == "R16") {
        FXMOD_CHECK(e.value.has_value());
        FXMOD_CHECK_EQ(*e.value, std::string("1.5"));
      }
    }

    // t4.mod: `integer function add2(a, b)` with two INTENT(IN) INTEGER
    // dummy args -- checks parse_procedure()'s FuncRetSimpleScalar path
    // and parse_dummy_arg().
    fxmod::ModuleFile t4 = fxmod::ModuleFile::open(cdir + "/t4.mod");
    auto t4e = t4.ifx_entities();
    for (const auto &e : t4e) {
      if (e.name == "ADD2") {
        FXMOD_CHECK(e.is_function.has_value());
        FXMOD_CHECK(*e.is_function);
        FXMOD_CHECK(e.return_type.has_value());
        FXMOD_CHECK_EQ(*e.return_type, std::string("integer(4)"));
        FXMOD_CHECK_EQ(e.argument_names.size(), 2u);
        FXMOD_CHECK_EQ(e.argument_names[0], std::string("A"));
        FXMOD_CHECK_EQ(e.argument_names[1], std::string("B"));
      } else if (e.name == "ADD2%A" || e.name == "ADD2%B") {
        FXMOD_CHECK(e.dummy_arg_type.has_value());
        FXMOD_CHECK_EQ(*e.dummy_arg_type, std::string("integer(4)"));
      }
    }

    // t5.mod: `subroutine bump(y)` -- checks parse_procedure()'s Subr
    // path (no return type).
    fxmod::ModuleFile t5 = fxmod::ModuleFile::open(cdir + "/t5.mod");
    auto t5e = t5.ifx_entities();
    for (const auto &e : t5e)
      if (e.name == "BUMP") {
        FXMOD_CHECK(e.is_function.has_value());
        FXMOD_CHECK(!*e.is_function);
        FXMOD_CHECK(!e.return_type.has_value());
        FXMOD_CHECK_EQ(e.argument_names.size(), 1u);
        FXMOD_CHECK_EQ(e.argument_names[0], std::string("Y"));
      }

    // t6.mod: `type :: point; real :: x, y; end type` + `type(point) ::
    // p` -- checks parse_derived_type(), parse_simple_scalar_field(),
    // and parse_struct_variable().
    fxmod::ModuleFile t6 = fxmod::ModuleFile::open(cdir + "/t6.mod");
    auto t6e = t6.ifx_entities();
    for (const auto &e : t6e) {
      if (e.name == "POINT") {
        FXMOD_CHECK_EQ(e.field_names.size(), 2u);
        FXMOD_CHECK_EQ(e.field_names[0], std::string("X"));
        FXMOD_CHECK_EQ(e.field_names[1], std::string("Y"));
      } else if (e.name == "POINT%X" || e.name == "POINT%Y") {
        FXMOD_CHECK(e.field_type.has_value());
        FXMOD_CHECK_EQ(*e.field_type, std::string("real(4)"));
      } else if (e.name == "P") {
        FXMOD_CHECK(e.struct_type_name.has_value());
        FXMOD_CHECK_EQ(*e.struct_type_name, std::string("POINT"));
      }
    }

    // t7.mod: `integer :: arr(10)` -- checks parse_simple_array()'s
    // header fields and parse_array_bounds()'s default-lower-bound path.
    fxmod::ModuleFile t7 = fxmod::ModuleFile::open(cdir + "/t7.mod");
    auto t7e = t7.ifx_entities();
    for (const auto &e : t7e)
      if (e.name == "ARR") {
        FXMOD_CHECK(e.array_element_type.has_value());
        FXMOD_CHECK_EQ(*e.array_element_type, std::string("integer(4)"));
        FXMOD_CHECK(e.array_size.has_value());
        FXMOD_CHECK_EQ(*e.array_size, 10u);
        FXMOD_CHECK(e.array_ndimen.has_value());
        FXMOD_CHECK_EQ(static_cast<int>(*e.array_ndimen), 1);
        FXMOD_CHECK(e.array_bounds.has_value());
        FXMOD_CHECK_EQ(*e.array_bounds, std::string("1:10"));
      }

    // t9.mod: `integer :: a5(5)` -- an independent default-lower-bound
    // sample with a different extent, to rule out "10" being hardcoded
    // anywhere.
    fxmod::ModuleFile t9 = fxmod::ModuleFile::open(cdir + "/t9.mod");
    auto t9e = t9.ifx_entities();
    for (const auto &e : t9e)
      if (e.name == "A5") {
        FXMOD_CHECK(e.array_bounds.has_value());
        FXMOD_CHECK_EQ(*e.array_bounds, std::string("1:5"));
      }

    // t10.mod: `integer :: a28(2:8)` -- a non-default lower bound, whose
    // "dimen" element uses "COLON(lower, upper)" (a 2-child range node)
    // instead of the plain extent leaf t7/t9 above use.
    fxmod::ModuleFile t10 = fxmod::ModuleFile::open(cdir + "/t10.mod");
    auto t10e = t10.ifx_entities();
    for (const auto &e : t10e)
      if (e.name == "A28") {
        FXMOD_CHECK(e.array_bounds.has_value());
        FXMOD_CHECK_EQ(*e.array_bounds, std::string("2:8"));
      }

    // t11.mod: `integer :: a2d(3,4)` -- ndimen == 2, both dimensions
    // default lower bound 1; "dimen" is a chain of one element per
    // dimension (confirmed against grid(3,4)/grid2(5,7) probe samples).
    fxmod::ModuleFile t11 = fxmod::ModuleFile::open(cdir + "/t11.mod");
    auto t11e = t11.ifx_entities();
    for (const auto &e : t11e)
      if (e.name == "A2D") {
        FXMOD_CHECK_EQ(static_cast<int>(*e.array_ndimen), 2);
        FXMOD_CHECK(e.array_bounds.has_value());
        FXMOD_CHECK_EQ(*e.array_bounds, std::string("1:3, 1:4"));
      }
  }

  // A file with none of the three known formats' magic bytes must be
  // refused outright, not misclassified as a known-but-unimplemented
  // format.
  {
    std::filesystem::path tmp =
        std::filesystem::temp_directory_path() / "fxmod-ifx-test";
    std::filesystem::create_directories(tmp);
    std::string path = (tmp / "fake.mod").string();
    write_file(path, std::string("not a real module file of any kind"));

    bool threw_error = false;
    try {
      fxmod::ModuleFile::open(path);
    } catch (const fxmod::UnsupportedError &) {
      std::fprintf(stderr, "unexpectedly classified as a known-but-"
                            "unimplemented format\n");
      return 1;
    } catch (const fxmod::Error &) {
      threw_error = true;
    }
    FXMOD_CHECK(threw_error);
  }

  std::puts("test_ifx_module: OK");
  return 0;
}
