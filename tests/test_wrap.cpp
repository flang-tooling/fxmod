// End-to-end test of `fxmod-cli wrap`: synthesizes a Flang-format module,
// points wrap at a real `gfortran` invocation (--target gfortran), and
// checks the full pipeline -- conversion, caching on a second run, and
// that the produced file really is a valid native gfortran module our own
// reader accepts. Drives the actual built fxmod-cli binary as a
// subprocess, since wrap's logic lives in tools/ (application logic, not
// part of the library) rather than something unit-testable directly.
#include <fxmod/fxmod.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>

#include <sys/wait.h>

#include "test_util.hpp"

#ifndef FXMOD_CLI_EXE
#error "FXMOD_CLI_EXE must be defined by the build"
#endif

namespace fs = std::filesystem;

namespace {

int run(const std::string &cmd) {
  int status = std::system((cmd + " >/tmp/fxmod-wrap-test.out 2>&1").c_str());
  return WEXITSTATUS(status);
}

} // namespace

int main() {
  std::string gfortran = which("gfortran");
  if (gfortran.empty())
    skip("no gfortran binary on PATH");

  fs::path root = fs::temp_directory_path() / "fxmod-wrap-e2e-test";
  fs::path src_dir = root / "src";
  fs::path cache_dir = root / "cache";
  std::error_code ec;
  fs::remove_all(root, ec);
  fs::create_directories(src_dir, ec);

  // A synthetic but valid Flang-format module -- same construction
  // test_flang_module.cpp already verifies round-trips correctly.
  const std::string body =
      "module foo\n implicit none\n integer(4) :: x\nend module foo\n";
  char *bytes = nullptr;
  size_t len = 0;
  FXMOD_CHECK_EQ(fxmod_wrap_flang_header(body.data(), body.size(), &bytes,
                                            &len, nullptr),
                  FXMOD_OK);
  write_file((src_dir / "foo.mod").string(), std::string(bytes, len));
  fxmod_free_string(bytes);

  std::string cli = FXMOD_CLI_EXE;

  // First run: must convert (a Flang-format module, gfortran target).
  std::string cmd1 = cli + " wrap --target gfortran --cache " +
                      cache_dir.string() + " -v -- " + gfortran + " -I" +
                      src_dir.string() + " -fsyntax-only /dev/null";
  FXMOD_CHECK_EQ(run(cmd1), 0);
  FXMOD_CHECK(fs::exists(cache_dir / "foo.mod"));
  FXMOD_CHECK(fs::exists(cache_dir / "foo.f90"));

  // The produced file must be a real, valid native gfortran module --
  // check with our own reader, independently of fxmod-cli.
  {
    fxmod_module *m = nullptr;
    fxmod_error *err = nullptr;
    FXMOD_CHECK_EQ(
        fxmod_open((cache_dir / "foo.mod").string().c_str(), &m, &err),
        FXMOD_OK);
    FXMOD_CHECK_EQ(fxmod_module_family(m), FXMOD_FAMILY_GFORTRAN);
    fxmod_module_free(m);
  }

  // Second run: must hit the cache, not reconvert (mtime of foo.mod must
  // not change).
  auto mtime_before = fs::last_write_time(cache_dir / "foo.mod");
  FXMOD_CHECK_EQ(run(cmd1), 0);
  auto mtime_after = fs::last_write_time(cache_dir / "foo.mod");
  FXMOD_CHECK(mtime_before == mtime_after);

  // The rewritten command must search the cache first (-I<cache> before
  // the original -I<src_dir>), via --dry-run's printed command.
  std::string dry_out = (root / "dry.txt").string();
  std::string cmd2 = cli + " wrap --target gfortran --cache " +
                      cache_dir.string() + " -n -- " + gfortran + " -I" +
                      src_dir.string() + " -c x.f90 > " + dry_out;
  int cmd2_status = std::system(cmd2.c_str());
  FXMOD_CHECK_EQ(WEXITSTATUS(cmd2_status), 0);
  std::ifstream in(dry_out);
  std::string line;
  std::getline(in, line);
  FXMOD_CHECK(line.find("-I" + cache_dir.string()) == 0 ||
               line.find(" -I" + cache_dir.string()) != std::string::npos);
  FXMOD_CHECK(line.find("-I" + cache_dir.string()) <
               line.find("-I" + src_dir.string()));

  std::puts("test_wrap: OK");
  return 0;
}
