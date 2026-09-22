// Minimal self-contained test helpers: no external test framework, so the
// build has no extra dependency beyond zlib. Each test is a small main()
// that returns 0 on success, 1 on a failed check, or 125 (CTest's
// conventional "skipped" code, wired up via SKIP_RETURN_CODE in
// tests/CMakeLists.txt) when its prerequisites aren't available on this
// machine (e.g. no real GCC-built fixture, no system gfortran).
#pragma once

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include <zlib.h>

#define FXMOD_CHECK(cond)                                                   \
  do {                                                                       \
    if (!(cond)) {                                                           \
      std::fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__,  \
                    #cond);                                                  \
      std::exit(1);                                                          \
    }                                                                        \
  } while (0)

#define FXMOD_CHECK_EQ(a, b)                                                \
  do {                                                                       \
    auto _a = (a);                                                          \
    auto _b = (b);                                                          \
    if (!(_a == _b)) {                                                      \
      std::fprintf(stderr, "%s:%d: check failed: %s == %s\n", __FILE__,      \
                    __LINE__, #a, #b);                                       \
      std::exit(1);                                                          \
    }                                                                        \
  } while (0)

constexpr int FXMOD_TEST_SKIPPED = 125;

inline void skip(const std::string &why) {
  std::fprintf(stderr, "skipping: %s\n", why.c_str());
  std::exit(FXMOD_TEST_SKIPPED);
}

inline std::vector<std::uint8_t> gzip_compress(const std::string &text) {
  z_stream zs{};
  if (deflateInit2(&zs, Z_DEFAULT_COMPRESSION, Z_DEFLATED, 15 + 16, 8,
                    Z_DEFAULT_STRATEGY) != Z_OK)
    skip("zlib deflateInit2 failed");

  zs.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(text.data()));
  zs.avail_in = static_cast<uInt>(text.size());

  std::vector<std::uint8_t> out;
  char buf[4096];
  int ret;
  do {
    zs.next_out = reinterpret_cast<Bytef *>(buf);
    zs.avail_out = sizeof buf;
    ret = deflate(&zs, Z_FINISH);
    out.insert(out.end(), buf, buf + (sizeof buf - zs.avail_out));
  } while (ret != Z_STREAM_END);
  deflateEnd(&zs);
  return out;
}

inline void write_file(const std::string &path,
                        const std::vector<std::uint8_t> &data) {
  std::ofstream f(path, std::ios::binary);
  f.write(reinterpret_cast<const char *>(data.data()),
          static_cast<std::streamsize>(data.size()));
}

inline void write_file(const std::string &path, const std::string &data) {
  std::ofstream f(path, std::ios::binary);
  f.write(data.data(), static_cast<std::streamsize>(data.size()));
}

// Finds the first of the given candidate paths that exists, or "".
inline std::string first_existing(const std::vector<std::string> &candidates) {
  for (const std::string &c : candidates) {
    std::ifstream f(c);
    if (f.good())
      return c;
  }
  return "";
}

// Locates the directory holding gfortran's own intrinsic module files
// (omp_lib_kinds.mod and friends) -- the real, GCC-built fixtures used by
// tests that need actual module content, not synthetic data. Honors
// FXMOD_GFORTRAN_FINCLUDE_DIR if set, otherwise searches common Homebrew
// install layouts. Returns "" if nothing is found, so callers can skip()
// rather than fail on a machine without a matching gfortran install.
inline std::string find_gfortran_finclude_dir() {
  if (const char *env = std::getenv("FXMOD_GFORTRAN_FINCLUDE_DIR")) {
    if (std::filesystem::exists(std::string(env) + "/omp_lib_kinds.mod"))
      return env;
  }
  namespace fs = std::filesystem;
  for (const char *root : {"/opt/homebrew/Cellar/gcc", "/usr/local/Cellar/gcc"}) {
    std::error_code ec;
    if (!fs::exists(root, ec))
      continue;
    for (auto it = fs::recursive_directory_iterator(
             root, fs::directory_options::skip_permission_denied, ec);
         it != fs::recursive_directory_iterator(); it.increment(ec)) {
      if (ec)
        break;
      if (it->is_directory(ec) && it->path().filename() == "finclude" &&
          fs::exists(it->path() / "omp_lib_kinds.mod"))
        return it->path().string();
    }
  }
  return "";
}

inline std::string which(const std::string &program) {
  std::string cmd = "command -v " + program + " 2>/dev/null";
  FILE *p = popen(cmd.c_str(), "r");
  if (!p)
    return "";
  char buf[4096] = {0};
  std::size_t n = std::fread(buf, 1, sizeof(buf) - 1, p);
  pclose(p);
  std::string out(buf, n);
  while (!out.empty() && (out.back() == '\n' || out.back() == '\r'))
    out.pop_back();
  return out;
}
