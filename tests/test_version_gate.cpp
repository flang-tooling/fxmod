// Confirms the version gate accepts exactly the two schema versions this
// library has actually verified against real GCC source ("15" = GCC
// 12-14, "16" = GCC 15.x/16.1/16.2) and refuses everything else --
// including a plausible-looking future/dev version ("16s", matching what
// GCC's own in-development snapshots currently write) -- with a clear
// UnsupportedError rather than silently misparsing it.
#include <fxmod/module.hpp>

#include <filesystem>

#include "test_util.hpp"

namespace {

std::string make_module_bytes_path(const std::string &version,
                                    const std::string &tmp_dir) {
  // Minimal but well-formed body: an empty symbol pool and an empty
  // symtree, which is all read_module() itself needs to succeed (symbol
  // interpretation happens lazily, on first use).
  std::string text = "GFORTRAN module version '" + version +
                      "' created from test.f90\n() ()\n";
  std::vector<std::uint8_t> gz = gzip_compress(text);
  std::string path = tmp_dir + "/v" + version + ".mod";
  write_file(path, gz);
  return path;
}

} // namespace

int main() {
  std::filesystem::path tmp =
      std::filesystem::temp_directory_path() / "fxmod-version-gate-test";
  std::filesystem::create_directories(tmp);

  for (const std::string &version : {std::string("15"), std::string("16")}) {
    std::string path = make_module_bytes_path(version, tmp.string());
    fxmod::ModuleFile mf = fxmod::ModuleFile::open(path); // must not throw
    FXMOD_CHECK(mf.family() == fxmod::Family::Gfortran);
    FXMOD_CHECK_EQ(mf.version(), version);
  }

  for (const std::string &version :
       {std::string("14"), std::string("17"), std::string("16s")}) {
    std::string path = make_module_bytes_path(version, tmp.string());
    bool threw_unsupported = false;
    try {
      fxmod::ModuleFile::open(path);
    } catch (const fxmod::UnsupportedError &) {
      threw_unsupported = true;
    } catch (const std::exception &e) {
      std::fprintf(stderr,
                    "version '%s': expected UnsupportedError, got a "
                    "different exception: %s\n",
                    version.c_str(), e.what());
      return 1;
    }
    if (!threw_unsupported) {
      std::fprintf(stderr, "version '%s' was accepted but should not be\n",
                    version.c_str());
      return 1;
    }
  }

  std::puts("test_version_gate: OK");
  return 0;
}
