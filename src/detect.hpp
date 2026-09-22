// Sniffs which compiler family a module file belongs to, by magic bytes --
// cheap and unambiguous, since the three formats don't overlap at all
// (gzip magic vs. UTF-8 BOM + Flang's own magic vs. unknown).
#pragma once

#include <string>

#include <fxmod/error.hpp> // for Family

namespace fxmod {

Family detect_family(const std::string &path);

} // namespace fxmod
