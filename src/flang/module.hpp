// Reads Flang module files. Unlike gfortran's, Flang's on-disk format is
// already plain Fortran free-form source behind a small checksummed header
// (see flang/lib/Semantics/mod-file.cpp and flang/docs/ModFiles.md) -- so
// there is no schema to decode here, only a header to verify. The body is
// handed back as-is; re-parsing Fortran source is out of scope for this
// library (that's what a real Fortran front end is for).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "../sexpr.hpp"

namespace fxmod::flang {

struct Module {
  std::string path;
  // 16 lowercase hex digits, as read from the header (and verified against
  // the body's own FNV-1a checksum by read_module()).
  std::string checksum;
  // Everything after the header line: !need$ dependency lines (if any) and
  // then the Fortran module source itself, exactly as Flang wrote it.
  std::string body;
};

// True if the file starts with the UTF-8 BOM + "!mod$ v1 sum:" magic Flang
// writes. Cheap sniff, does not verify the checksum.
bool looks_like_flang_module(const std::string &path);

// Reads the file, verifies the header magic and the checksum against the
// body, and returns the parsed header + body. Throws sexpr::FormatError on
// a bad magic or a checksum mismatch (a corrupted or hand-edited module).
Module read_module(const std::string &path);

// FNV-1a 64-bit over `body`, formatted as 16 lowercase hex digits -- matches
// ComputeCheckSum()/CheckSumString() in flang/lib/Semantics/mod-file.cpp.
std::string checksum_of(const std::string &body);

// Wraps `body_text` in the header Flang expects (BOM + magic + checksum +
// newline), producing bytes a real Flang-compatible reader would accept.
// Convenience only -- the library's primary output is emit_fortran_source()
// in fxmod::gfortran, which produces vendor-neutral source instead.
std::vector<std::uint8_t> wrap_module_bytes(const std::string &body_text);

} // namespace fxmod::flang
