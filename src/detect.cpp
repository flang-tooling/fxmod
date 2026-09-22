#include "detect.hpp"

#include "flang/module.hpp"
#include "gfortran/module.hpp"
#include "ifx/module.hpp"

namespace fxmod {

Family detect_family(const std::string &path) {
  if (gfortran::looks_like_gfortran_module(path))
    return Family::Gfortran;
  if (flang::looks_like_flang_module(path))
    return Family::Flang;
  if (ifx::looks_like_ifx_module(path))
    return Family::Ifx;
  return Family::Unknown;
}

const char *family_name(Family family) {
  switch (family) {
  case Family::Gfortran:
    return "gfortran";
  case Family::Flang:
    return "flang";
  case Family::Ifx:
    return "ifx";
  case Family::Unknown:
    break;
  }
  return "unknown";
}

} // namespace fxmod
