#include "module.hpp"

#include <cstdio>
#include <fstream>
#include <iterator>

namespace fxmod::flang {

namespace {

const std::string kBom = "\xef\xbb\xbf";
const std::string kMagic = "!mod$ v1 sum:";
constexpr std::size_t kSumLen = 16;

// FNV-1a 64-bit offset basis / prime, matching ComputeCheckSum() in
// flang/lib/Semantics/mod-file.cpp.
constexpr std::uint64_t kFnvOffset = 0xcbf29ce484222325ull;
constexpr std::uint64_t kFnvPrime = 0x100000001b3ull;

std::string read_file(const std::string &path) {
  std::ifstream f(path, std::ios::binary);
  if (!f)
    throw sexpr::FormatError(path + ": could not open file");
  return std::string((std::istreambuf_iterator<char>(f)),
                      std::istreambuf_iterator<char>());
}

} // namespace

std::string checksum_of(const std::string &body) {
  std::uint64_t h = kFnvOffset;
  for (unsigned char byte : body) {
    h ^= byte;
    h *= kFnvPrime;
  }
  char buf[17];
  std::snprintf(buf, sizeof buf, "%016llx", static_cast<unsigned long long>(h));
  return std::string(buf, 16);
}

bool looks_like_flang_module(const std::string &path) {
  std::ifstream f(path, std::ios::binary);
  if (!f)
    return false;
  std::string head(kBom.size() + kMagic.size(), '\0');
  f.read(head.data(), static_cast<std::streamsize>(head.size()));
  if (static_cast<std::size_t>(f.gcount()) != head.size())
    return false;
  return head == kBom + kMagic;
}

Module read_module(const std::string &path) {
  std::string data = read_file(path);
  std::string prefix = kBom + kMagic;
  if (data.size() < prefix.size() || data.compare(0, prefix.size(), prefix) != 0)
    throw sexpr::FormatError(path + ": not a Flang module (bad header magic)");

  std::size_t sum_start = prefix.size();
  if (data.size() < sum_start + kSumLen + 1)
    throw sexpr::FormatError(path + ": truncated Flang module header");
  std::string checksum = data.substr(sum_start, kSumLen);

  std::size_t body_start = sum_start + kSumLen;
  // The header is terminated by a newline before the body begins.
  if (data[body_start] != '\n')
    throw sexpr::FormatError(path + ": malformed Flang module header (no "
                              "terminator after checksum)");
  ++body_start;

  std::string body = data.substr(body_start);
  std::string actual = checksum_of(body);
  if (actual != checksum) {
    throw sexpr::FormatError(path + ": checksum mismatch (header says " +
                              checksum + ", body hashes to " + actual +
                              ") -- module file is corrupted or was "
                              "hand-edited without recompiling");
  }

  Module mod;
  mod.path = path;
  mod.checksum = std::move(checksum);
  mod.body = std::move(body);
  return mod;
}

std::vector<std::uint8_t> wrap_module_bytes(const std::string &body_text) {
  std::string sum = checksum_of(body_text);
  std::string full = kBom + kMagic + sum + "\n" + body_text;
  return std::vector<std::uint8_t>(full.begin(), full.end());
}

} // namespace fxmod::flang
