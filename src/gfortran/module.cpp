#include "module.hpp"

#include <array>
#include <filesystem>
#include <fstream>
#include <regex>
#include <set>
#include <sstream>

#include <zlib.h>

namespace fxmod::gfortran {

namespace {

constexpr unsigned char kGzipMagic[2] = {0x1f, 0x8b};

std::vector<std::uint8_t> read_file(const std::string &path) {
  std::ifstream f(path, std::ios::binary);
  if (!f)
    throw sexpr::FormatError(path + ": could not open file");
  std::vector<std::uint8_t> data(
      (std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  return data;
}

// Gunzip the whole buffer. gfortran module files are small (a few hundred KB
// at most), so decompressing fully into memory rather than streaming is
// simplest and matches what the Python prototype does via gzip.decompress().
std::string gunzip(const std::string &path,
                    const std::vector<std::uint8_t> &raw) {
  z_stream zs{};
  // windowBits = 15 + 16 tells zlib to expect a gzip (not raw deflate)
  // stream and parse its header itself.
  if (inflateInit2(&zs, 15 + 16) != Z_OK)
    throw sexpr::FormatError(path + ": zlib initialisation failed");

  zs.next_in = const_cast<Bytef *>(raw.data());
  zs.avail_in = static_cast<uInt>(raw.size());

  std::string out;
  std::array<char, 64 * 1024> buf{};
  int ret;
  do {
    zs.next_out = reinterpret_cast<Bytef *>(buf.data());
    zs.avail_out = static_cast<uInt>(buf.size());
    ret = inflate(&zs, Z_NO_FLUSH);
    if (ret != Z_OK && ret != Z_STREAM_END) {
      inflateEnd(&zs);
      throw sexpr::FormatError(path + ": gzip decompression failed (zlib error " +
                                std::to_string(ret) + ")");
    }
    out.append(buf.data(), buf.size() - zs.avail_out);
  } while (ret != Z_STREAM_END);

  inflateEnd(&zs);
  return out;
}

} // namespace

std::string Module::name() const {
  return std::filesystem::path(path).stem().string();
}

bool is_supported_version(const std::string &version) {
  static const std::set<std::string> kSupported = {"15", "16"};
  return kSupported.count(version) != 0;
}

bool looks_like_gfortran_module(const std::string &path) {
  std::ifstream f(path, std::ios::binary);
  if (!f)
    return false;
  unsigned char head[2] = {0, 0};
  f.read(reinterpret_cast<char *>(head), 2);
  return f.gcount() == 2 && head[0] == kGzipMagic[0] && head[1] == kGzipMagic[1];
}

Module read_module(const std::string &path) {
  std::vector<std::uint8_t> raw = read_file(path);
  if (raw.size() < 2 || raw[0] != kGzipMagic[0] || raw[1] != kGzipMagic[1])
    throw sexpr::FormatError(path + ": not a gzip-compressed gfortran module");

  std::string text = gunzip(path, raw);

  std::size_t nl = text.find('\n');
  if (nl == std::string::npos)
    throw sexpr::FormatError(path + ": module has no header line");
  std::string header = text.substr(0, nl);
  std::string body = text.substr(nl + 1);

  // The version string is quoted but not restricted to digits: GCC's own
  // development snapshots write suffixed versions like "16s". Capture
  // whatever is inside the quotes and let is_supported_version() decide
  // whether to accept it, rather than failing to parse the header at all.
  static const std::regex kHeaderRe(
      R"(GFORTRAN module version '([^']+)' created from (.*))");
  std::smatch m;
  if (!std::regex_match(header, m, kHeaderRe))
    throw sexpr::FormatError(path + ": unexpected header line '" + header + "'");

  std::string version = m[1].str();
  if (!is_supported_version(version)) {
    throw UnsupportedError(
        path + ": gfortran module version '" + version +
        "' is not supported (known: \"15\" = GCC 12-14, \"16\" = GCC "
        "15.x/16.1/16.2). The format changes between releases; extending "
        "support means checking gcc/fortran/module.cc's mio_symbol() for "
        "that version.");
  }

  Module mod;
  mod.path = path;
  mod.version = version;
  mod.created_from = m[2].str();
  mod.forest = sexpr::parse_forest(body);
  return mod;
}

} // namespace fxmod::gfortran
