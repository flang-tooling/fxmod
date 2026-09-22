// Regression test for a real bug found via a real-world module: OpenMPI's
// mpi_f08.mod has a procedure with a dummy argument typed as something
// TypeSpec::to_fortran() doesn't have a case for (observed: base "VOID",
// from a BIND(C) void-pointer-ish parameter). That call is made directly
// inside emit_interface(), outside its own try/catch, and throws the
// *sibling* class sexpr::FormatError -- which analyse()'s PROCEDURE branch
// didn't catch (it only caught the narrower UnsupportedError), so the
// exception escaped uncaught all the way past emit_fortran_source(),
// aborting the whole emission even under strict=false, whose entire point
// is to never let one bad symbol take the rest down.
//
// Reproduces the same shape synthetically (a SUBROUTINE with one dummy
// argument of typespec base "VOID") rather than embedding a chunk of
// OpenMPI's own module file, so this test needs no external fixture and no
// real gfortran/MPI install.
#include <fxmod/module.hpp>

#include <filesystem>

#include "test_util.hpp"

int main() {
  std::filesystem::path tmp =
      std::filesystem::temp_directory_path() / "fxmod-best-effort-test";
  std::filesystem::create_directories(tmp);

  // A minimal but well-formed gfortran v16 module: one PROCEDURE (FOO,
  // flavor PROCEDURE, SUBROUTINE attribute -- no FUNCTION, so no return
  // type is needed) whose sole formal argument (symbol 2, X) has typespec
  // (VOID 0) -- a base TypeSpec::to_fortran() has no case for.
  std::string text =
      "GFORTRAN module version '16' created from test.f90\n"
      "(\n"
      "1 'FOO' '' '' 0 ( (PROCEDURE SUBROUTINE) () () () () (2) () )\n"
      "2 'X' '' '' 0 ( () () (VOID 0) () () () () )\n"
      ")\n"
      "(\n"
      "'FOO' 0 1\n"
      ")\n";
  std::string path = (tmp / "badtype.mod").string();
  write_file(path, gzip_compress(text));

  fxmod::ModuleFile mf = fxmod::ModuleFile::open(path); // must not throw

  // strict = false: must return, not throw, and must actually report the
  // problem rather than silently dropping FOO.
  fxmod::EmitResult r = mf.emit_fortran_source(/*strict=*/false);
  FXMOD_CHECK(r.problems.size() == 1);
  FXMOD_CHECK(r.problems[0].find("FOO") != std::string::npos);
  FXMOD_CHECK(r.problems[0].find("VOID") != std::string::npos);
  // FOO must not appear in the emitted source as a (wrongly) successful
  // declaration -- it was skipped, not guessed at.
  FXMOD_CHECK(r.source.find("FOO") == std::string::npos);

  // strict = true: must throw fxmod::UnsupportedError (the summarized
  // "N untranslatable symbol(s)" error), not let the raw sexpr::FormatError
  // escape uncaught.
  bool threw_unsupported = false;
  try {
    mf.emit_fortran_source(/*strict=*/true);
  } catch (const fxmod::UnsupportedError &e) {
    threw_unsupported = true;
    FXMOD_CHECK(std::string(e.what()).find("untranslatable") !=
                std::string::npos);
  } catch (const std::exception &e) {
    std::fprintf(stderr,
                  "expected fxmod::UnsupportedError, got a different "
                  "exception: %s\n",
                  e.what());
    return 1;
  }
  FXMOD_CHECK(threw_unsupported);

  std::puts("test_emit_best_effort: OK");
  return 0;
}
