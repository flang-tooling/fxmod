#include "module.hpp"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>

namespace fxmod::ifx {

namespace {

constexpr std::size_t kHeaderSize = 160;
constexpr std::size_t kMagicOffset = 12;
const char kMagic[9] = "k820309"; // 7 chars + NUL = 8 bytes on disk

std::uint16_t u16(const std::vector<std::uint8_t> &d, std::size_t off) {
  return static_cast<std::uint16_t>(d[off] | (d[off + 1] << 8));
}
std::uint32_t u32(const std::vector<std::uint8_t> &d, std::size_t off) {
  return static_cast<std::uint32_t>(d[off]) |
         (static_cast<std::uint32_t>(d[off + 1]) << 8) |
         (static_cast<std::uint32_t>(d[off + 2]) << 16) |
         (static_cast<std::uint32_t>(d[off + 3]) << 24);
}
std::uint64_t u64(const std::vector<std::uint8_t> &d, std::size_t off) {
  std::uint64_t lo = u32(d, off);
  std::uint64_t hi = u32(d, off + 4);
  return lo | (hi << 32);
}

std::int64_t s64(const std::vector<std::uint8_t> &d, std::size_t off) {
  return static_cast<std::int64_t>(u64(d, off));
}

float f32(const std::vector<std::uint8_t> &d, std::size_t off) {
  std::uint32_t bits = u32(d, off);
  float v;
  std::memcpy(&v, &bits, sizeof v);
  return v;
}

double f64(const std::vector<std::uint8_t> &d, std::size_t off) {
  std::uint64_t bits = u64(d, off);
  double v;
  std::memcpy(&v, &bits, sizeof v);
  return v;
}

// Narrows a little-endian IEEE-754 binary128 value at `off` to double.
// Only normal finite values are handled -- refuses (nullopt) for zero,
// subnormal, infinity, or NaN, since those need bit-twiddling this
// hasn't been checked against any real sample. The narrowing to double
// loses most of binary128's 112-bit mantissa (kept: the top 52 bits) --
// this renders a usable Fortran literal, not a bit-exact round trip of
// the original quad value.
std::optional<double> f128_to_double(const std::vector<std::uint8_t> &d,
                                      std::size_t off) {
  std::uint64_t lo = u64(d, off);     // low 64 bits: bottom of the mantissa
  std::uint64_t hi = u64(d, off + 8); // bit 127 = sign, 112-126 = exponent,
                                       // 64-111 = top 48 bits of mantissa
  bool sign = (hi >> 63) != 0;
  std::uint32_t exp = static_cast<std::uint32_t>((hi >> 48) & 0x7FFF);
  if (exp == 0 || exp == 0x7FFF)
    return std::nullopt;
  int unbiased_exp = static_cast<int>(exp) - 16383;
  int double_exp = unbiased_exp + 1023;
  if (double_exp <= 0 || double_exp >= 0x7FF)
    return std::nullopt;

  std::uint64_t mant_hi = hi & 0xFFFFFFFFFFFFull; // top 48 bits of mantissa
  std::uint64_t mantissa_top52 = (mant_hi << 4) | (lo >> 60);
  std::uint64_t double_mantissa = mantissa_top52 & 0xFFFFFFFFFFFFFull;

  std::uint64_t bits = (static_cast<std::uint64_t>(sign) << 63) |
                        (static_cast<std::uint64_t>(double_exp) << 52) |
                        double_mantissa;
  double v;
  std::memcpy(&v, &bits, sizeof v);
  return v;
}

// Formats a floating-point value as a valid Fortran real literal -- it
// must contain a '.' or exponent, or Fortran would read it as an integer.
std::string format_real_literal(double v) {
  char buf[64];
  std::snprintf(buf, sizeof buf, "%.9g", v);
  std::string s(buf);
  if (s.find('.') == std::string::npos && s.find('e') == std::string::npos &&
      s.find('E') == std::string::npos)
    s += ".0";
  return s;
}

// Renders an INTEGER value as Fortran source text. The minimum value
// representable at a given kind (e.g. -2147483648 for INTEGER(4)) can't
// be written as a plain decimal literal in Fortran: the digit sequence
// "2147483648" is parsed as a positive literal *before* the minus sign
// is applied, and that exceeds the kind's own positive range ("Integer
// too big for its kind", confirmed against real gfortran) even though
// the actual negated value fits exactly. `-huge(0_<kind>)-1` sidesteps
// this -- a compile-time-constant expression, not a single out-of-range
// literal token, valid in a PARAMETER initializer.
std::string format_integer_literal(std::int64_t v, ScalarType type) {
  int kind = 4;
  std::int64_t min_value = static_cast<std::int64_t>(
      std::numeric_limits<std::int32_t>::min());
  switch (type) {
  case ScalarType::Int1:
    kind = 1;
    min_value = std::numeric_limits<std::int8_t>::min();
    break;
  case ScalarType::Int2:
    kind = 2;
    min_value = std::numeric_limits<std::int16_t>::min();
    break;
  case ScalarType::Int4:
    kind = 4;
    min_value = std::numeric_limits<std::int32_t>::min();
    break;
  case ScalarType::Int8:
    kind = 8;
    min_value = std::numeric_limits<std::int64_t>::min();
    break;
  default:
    break;
  }
  if (v == min_value)
    return "-huge(0_" + std::to_string(kind) + ")-1";
  return std::to_string(v);
}

// Where read_value_block() (below) found the two alternative forms a
// CONTOK's payload takes: `value_pos` (an 8-byte numeric slot -- used by
// every non-CHARACTER type) and `text_pos`/`text_len` (an ASCII string,
// exactly `text_len` bytes starting at `text_pos`). For every numeric
// type sampled, `text_len` bytes of decimal text sit at `text_pos` too,
// but redundantly with `value_pos` and not otherwise used. For
// CHARACTER specifically (confirmed 2026-09-17), `value_pos`'s 8 bytes
// are unused/zero and the STRING content lives at `text_pos` instead --
// `text_len` there is the string's declared length plus 1 (a leading
// tag byte to skip, byte value not otherwise understood -- same "+1"
// convention already confirmed for a scalar CHARACTER constant's own
// connode, see parse_scalar_constant()'s Char case).
struct ContokValueLocation {
  std::size_t value_pos = 0;
  std::size_t text_pos = 0;
  std::uint32_t text_len = 0;
};

// Reads the shared "value block" that both a PARAMETER array's CONTOK
// node and a scalar field/variable default-value initializer carry: an
// 8-byte field carrying the value's own ScalarType at a fixed sub-
// position (redundant with what the caller already knows independently
// -- the array/field's own declared type -- so not separately modeled
// or checked), 8 reserved zero bytes, a `text_length` u32, a zero-
// padding u32, the 8-byte value itself (int64 for INTEGER/LOGICAL,
// float32 in the low 4 bytes for REAL4, float64 for REAL8 -- confirmed
// 2026-09-17 against real REAL4/REAL8/LOGICAL PARAMETER array samples,
// same positions as the already-confirmed INTEGER case; unused/zero for
// CHARACTER), 8 more reserved zero bytes, then exactly `text_length`
// bytes of ASCII text (redundant with the binary value for every
// numeric type, but the ONLY place a CHARACTER value's actual content
// lives -- see ContokValueLocation's comment). Advances `pos` past the
// whole block.
std::optional<ContokValueLocation>
read_value_block(const std::vector<std::uint8_t> &s, std::size_t &pos) {
  constexpr std::size_t kFixedTail = 8 + 8 + 4 + 4 + 8 + 8;
  if (pos + kFixedTail > s.size())
    return std::nullopt;
  pos += 8; // the type-carrying 8-byte field
  pos += 8; // reserved
  std::uint32_t text_len = u32(s, pos);
  pos += 8; // text_len (4B) + zero pad (4B)
  std::size_t value_pos = pos;
  pos += 16; // value (8B) + reserved (8B)
  if (pos + text_len > s.size())
    return std::nullopt;
  std::size_t text_pos = pos;
  pos += text_len;
  return ContokValueLocation{value_pos, text_pos, text_len};
}

// Renders a CONTOK's value (as located by read_value_block() above) as
// Fortran source text, per `type`. INTEGER, LOGICAL, REAL4, REAL8 and
// CHARACTER are confirmed (REAL16/COMPLEX aren't sampled) -- nullopt for
// anything else.
std::optional<std::string> render_contok_value(const std::vector<std::uint8_t> &s,
                                                const ContokValueLocation &loc,
                                                ScalarType type) {
  std::size_t value_pos = loc.value_pos;
  switch (type) {
  case ScalarType::Log1:
  case ScalarType::Log2:
  case ScalarType::Log4:
  case ScalarType::Log8:
    return s64(s, value_pos) != 0 ? ".true." : ".false.";
  case ScalarType::Int1:
  case ScalarType::Int2:
  case ScalarType::Int4:
  case ScalarType::Int8: {
    std::int64_t v = s64(s, value_pos);
    switch (type) {
    case ScalarType::Int1:
      v = static_cast<std::int8_t>(v);
      break;
    case ScalarType::Int2:
      v = static_cast<std::int16_t>(v);
      break;
    case ScalarType::Int4:
      v = static_cast<std::int32_t>(v);
      break;
    default:
      break;
    }
    return format_integer_literal(v, type);
  }
  case ScalarType::Real4:
    return format_real_literal(f32(s, value_pos));
  case ScalarType::Real8:
    return format_real_literal(f64(s, value_pos));
  case ScalarType::Char: {
    // `text_len` includes a 1-byte tag in front of the actual string
    // bytes (see ContokValueLocation's comment) -- refuse rather than
    // guess for text_len == 0 (would mean no tag byte at all).
    if (loc.text_len == 0)
      return std::nullopt;
    std::string raw(reinterpret_cast<const char *>(s.data() + loc.text_pos + 1),
                     loc.text_len - 1);
    std::string escaped;
    escaped.reserve(raw.size() + 2);
    for (char c : raw) {
      if (c == '\'')
        escaped += "''";
      else
        escaped += c;
    }
    return "'" + escaped + "'";
  }
  default:
    return std::nullopt;
  }
}

std::vector<std::uint8_t> read_file(const std::string &path) {
  std::ifstream f(path, std::ios::binary);
  if (!f)
    throw fxmod::Error(path + ": could not open file");
  return std::vector<std::uint8_t>((std::istreambuf_iterator<char>(f)),
                                    std::istreambuf_iterator<char>());
}

std::string fixed_cstr(const std::vector<std::uint8_t> &d, std::size_t off,
                        std::size_t max_len) {
  std::size_t len = 0;
  while (len < max_len && d[off + len] != 0)
    ++len;
  return std::string(reinterpret_cast<const char *>(d.data() + off), len);
}

std::size_t round_up8(std::size_t off) { return (off + 7) & ~std::size_t{7}; }

constexpr std::uint8_t kSymIdTag = '#';         // 0x23
constexpr std::uint8_t kDefaultValueTag = '%';  // 0x25

// Reads one NamedRef starting at `pos` (specifics-relative) and advances
// `pos` past it. Returns nullopt -- leaving `pos` unspecified -- if `pos`
// is `%`-tagged (an inline default-value expression, not modeled) or the
// data runs out before a NUL terminator (and, for a `#`-tagged entry,
// before the trailing sym_id).
std::optional<NamedRef> read_named_ref(const std::vector<std::uint8_t> &s,
                                        std::size_t &pos) {
  if (pos >= s.size())
    return std::nullopt;
  if (s[pos] == kDefaultValueTag)
    return std::nullopt;
  bool tagged = s[pos] == kSymIdTag;
  std::size_t name_start = tagged ? pos + 1 : pos;
  if (name_start > s.size())
    return std::nullopt;
  std::size_t nul = name_start;
  while (nul < s.size() && s[nul] != 0)
    ++nul;
  if (nul == s.size())
    return std::nullopt;
  NamedRef ref;
  ref.name = std::string(reinterpret_cast<const char *>(s.data() + name_start),
                          nul - name_start);
  pos = nul + 1;
  if (tagged) {
    if (pos + 4 > s.size())
      return std::nullopt;
    ref.sym_id = u32(s, pos);
    pos += 4;
  }
  return ref;
}

std::optional<std::vector<NamedRef>>
read_named_ref_list(const std::vector<std::uint8_t> &s, std::size_t &pos,
                     std::uint16_t count) {
  std::vector<NamedRef> out;
  out.reserve(count);
  for (std::uint16_t i = 0; i < count; ++i) {
    std::optional<NamedRef> ref = read_named_ref(s, pos);
    if (!ref.has_value())
      return std::nullopt;
    out.push_back(std::move(*ref));
  }
  return out;
}

} // namespace

std::string Module::name() const {
  return std::filesystem::path(path).stem().string();
}

std::optional<ScalarType> parse_scalar_type(const Entity &e) {
  if (e.decl_rec_kind != DeclRecKind::SimpleScalar)
    return std::nullopt;
  // `stype` (body offset 64, specifics[0]) is present unconditionally --
  // unlike the connode, which only exists when this is a PARAMETER (see
  // parse_scalar_constant()) -- so this works for a plain variable too.
  if (e.specifics.empty())
    return std::nullopt;
  return static_cast<ScalarType>(e.specifics[0]);
}

std::optional<std::uint32_t> parse_char_length(const Entity &e) {
  if (e.decl_rec_kind != DeclRecKind::Char)
    return std::nullopt;
  if (e.specifics.size() < 4)
    return std::nullopt;
  return u32(e.specifics, 0); // body offset 64
}

std::optional<std::string> parse_scalar_constant(const Entity &e) {
  if (e.decl_rec_kind != DeclRecKind::Char &&
      e.decl_rec_kind != DeclRecKind::SimpleScalar)
    return std::nullopt;

  // The connode sits at body offset 72 for both kinds, i.e. specifics[8]
  // (specifics starts at body offset 64) -- see the header comment for
  // the full offset layout. Bounds-check generously: refuse rather than
  // read past the end of a shorter-than-expected specifics blob (e.g. a
  // non-PARAMETER simple_scalar, whose specifics is only 8 bytes long
  // with no connode at all).
  const std::vector<std::uint8_t> &s = e.specifics;
  constexpr std::size_t kTypeIdx = 21;
  if (s.size() <= kTypeIdx)
    return std::nullopt;
  ScalarType type = static_cast<ScalarType>(s[kTypeIdx]);

  constexpr std::size_t kValueIdx = 40;
  constexpr std::size_t kLenIdx = 32;
  constexpr std::size_t kTextIdx = 56;

  switch (type) {
  // All four LOGICAL widths and all four INTEGER widths are read the
  // same way, an 8-byte value regardless of the declared kind's actual
  // storage size (e.g. an INTEGER(4) with value 42 has the full 8 bytes
  // 2a 00 00 00 00 00 00 00, not just the low 4).
  case ScalarType::Log1:
  case ScalarType::Log2:
  case ScalarType::Log4:
  case ScalarType::Log8:
  case ScalarType::Int1:
  case ScalarType::Int2:
  case ScalarType::Int4:
  case ScalarType::Int8: {
    if (s.size() < kValueIdx + 8)
      return std::nullopt;
    std::int64_t v = s64(s, kValueIdx);
    bool is_logical = type == ScalarType::Log1 || type == ScalarType::Log2 ||
                       type == ScalarType::Log4 || type == ScalarType::Log8;
    if (is_logical)
      return v != 0 ? ".true." : ".false.";
    // The raw 8 bytes are zero-extended, not sign-extended, from the
    // declared width (confirmed: a real INTEGER(4) flag constant with
    // its top bit set, e.g. OMP_SCHED_MONOTONIC = Z'80000000', reads
    // back as +2147483648 here -- which doesn't fit in a literal
    // Fortran source would accept for INTEGER(4) -- rather than the
    // real, intended -2147483648). Narrow and sign-extend at the
    // declared width before rendering.
    switch (type) {
    case ScalarType::Int1:
      v = static_cast<std::int8_t>(v);
      break;
    case ScalarType::Int2:
      v = static_cast<std::int16_t>(v);
      break;
    case ScalarType::Int4:
      v = static_cast<std::int32_t>(v);
      break;
    default:
      break; // Int8: already the full width
    }
    return format_integer_literal(v, type);
  }
  case ScalarType::Real4: {
    if (s.size() < kValueIdx + 4)
      return std::nullopt;
    return format_real_literal(f32(s, kValueIdx));
  }
  case ScalarType::Real8: {
    if (s.size() < kValueIdx + 8)
      return std::nullopt;
    return format_real_literal(f64(s, kValueIdx));
  }
  case ScalarType::Real16: { // quad, narrowed to double -- see f128_to_double
    if (s.size() < kValueIdx + 16)
      return std::nullopt;
    std::optional<double> v = f128_to_double(s, kValueIdx);
    if (!v.has_value())
      return std::nullopt;
    return format_real_literal(*v);
  }
  case ScalarType::Cmpx8: { // two consecutive float32 (real, imag)
    if (s.size() < kValueIdx + 8)
      return std::nullopt;
    return "(" + format_real_literal(f32(s, kValueIdx)) + "," +
           format_real_literal(f32(s, kValueIdx + 4)) + ")";
  }
  case ScalarType::Cmpx16: { // two consecutive float64 (real, imag)
    if (s.size() < kValueIdx + 16)
      return std::nullopt;
    return "(" + format_real_literal(f64(s, kValueIdx)) + "," +
           format_real_literal(f64(s, kValueIdx + 8)) + ")";
  }
  case ScalarType::Char: {
    if (s.size() < kLenIdx + 4)
      return std::nullopt;
    std::uint32_t len = u32(s, kLenIdx);
    // `len` includes the 1-byte encoding tag in front of the actual
    // string bytes; refuse rather than guess for len == 0 (would mean no
    // tag byte at all) or a length that doesn't fit in what was actually
    // captured.
    if (len == 0 || s.size() < kTextIdx + len)
      return std::nullopt;
    std::string raw(reinterpret_cast<const char *>(s.data() + kTextIdx + 1),
                     len - 1);
    std::string escaped;
    escaped.reserve(raw.size() + 2);
    for (char c : raw) {
      if (c == '\'')
        escaped += "''";
      else
        escaped += c;
    }
    return "'" + escaped + "'";
  }
  default:
    return std::nullopt;
  }
}

std::optional<std::string> to_fortran_type(ScalarType type) {
  switch (type) {
  case ScalarType::Log1:
    return "logical(1)";
  case ScalarType::Log2:
    return "logical(2)";
  case ScalarType::Log4:
    return "logical(4)";
  case ScalarType::Log8:
    return "logical(8)";
  case ScalarType::Int1:
    return "integer(1)";
  case ScalarType::Int2:
    return "integer(2)";
  case ScalarType::Int4:
    return "integer(4)";
  case ScalarType::Int8:
    return "integer(8)";
  case ScalarType::Real4:
    return "real(4)";
  case ScalarType::Real8:
    return "real(8)";
  case ScalarType::Real16:
    return "real(16)";
  case ScalarType::Cmpx8:
    return "complex(4)";
  case ScalarType::Cmpx16:
    return "complex(8)";
  case ScalarType::Char:
    return "character";
  default:
    return std::nullopt;
  }
}

std::optional<DummyArgInfo> parse_dummy_arg(const Entity &e) {
  if (e.decl_rec_kind != DeclRecKind::SimpleScalarArg)
    return std::nullopt;
  const std::vector<std::uint8_t> &s = e.specifics;
  if (s.size() < 2)
    return std::nullopt;
  DummyArgInfo info;
  info.type = static_cast<ScalarType>(s[0]);        // body offset 64
  info.passed_by = static_cast<PassedBy>(s[1]); // body offset 65
  return info;
}

std::optional<CharArgInfo> parse_char_arg(const Entity &e) {
  if (e.decl_rec_kind != DeclRecKind::CharArg)
    return std::nullopt;
  const std::vector<std::uint8_t> &s = e.specifics;
  constexpr std::size_t kLen = 0;           // body offset 64
  constexpr std::size_t kPassedBy = 8;      // body offset 72
  constexpr std::size_t kLenExprBegin = 12; // body offset 76
  constexpr std::size_t kLenExprTag = 16;   // body offset 80
  constexpr std::uint16_t kTagStar = 305;   // "STAR", assumed length (*)
  if (s.size() < kLenExprBegin)
    return std::nullopt;

  CharArgInfo info;
  info.passed_by = static_cast<PassedBy>(s[kPassedBy]);

  // Explicit, compile-time-constant length: the trailing "len
  // expression" region is entirely absent (all zero) and num_nbytes
  // already holds the real length.
  bool all_zero = true;
  for (std::size_t i = kLenExprBegin; i < s.size(); ++i) {
    if (s[i] != 0) {
      all_zero = false;
      break;
    }
  }
  if (all_zero) {
    info.length = u32(s, kLen);
    return info;
  }
  // Assumed length, `character(*)`: a 4-byte zero gap, then "STAR" (tag
  // 305) -- num_nbytes is meaningless here.
  if (s.size() >= kLenExprTag + 4 && s[kLenExprBegin] == 0 &&
      s[kLenExprBegin + 1] == 0 && s[kLenExprBegin + 2] == 0 &&
      s[kLenExprBegin + 3] == 0 && u16(s, kLenExprTag) == kTagStar) {
    info.is_assumed_length = true;
    return info;
  }
  // Anything else (a length that's itself a symbol reference, or a
  // shape not seen yet) isn't decoded -- refuse rather than guess.
  return std::nullopt;
}

std::optional<StructArgInfo> parse_struct_arg(const Entity &e) {
  if (e.decl_rec_kind != DeclRecKind::StructArg)
    return std::nullopt;
  const std::vector<std::uint8_t> &s = e.specifics;
  constexpr std::size_t kNbytes = 0;        // body offset 64
  constexpr std::size_t kPassedBy = 8;      // body offset 72
  constexpr std::size_t kTypeNameStart = 16; // body offset 80
  if (s.size() < kTypeNameStart)
    return std::nullopt;
  StructArgInfo info;
  info.nbytes = u32(s, kNbytes);
  info.passed_by = static_cast<PassedBy>(s[kPassedBy]);
  std::size_t pos = kTypeNameStart;
  std::optional<NamedRef> type_name = read_named_ref(s, pos);
  if (!type_name.has_value())
    return std::nullopt;
  info.type_name = std::move(*type_name);
  return info;
}

std::optional<ProcedureInfo> parse_procedure(const Entity &e) {
  bool is_func = e.decl_rec_kind == DeclRecKind::FuncRetSimpleScalar;
  if (e.decl_rec_kind != DeclRecKind::Subr && !is_func)
    return std::nullopt;

  const std::vector<std::uint8_t> &s = e.specifics;
  // All offsets below are specifics-relative (specifics[0] == body
  // offset 64) -- see the header comment for the full layout.
  constexpr std::size_t kNumUses = 0;
  constexpr std::size_t kNumAbstracts = 2;
  constexpr std::size_t kNumTypes = 4;
  constexpr std::size_t kNumParams = 6;
  constexpr std::size_t kNumCommons = 8;
  constexpr std::size_t kNumIntrinsics = 10;
  constexpr std::size_t kSymNargu = 12;
  constexpr std::size_t kBlankCommonFlag = 14;
  constexpr std::size_t kReturnType = 16; // FuncRetSimpleScalar only
  if (s.size() <= kBlankCommonFlag)
    return std::nullopt;

  ProcedureInfo info;
  std::size_t header_end;
  if (is_func) {
    if (s.size() <= kReturnType)
      return std::nullopt;
    info.return_type = static_cast<ScalarType>(s[kReturnType]);
    header_end = kReturnType + 1;
  } else {
    header_end = kBlankCommonFlag + 1;
  }
  std::size_t pos = round_up8(header_end);

  std::uint16_t num_uses = u16(s, kNumUses);
  std::uint16_t num_abstracts = u16(s, kNumAbstracts);
  std::uint16_t num_types = u16(s, kNumTypes);
  std::uint16_t num_params = u16(s, kNumParams);
  std::uint16_t num_commons = u16(s, kNumCommons);
  std::uint16_t num_intrinsics = u16(s, kNumIntrinsics);
  std::uint16_t sym_nargu = u16(s, kSymNargu);
  bool blank_common = s[kBlankCommonFlag] != 0;

  // Walked only to find where `arguments` starts -- nothing downstream
  // needs the uses/abstracts/types/params/commons/intrinsics lists
  // themselves, so their contents are discarded.
  if (!read_named_ref_list(s, pos, num_uses).has_value() ||
      !read_named_ref_list(s, pos, num_abstracts).has_value() ||
      !read_named_ref_list(s, pos, num_types).has_value() ||
      !read_named_ref_list(s, pos, num_params).has_value() ||
      !read_named_ref_list(s, pos, num_commons).has_value())
    return std::nullopt;
  if (blank_common && !read_named_ref_list(s, pos, 1).has_value())
    return std::nullopt;
  if (!read_named_ref_list(s, pos, num_intrinsics).has_value())
    return std::nullopt;

  std::optional<std::vector<NamedRef>> args =
      read_named_ref_list(s, pos, sym_nargu);
  if (!args.has_value())
    return std::nullopt;
  info.arguments = std::move(*args);
  return info;
}

std::optional<FuncRetCharInfo> parse_func_ret_char(const Entity &e) {
  if (e.decl_rec_kind != DeclRecKind::FuncRetChar)
    return std::nullopt;
  const std::vector<std::uint8_t> &s = e.specifics;
  constexpr std::size_t kLen = 0;         // body offset 64
  constexpr std::size_t kReservedEnd = 20; // body offset 84 (16 zero bytes from 68)
  constexpr std::size_t kArgCount = 20;    // body offset 84
  constexpr std::size_t kArgsStart = 24;   // body offset 88
  if (s.size() < kArgsStart)
    return std::nullopt;
  for (std::size_t i = kLen + 4; i < kReservedEnd; ++i)
    if (s[i] != 0)
      return std::nullopt;
  FuncRetCharInfo info;
  info.length = u32(s, kLen);
  std::uint32_t arg_count = u32(s, kArgCount);
  std::size_t pos = kArgsStart;
  std::optional<std::vector<NamedRef>> arguments =
      read_named_ref_list(s, pos, static_cast<std::uint16_t>(arg_count));
  if (!arguments.has_value())
    return std::nullopt;
  info.arguments = std::move(*arguments);
  return info;
}

std::optional<FuncRetStructInfo> parse_func_ret_struct(const Entity &e) {
  if (e.decl_rec_kind != DeclRecKind::FuncRetStruct)
    return std::nullopt;
  const std::vector<std::uint8_t> &s = e.specifics;
  constexpr std::size_t kNbytes = 0;        // body offset 64
  constexpr std::size_t kReservedEnd = 20;  // body offset 84 (16 zero bytes from 68)
  constexpr std::size_t kArgCount = 20;     // body offset 84
  constexpr std::size_t kArgsStart = 24;    // body offset 88
  if (s.size() < kArgsStart)
    return std::nullopt;
  for (std::size_t i = kNbytes + 4; i < kReservedEnd; ++i)
    if (s[i] != 0)
      return std::nullopt;
  FuncRetStructInfo info;
  info.nbytes = u32(s, kNbytes);
  std::uint32_t arg_count = u32(s, kArgCount);
  std::size_t pos = kArgsStart;
  std::optional<std::vector<NamedRef>> arguments =
      read_named_ref_list(s, pos, static_cast<std::uint16_t>(arg_count));
  if (!arguments.has_value())
    return std::nullopt;
  info.arguments = std::move(*arguments);
  std::optional<NamedRef> type_name = read_named_ref(s, pos);
  if (!type_name.has_value())
    return std::nullopt;
  info.type_name = std::move(*type_name);
  return info;
}

std::optional<DerivedTypeFields> parse_derived_type(const Entity &e) {
  if (e.decl_rec_kind != DeclRecKind::DerivedType)
    return std::nullopt;
  const std::vector<std::uint8_t> &s = e.specifics;
  constexpr std::size_t kNumFields = 16; // body offset 80
  constexpr std::size_t kHeaderEnd = 22; // body offset 86
  if (s.size() < kHeaderEnd)
    return std::nullopt;
  std::uint16_t num_fields = u16(s, kNumFields);
  std::size_t pos = round_up8(kHeaderEnd);
  return read_named_ref_list(s, pos, num_fields);
}

std::optional<SimpleScalarFieldInfo> parse_simple_scalar_field(const Entity &e) {
  if (e.decl_rec_kind != DeclRecKind::SimpleScalarField)
    return std::nullopt;
  const std::vector<std::uint8_t> &s = e.specifics;
  constexpr std::size_t kOffset = 0;   // body offset 64
  constexpr std::size_t kNumInit = 10; // body offset 74
  constexpr std::size_t kType = 12;    // body offset 76
  if (s.size() <= kType)
    return std::nullopt;
  SimpleScalarFieldInfo info;
  info.offset = u32(s, kOffset);
  info.type = static_cast<ScalarType>(s[kType]);
  std::uint16_t num_init = u16(s, kNumInit);
  if (num_init > 0) {
    info.has_undecoded_initializer = true;
    // Confirmed shape, single-value only: init.offset/count/ncopies (u64
    // each, all must read as 0/1/1 -- an array-valued or RESHAPE-style
    // default with any of those different isn't sampled) then two more
    // constant-1 u64 fields (not otherwise understood) and a zero u64,
    // then the same value block a PARAMETER array's CONTOK node carries
    // -- see read_value_block()'s comment.
    constexpr std::size_t kInitStart = 16; // body offset 80
    constexpr std::size_t kInitOffset = kInitStart;
    constexpr std::size_t kInitCount = kInitStart + 8;
    constexpr std::size_t kInitNcopies = kInitStart + 16;
    constexpr std::size_t kValueBlockStart = kInitStart + 48; // body offset 128
    if (num_init == 1 && s.size() > kValueBlockStart &&
        u64(s, kInitOffset) == 0 && u64(s, kInitCount) == 1 &&
        u64(s, kInitNcopies) == 1) {
      std::size_t pos = kValueBlockStart;
      std::optional<ContokValueLocation> loc = read_value_block(s, pos);
      std::optional<std::string> rendered =
          loc ? render_contok_value(s, *loc, info.type) : std::nullopt;
      if (rendered.has_value()) {
        info.default_value = std::move(*rendered);
        info.has_undecoded_initializer = false;
      }
    }
  }
  return info;
}

std::optional<CharFieldInfo> parse_char_field(const Entity &e) {
  if (e.decl_rec_kind != DeclRecKind::CharField)
    return std::nullopt;
  const std::vector<std::uint8_t> &s = e.specifics;
  constexpr std::size_t kLen = 0;    // body offset 64
  constexpr std::size_t kOffset = 8; // body offset 72
  if (s.size() < kOffset + 4)
    return std::nullopt;
  CharFieldInfo info;
  info.length = u32(s, kLen);
  info.offset = u32(s, kOffset);
  // Confirmed 2026-09-17 by comparing a with- and without-default
  // sample: a single byte at body offset 82 (specifics offset 18) is
  // nonzero exactly when a default value follows. Same confirmed
  // "offset/count/ncopies/two more constant-1 fields/zero/value block"
  // initializer wrapper as SimpleScalarField's own default (see its
  // comment), just starting at body offset 88 (specifics offset 24)
  // instead of 80 -- CharField's own header ends 8 bytes later.
  constexpr std::size_t kHasInit = 18; // body offset 82
  if (s.size() > kHasInit && s[kHasInit] != 0) {
    info.has_undecoded_initializer = true;
    constexpr std::size_t kInitStart = 24; // body offset 88
    constexpr std::size_t kInitOffset = kInitStart;
    constexpr std::size_t kInitCount = kInitStart + 8;
    constexpr std::size_t kInitNcopies = kInitStart + 16;
    constexpr std::size_t kValueBlockStart = kInitStart + 48; // body offset 136
    if (s.size() > kValueBlockStart && u64(s, kInitOffset) == 0 &&
        u64(s, kInitCount) == 1 && u64(s, kInitNcopies) == 1) {
      std::size_t pos = kValueBlockStart;
      std::optional<ContokValueLocation> loc = read_value_block(s, pos);
      std::optional<std::string> rendered =
          loc ? render_contok_value(s, *loc, ScalarType::Char) : std::nullopt;
      if (rendered.has_value()) {
        info.default_value = std::move(*rendered);
        info.has_undecoded_initializer = false;
      }
    }
  }
  return info;
}

std::optional<CharArrayFieldInfo> parse_char_array_field(const Entity &e) {
  if (e.decl_rec_kind != DeclRecKind::CharArrayField)
    return std::nullopt;
  const std::vector<std::uint8_t> &s = e.specifics;
  constexpr std::size_t kSize = 0;    // body offset 64
  constexpr std::size_t kLength = 16; // body offset 80
  constexpr std::size_t kNdimen = 29; // body offset 93
  if (s.size() <= kNdimen)
    return std::nullopt;
  CharArrayFieldInfo info;
  info.size = u32(s, kSize);
  info.length = u32(s, kLength);
  info.ndimen = s[kNdimen];
  return info;
}

std::optional<TypeBoundProcFieldInfo>
parse_type_bound_proc_field(const Entity &e) {
  if (e.decl_rec_kind != DeclRecKind::TypeBoundProcField)
    return std::nullopt;
  const std::vector<std::uint8_t> &s = e.specifics;
  constexpr std::size_t kPassedArg = 12;   // body offset 76
  constexpr std::size_t kIfaceStart = 16;  // body offset 80
  if (s.size() < kIfaceStart)
    return std::nullopt;
  // `TBProcEnum` (body offset 74, not otherwise modeled) was first
  // thought to gate "normal binding vs GENERIC" (refusing unless == 1),
  // but a second sample showed it varies (1 then 2) across two ordinary
  // PASS-bound bindings that both happen to also be listed as a GENERIC
  // interface's specific procedures -- decl_rec_kind alone (this kind vs
  // TypeBoundGenericField/52) already distinguishes GENERIC, so this
  // isn't gated on at all.
  TypeBoundProcFieldInfo info;
  info.passed_arg = s[kPassedArg] != 0;
  std::size_t pos = kIfaceStart;
  std::optional<NamedRef> iface = read_named_ref(s, pos);
  if (!iface.has_value())
    return std::nullopt;
  info.interface_symbol = std::move(*iface);
  return info;
}

std::optional<TypeBoundGenericFieldInfo>
parse_type_bound_generic_field(const Entity &e) {
  if (e.decl_rec_kind != DeclRecKind::TypeBoundGenericField)
    return std::nullopt;
  const std::vector<std::uint8_t> &s = e.specifics;
  constexpr std::size_t kSeqno = 8; // body offset 72
  if (s.size() < kSeqno + 4)
    return std::nullopt;
  TypeBoundGenericFieldInfo info;
  info.seqno = u32(s, kSeqno);
  return info;
}

std::optional<TbpGennodeInfo> parse_tbp_gennode(const Entity &e) {
  if (e.decl_rec_kind != DeclRecKind::TbpGennode)
    return std::nullopt;
  const std::vector<std::uint8_t> &s = e.specifics;
  constexpr std::size_t kNumProcs = 0;       // body offset 64
  constexpr std::size_t kGenericOperator = 4; // body offset 68
  constexpr std::size_t kGenStacod = 6;       // body offset 70
  constexpr std::size_t kParentStart = 8;     // body offset 72
  if (s.size() < kParentStart)
    return std::nullopt;
  std::uint32_t num_procs = u32(s, kNumProcs);
  TbpGennodeInfo info;
  info.generic_operator = u16(s, kGenericOperator);
  info.gen_stacod = u16(s, kGenStacod);
  std::size_t pos = kParentStart;
  std::optional<NamedRef> parent = read_named_ref(s, pos);
  if (!parent.has_value())
    return std::nullopt;
  info.parent = std::move(*parent);
  std::optional<std::vector<NamedRef>> gen_procs =
      read_named_ref_list(s, pos, static_cast<std::uint16_t>(num_procs));
  if (!gen_procs.has_value())
    return std::nullopt;
  info.gen_procs = std::move(*gen_procs);
  return info;
}

std::optional<StructVariableInfo> parse_struct_variable(const Entity &e) {
  if (e.decl_rec_kind != DeclRecKind::Struct)
    return std::nullopt;
  const std::vector<std::uint8_t> &s = e.specifics;
  constexpr std::size_t kNbytes = 0; // body offset 64
  if (s.size() < kNbytes + 4)
    return std::nullopt;
  StructVariableInfo info;
  info.nbytes = u32(s, kNbytes);
  if (e.skind == SymbolKind::Param)
    return info; // connode path, not confirmed for a struct -- see above
  constexpr std::size_t kTypeNameStart = 8; // body offset 72
  std::size_t pos = kTypeNameStart;
  std::optional<NamedRef> type_name = read_named_ref(s, pos);
  if (!type_name.has_value())
    return std::nullopt;
  info.type_name = std::move(*type_name);
  return info;
}

std::optional<ExternFuncInfo> parse_extern_func(const Entity &e) {
  if (e.decl_rec_kind != DeclRecKind::ExternFunc)
    return std::nullopt;
  const std::vector<std::uint8_t> &s = e.specifics;
  constexpr std::size_t kNbytes = 0;   // body offset 64
  constexpr std::size_t kNargu = 8;    // body offset 72
  constexpr std::size_t kType = 10;    // body offset 74
  if (s.size() <= kType)
    return std::nullopt;
  ExternFuncInfo info;
  info.nbytes = u32(s, kNbytes);
  info.sym_nargu = u16(s, kNargu);
  info.type = static_cast<ScalarType>(s[kType]);
  return info;
}

std::optional<ExternDerivedTypeFuncInfo>
parse_extern_derived_type_func(const Entity &e) {
  if (e.decl_rec_kind != DeclRecKind::ExternDerivedTypeFunc)
    return std::nullopt;
  const std::vector<std::uint8_t> &s = e.specifics;
  constexpr std::size_t kNbytes = 0;         // body offset 64
  constexpr std::size_t kNargu = 8;          // body offset 72
  constexpr std::size_t kTypeNameStart = 16; // body offset 80
  if (s.size() < kTypeNameStart)
    return std::nullopt;
  ExternDerivedTypeFuncInfo info;
  info.nbytes = u32(s, kNbytes);
  info.sym_nargu = u16(s, kNargu);
  std::size_t pos = kTypeNameStart;
  std::optional<NamedRef> type_name = read_named_ref(s, pos);
  if (!type_name.has_value())
    return std::nullopt;
  info.type_name = std::move(*type_name);
  return info;
}

std::optional<StructFieldInfo> parse_struct_field(const Entity &e) {
  if (e.decl_rec_kind != DeclRecKind::StructField)
    return std::nullopt;
  const std::vector<std::uint8_t> &s = e.specifics;
  constexpr std::size_t kNbytes = 0; // body offset 64
  constexpr std::size_t kOffset = 8; // body offset 72
  constexpr std::size_t kTypeNameStart = 24; // body offset 88
  if (s.size() < kTypeNameStart)
    return std::nullopt;
  StructFieldInfo info;
  info.nbytes = u32(s, kNbytes);
  info.offset = u32(s, kOffset);
  std::size_t pos = kTypeNameStart;
  std::optional<NamedRef> type_name = read_named_ref(s, pos);
  if (!type_name.has_value())
    return std::nullopt;
  info.type_name = std::move(*type_name);
  return info;
}

std::optional<CommonBlockVariables> parse_common_block(const Entity &e) {
  if (e.decl_rec_kind != DeclRecKind::Common)
    return std::nullopt;
  const std::vector<std::uint8_t> &s = e.specifics;
  constexpr std::size_t kNumVars = 8;    // body offset 72
  constexpr std::size_t kListStart = 24; // body offset 88
  if (s.size() < kListStart)
    return std::nullopt;
  std::uint32_t num_vars = u32(s, kNumVars);
  std::size_t pos = kListStart;
  return read_named_ref_list(s, pos, static_cast<std::uint16_t>(num_vars));
}

std::optional<CommonVariableInfo> parse_common_variable(const Entity &e) {
  if (e.decl_rec_kind != DeclRecKind::SimpleScalarCommon)
    return std::nullopt;
  const std::vector<std::uint8_t> &s = e.specifics;
  constexpr std::size_t kOffset = 0; // body offset 64
  constexpr std::size_t kType = 8;   // body offset 72
  if (s.size() <= kType)
    return std::nullopt;
  CommonVariableInfo info;
  info.offset = u32(s, kOffset);
  info.type = static_cast<ScalarType>(s[kType]);
  return info;
}

std::optional<CharCommonInfo> parse_char_common(const Entity &e) {
  if (e.decl_rec_kind != DeclRecKind::CharCommon)
    return std::nullopt;
  const std::vector<std::uint8_t> &s = e.specifics;
  constexpr std::size_t kLen = 0;    // body offset 64
  constexpr std::size_t kOffset = 8; // body offset 72
  if (s.size() < kOffset + 4)
    return std::nullopt;
  CharCommonInfo info;
  info.length = u32(s, kLen);
  info.offset = u32(s, kOffset);
  return info;
}

std::optional<StructCommonInfo> parse_struct_common(const Entity &e) {
  if (e.decl_rec_kind != DeclRecKind::StructCommon)
    return std::nullopt;
  const std::vector<std::uint8_t> &s = e.specifics;
  constexpr std::size_t kNbytes = 0;          // body offset 64
  constexpr std::size_t kOffset = 8;          // body offset 72
  constexpr std::size_t kTypeNameStart = 16; // body offset 80
  if (s.size() < kTypeNameStart)
    return std::nullopt;
  StructCommonInfo info;
  info.nbytes = u32(s, kNbytes);
  info.offset = u32(s, kOffset);
  std::size_t pos = kTypeNameStart;
  std::optional<NamedRef> type_name = read_named_ref(s, pos);
  if (!type_name.has_value())
    return std::nullopt;
  info.type_name = std::move(*type_name);
  return info;
}

std::optional<NamelistVariables> parse_namelist_group(const Entity &e) {
  if (e.decl_rec_kind != DeclRecKind::NamelistGroup)
    return std::nullopt;
  const std::vector<std::uint8_t> &s = e.specifics;
  constexpr std::size_t kNumNames = 0;  // body offset 64
  constexpr std::size_t kListStart = 8; // body offset 72
  if (s.size() < kListStart)
    return std::nullopt;
  std::uint32_t num_names = u32(s, kNumNames);
  std::size_t pos = kListStart;
  return read_named_ref_list(s, pos, static_cast<std::uint16_t>(num_names));
}

std::optional<OverlayInfo> parse_overlay(const Entity &e) {
  if (e.decl_rec_kind != DeclRecKind::Overlay)
    return std::nullopt;
  const std::vector<std::uint8_t> &s = e.specifics;
  // No common envelope -- specifics starts right after decl_rec_kind
  // (body offset 4), not offset 64. See OverlayInfo's comment.
  constexpr std::size_t kOffset = 4;    // body offset 8
  constexpr std::size_t kCount = 8;     // body offset 12
  constexpr std::size_t kListStart = 12; // body offset 16
  if (s.size() < kListStart)
    return std::nullopt;
  OverlayInfo info;
  info.offset = u32(s, kOffset);
  std::uint32_t count = u32(s, kCount);
  std::size_t pos = kListStart;
  std::optional<std::vector<NamedRef>> syms =
      read_named_ref_list(s, pos, static_cast<std::uint16_t>(count));
  if (!syms.has_value())
    return std::nullopt;
  info.ovl_syms = std::move(*syms);
  return info;
}

std::optional<SimpleArrayInfo> parse_simple_array(const Entity &e) {
  if (e.decl_rec_kind != DeclRecKind::SimpleScalarArray)
    return std::nullopt;
  const std::vector<std::uint8_t> &s = e.specifics;
  constexpr std::size_t kSize = 0;    // body offset 64
  constexpr std::size_t kNbytes = 8;  // body offset 72
  constexpr std::size_t kType = 16;   // body offset 80
  constexpr std::size_t kNdimen = 17; // body offset 81
  if (s.size() <= kNdimen)
    return std::nullopt;
  SimpleArrayInfo info;
  info.size = u32(s, kSize);
  info.element_bytes = u32(s, kNbytes);
  info.type = static_cast<ScalarType>(s[kType]);
  info.ndimen = s[kNdimen];
  info.is_parameter = e.skind == SymbolKind::Param;
  return info;
}

namespace {

constexpr std::uint16_t kExprTagVal = 368;
constexpr std::uint16_t kExprTagListOpen = 2;
constexpr std::uint16_t kExprTagListClose = 3;
constexpr std::uint16_t kExprTagColon = 294; // the "COLON(lower,upper)" range

// A PARAMETER array's own "parameter_expression" -- the vendor
// diagnostic's own node-kind names, e.g. "AREX(CARRCON(1,[CONTOK(intval:
// 42)CONTOK(intval:100)]),SZLCON([2]))". Confirmed via three independent
// samples (single-element, two-element, and two elements with very
// different digit counts) that decode consistently down to the byte and
// land exactly on the `dim_info` that follows -- see parse_array_constant()
// in module.hpp for what's NOT confirmed (only integers, only rank 1).
constexpr std::uint16_t kExprTagArex = 340;
constexpr std::uint16_t kExprTagCarrcon = 343;
constexpr std::uint16_t kExprTagContok = 366;
constexpr std::uint16_t kExprTagSzlcon = 360;

// Rounds `p` up to the next 8-byte boundary STRICTLY greater than `p`
// (not greater-or-equal) -- confirmed 2026-09-17 against a LOGICAL
// PARAMETER array sample, the first one whose CONTOK header happened to
// already land on an 8-byte boundary: the padding before the value block
// is still a full (up to 8-byte) gap in that case, not zero. A plain
// ceiling matched every INTEGER/REAL sample seen before that only by
// coincidence (their headers never landed exactly on a boundary).
std::size_t align8(std::size_t p) { return (p & ~static_cast<std::size_t>(7)) + 8; }

// Reads one plain integer leaf (tag 368: 2-byte tag, 1-byte ScalarType,
// 1-byte unused, 4-byte little-endian value, 4 zero-padded high bytes --
// 12 bytes total) at `pos` (specifics-relative) and advances `pos` past
// it.
std::optional<std::uint32_t> read_val_leaf(const std::vector<std::uint8_t> &s,
                                            std::size_t &pos) {
  if (pos + 12 > s.size() || u16(s, pos) != kExprTagVal)
    return std::nullopt;
  std::uint32_t value = u32(s, pos + 4);
  pos += 12;
  return value;
}

// Reads one CONTOK integer-constant node at `pos` (specifics-relative,
// Reads one CONTOK constant node at `pos` (specifics-relative, pointing
// at its own 4-byte tag+type+unused header), renders it as Fortran
// source text per `type` (see render_contok_value()'s comment for which
// types are understood), and advances `pos` past it. Fixed shape
// confirmed byte-for-byte: header(4B), then padding up to the next
// 8-byte boundary (relative to the entity body -- 0, 2, 4 or 6 bytes
// depending on where the header landed), then the value block (see
// read_value_block() above) -- with NO trailing padding before whatever
// list marker (open/close) follows.
std::optional<std::string> read_contok_value(const std::vector<std::uint8_t> &s,
                                              std::size_t &pos, ScalarType type) {
  if (pos + 4 > s.size() || u16(s, pos) != kExprTagContok)
    return std::nullopt;
  pos = align8(pos + 4);
  std::optional<ContokValueLocation> loc = read_value_block(s, pos);
  if (!loc.has_value())
    return std::nullopt;
  return render_contok_value(s, *loc, type);
}

// Reads a PARAMETER array's "parameter_expression" at `pos` (specifics-
// relative, pointing at AREX's own tag), rendering each element per
// `type` (see render_contok_value()'s comment), and advances `pos` past
// it -- landing exactly on `dim_info`, which starts wherever this ends
// (see parse_array_constant()'s comment in module.hpp). `expected_count`
// (the array's own already-known `size`) is cross-checked against how
// many CONTOK elements are actually found, refusing on any mismatch
// rather than silently trusting either source.
std::optional<std::vector<std::string>>
read_array_constant(const std::vector<std::uint8_t> &s, std::size_t &pos,
                     std::uint32_t expected_count, ScalarType type,
                     std::uint8_t ndimen) {
  auto expect_tag = [&](std::uint16_t tag) {
    if (pos + 4 > s.size() || u16(s, pos) != tag)
      return false;
    pos += 4;
    return true;
  };
  if (!expect_tag(kExprTagArex) || !expect_tag(kExprTagCarrcon))
    return std::nullopt;
  if (!read_val_leaf(s, pos).has_value()) // the "1" in CARRCON(1, ...)
    return std::nullopt;
  if (!expect_tag(kExprTagListOpen))
    return std::nullopt;
  std::vector<std::string> values;
  while (true) {
    std::optional<std::string> v = read_contok_value(s, pos, type);
    if (!v.has_value())
      return std::nullopt;
    values.push_back(std::move(*v));
    if (pos + 4 > s.size())
      return std::nullopt;
    std::uint16_t marker = u16(s, pos);
    pos += 4;
    if (marker == kExprTagListClose)
      break;
    if (marker != kExprTagListOpen)
      return std::nullopt;
  }
  if (values.size() != expected_count)
    return std::nullopt;
  // SZLCON's own header -- unlike CONTOK's, not preceded by alignment
  // padding (confirmed: it directly follows CONTOK's closing marker).
  if (!expect_tag(kExprTagSzlcon))
    return std::nullopt;
  // SZLCON's own size list is the same "open, element, open, element,
  // ..., close" chain shape as CONTOK's list above (confirmed on a real
  // rank-2 sample, ndimen == 2, SZLCON([2, 2])) -- one plain val_leaf per
  // dimension, no COLON-range variant (these are always sizes, not
  // bounds).
  if (ndimen == 0)
    return std::nullopt;
  for (std::uint8_t i = 0; i < ndimen; ++i) {
    if (!expect_tag(kExprTagListOpen))
      return std::nullopt;
    if (!read_val_leaf(s, pos).has_value())
      return std::nullopt;
  }
  if (!expect_tag(kExprTagListClose))
    return std::nullopt;
  return values;
}

// Reads one "dimen" chain element at `pos` (specifics-relative,
// pointing at its own 4-byte `open` marker -- tag 2) and advances `pos`
// past it: either a plain leaf (lower bound 1, upper == the leaf value)
// or "COLON(lower, upper)" (a 2-child range node, tag 294, any other
// lower bound).
std::optional<ArrayBound> read_dim_element(const std::vector<std::uint8_t> &s,
                                            std::size_t &pos) {
  if (pos + 4 > s.size() || u16(s, pos) != kExprTagListOpen)
    return std::nullopt;
  pos += 4;
  if (pos + 2 <= s.size() && u16(s, pos) == kExprTagColon) {
    pos += 4;
    std::optional<std::uint32_t> lower = read_val_leaf(s, pos);
    std::optional<std::uint32_t> upper = read_val_leaf(s, pos);
    if (!lower.has_value() || !upper.has_value())
      return std::nullopt;
    return ArrayBound{static_cast<std::int64_t>(*lower),
                       static_cast<std::int64_t>(*upper)};
  }
  std::optional<std::uint32_t> extent = read_val_leaf(s, pos);
  if (!extent.has_value() || *extent == 0)
    return std::nullopt;
  return ArrayBound{1, static_cast<std::int64_t>(*extent)};
}

// Reads "offsetExpr" (skipped -- see parse_array_bounds()'s comment)
// then "dimen" (a chain: one read_dim_element() per dimension, then a
// final 4-byte `close` marker, tag 3) at `pos`, advancing `pos` past it.
std::optional<std::vector<ArrayBound>>
read_dimen_bounds(const std::vector<std::uint8_t> &s, std::size_t &pos,
                   std::uint8_t ndimen) {
  if (!read_val_leaf(s, pos).has_value())
    return std::nullopt;
  std::vector<ArrayBound> bounds;
  bounds.reserve(ndimen);
  for (std::uint8_t i = 0; i < ndimen; ++i) {
    std::optional<ArrayBound> bound = read_dim_element(s, pos);
    if (!bound.has_value())
      return std::nullopt;
    bounds.push_back(*bound);
  }
  if (pos + 4 > s.size() || u16(s, pos) != kExprTagListClose)
    return std::nullopt;
  pos += 4;
  return bounds;
}

// Skips one more dim-shaped chain (the same shape read_dimen_bounds()'s
// "dimen" reads, values discarded) -- StructArray's "sizeOfDims", which
// must be walked past (not just ignored) to reach whatever follows.
bool skip_dim_chain(const std::vector<std::uint8_t> &s, std::size_t &pos,
                     std::uint8_t ndimen) {
  for (std::uint8_t i = 0; i < ndimen; ++i)
    if (!read_dim_element(s, pos).has_value())
      return false;
  if (pos + 4 > s.size() || u16(s, pos) != kExprTagListClose)
    return false;
  pos += 4;
  return true;
}

} // namespace

std::optional<std::vector<ArrayBound>> parse_array_bounds(const Entity &e) {
  std::optional<SimpleArrayInfo> info = parse_simple_array(e);
  if (!info.has_value() || info->ndimen == 0 || info->is_parameter)
    return std::nullopt;
  const std::vector<std::uint8_t> &s = e.specifics;
  std::size_t pos = 24; // round_up8(82 - 64) -- body offset 88
  // offsetExpr: always a plain leaf in every sample seen, but its value
  // doesn't correspond to anything needed here (it isn't simply "the
  // lower bound" once ndimen > 1) -- read_dimen_bounds() validates its
  // shape then skips it; `dimen`, read next, carries everything needed.
  return read_dimen_bounds(s, pos, info->ndimen);
}

std::optional<std::vector<std::string>> parse_array_constant(const Entity &e) {
  std::optional<SimpleArrayInfo> info = parse_simple_array(e);
  if (!info.has_value() || !info->is_parameter || info->ndimen == 0)
    return std::nullopt;
  const std::vector<std::uint8_t> &s = e.specifics;
  std::size_t pos = 72; // round_up8(89 - 64) -- body offset 136
  return read_array_constant(s, pos, info->size, info->type, info->ndimen);
}

std::optional<std::vector<ArrayBound>>
parse_array_constant_bounds(const Entity &e) {
  std::optional<SimpleArrayInfo> info = parse_simple_array(e);
  if (!info.has_value() || !info->is_parameter || info->ndimen == 0)
    return std::nullopt;
  const std::vector<std::uint8_t> &s = e.specifics;
  std::size_t pos = 72; // body offset 136
  if (!read_array_constant(s, pos, info->size, info->type, info->ndimen).has_value())
    return std::nullopt;
  return read_dimen_bounds(s, pos, info->ndimen);
}

std::optional<StructArrayInfo> parse_struct_array(const Entity &e) {
  if (e.decl_rec_kind != DeclRecKind::StructArray)
    return std::nullopt;
  const std::vector<std::uint8_t> &s = e.specifics;
  constexpr std::size_t kSize = 0;    // body offset 64
  constexpr std::size_t kNbytes = 8;  // body offset 72
  constexpr std::size_t kNdimen = 17; // body offset 81 (offset 80 unused)
  if (s.size() <= kNdimen)
    return std::nullopt;
  StructArrayInfo info;
  info.size = u32(s, kSize);
  info.element_bytes = u32(s, kNbytes);
  info.ndimen = s[kNdimen];
  // Only rank 1 is understood past this point -- unlike SimpleScalarArray,
  // whether the trailing gap below scales with ndimen hasn't been checked
  // against a real rank > 1 sample yet.
  if (info.ndimen != 1)
    return std::nullopt;

  // dim_info (offsetExpr + dimen), then a redundant "sizeOfDims" repeat
  // (same shape), then a fixed 16-byte gap ("distribution"/"shadow" here
  // are NOT the same open/element/close chain -- confirmed zero, with no
  // tag structure at all, unlike every other dim_info field), then
  // type_name -- all must be walked, even though only type_name is kept
  // here (parse_struct_array_bounds() below reads the actual bounds).
  std::size_t pos = 24; // round_up8(82 - 64) -- body offset 88
  if (!read_dimen_bounds(s, pos, info.ndimen).has_value() ||
      !skip_dim_chain(s, pos, info.ndimen))
    return std::nullopt;
  constexpr std::size_t kTrailingGap = 16;
  if (pos + kTrailingGap > s.size())
    return std::nullopt;
  pos += kTrailingGap;
  std::optional<NamedRef> type_name = read_named_ref(s, pos);
  if (!type_name.has_value())
    return std::nullopt;
  info.type_name = std::move(*type_name);
  return info;
}

std::optional<std::vector<ArrayBound>> parse_struct_array_bounds(const Entity &e) {
  std::optional<StructArrayInfo> info = parse_struct_array(e);
  if (!info.has_value() || info->ndimen == 0)
    return std::nullopt;
  const std::vector<std::uint8_t> &s = e.specifics;
  std::size_t pos = 24; // round_up8(82 - 64) -- body offset 88
  return read_dimen_bounds(s, pos, info->ndimen);
}

std::optional<CharArrayInfo> parse_char_array(const Entity &e) {
  if (e.decl_rec_kind != DeclRecKind::CharArray)
    return std::nullopt;
  const std::vector<std::uint8_t> &s = e.specifics;
  constexpr std::size_t kSize = 0;    // body offset 64
  constexpr std::size_t kLength = 8;  // body offset 72
  constexpr std::size_t kNdimen = 17; // body offset 81 (offset 80 == CHAR, unused)
  if (s.size() <= kNdimen)
    return std::nullopt;
  CharArrayInfo info;
  info.size = u32(s, kSize);
  info.length = u32(s, kLength);
  info.ndimen = s[kNdimen];
  info.is_parameter = e.skind == SymbolKind::Param;
  return info;
}

std::optional<std::vector<ArrayBound>> parse_char_array_bounds(const Entity &e) {
  std::optional<CharArrayInfo> info = parse_char_array(e);
  if (!info.has_value() || info->ndimen == 0 || info->is_parameter)
    return std::nullopt;
  const std::vector<std::uint8_t> &s = e.specifics;
  std::size_t pos = 24; // round_up8(82 - 64) -- body offset 88
  return read_dimen_bounds(s, pos, info->ndimen);
}

std::optional<FuncRetSimpleScalarArrayInfo>
parse_func_ret_simple_scalar_array(const Entity &e) {
  if (e.decl_rec_kind != DeclRecKind::FuncRetSimpleScalarArray)
    return std::nullopt;
  const std::vector<std::uint8_t> &s = e.specifics;
  constexpr std::size_t kSize = 0;      // body offset 64
  constexpr std::size_t kNbytes = 8;    // body offset 72
  constexpr std::size_t kGapStart = 16; // body offset 80
  constexpr std::size_t kArgCount = 30; // body offset 94
  constexpr std::size_t kType = 34;     // body offset 98
  constexpr std::size_t kNdimen = 35;   // body offset 99
  if (s.size() <= kNdimen)
    return std::nullopt;
  // The constant byte + 13 reserved zero bytes confirmed to precede the
  // argument count -- see the header comment on FuncRetSimpleScalarArrayInfo.
  if (s[kGapStart] != 1)
    return std::nullopt;
  for (std::size_t i = kGapStart + 1; i < kArgCount; ++i)
    if (s[i] != 0)
      return std::nullopt;
  FuncRetSimpleScalarArrayInfo info;
  info.size = u32(s, kSize);
  info.nbytes = u32(s, kNbytes);
  info.type = static_cast<ScalarType>(s[kType]);
  info.ndimen = s[kNdimen];
  std::uint32_t arg_count = u32(s, kArgCount);
  std::size_t pos = round_up8(kNdimen + 1); // body offset 104 when arg_count == 0
  std::optional<std::vector<NamedRef>> arguments =
      read_named_ref_list(s, pos, static_cast<std::uint16_t>(arg_count));
  if (!arguments.has_value())
    return std::nullopt;
  info.arguments = std::move(*arguments);
  return info;
}

std::optional<std::vector<ArrayBound>>
parse_func_ret_simple_scalar_array_bounds(const Entity &e) {
  std::optional<FuncRetSimpleScalarArrayInfo> info =
      parse_func_ret_simple_scalar_array(e);
  if (!info.has_value() || info->ndimen == 0)
    return std::nullopt;
  const std::vector<std::uint8_t> &s = e.specifics;
  std::size_t pos = round_up8(35 + 1); // body offset 104 when arg_count == 0
  std::optional<std::vector<NamedRef>> skip_args = read_named_ref_list(
      s, pos, static_cast<std::uint16_t>(info->arguments.size()));
  if (!skip_args.has_value())
    return std::nullopt;
  return read_dimen_bounds(s, pos, info->ndimen);
}

std::optional<FuncRetStructArrayInfo> parse_func_ret_struct_array(const Entity &e) {
  if (e.decl_rec_kind != DeclRecKind::FuncRetStructArray)
    return std::nullopt;
  const std::vector<std::uint8_t> &s = e.specifics;
  constexpr std::size_t kSize = 0;      // body offset 64
  constexpr std::size_t kNbytes = 8;    // body offset 72
  constexpr std::size_t kGapStart = 16; // body offset 80
  constexpr std::size_t kArgCount = 30; // body offset 94
  constexpr std::size_t kNdimen = 34;   // body offset 98
  if (s.size() <= kNdimen)
    return std::nullopt;
  // Same confirmed constant-byte + 13-reserved-zero-bytes shape as
  // FuncRetSimpleScalarArrayInfo -- see its header comment.
  if (s[kGapStart] != 1)
    return std::nullopt;
  for (std::size_t i = kGapStart + 1; i < kArgCount; ++i)
    if (s[i] != 0)
      return std::nullopt;
  FuncRetStructArrayInfo info;
  info.size = u32(s, kSize);
  info.nbytes = u32(s, kNbytes);
  info.ndimen = s[kNdimen];
  std::uint32_t arg_count = u32(s, kArgCount);
  std::size_t pos = round_up8(kNdimen + 1); // body offset 104 when arg_count == 0
  std::optional<std::vector<NamedRef>> arguments =
      read_named_ref_list(s, pos, static_cast<std::uint16_t>(arg_count));
  if (!arguments.has_value())
    return std::nullopt;
  info.arguments = std::move(*arguments);
  std::optional<NamedRef> type_name = read_named_ref(s, pos);
  if (!type_name.has_value())
    return std::nullopt;
  info.type_name = std::move(*type_name);
  return info;
}

std::optional<std::vector<ArrayBound>>
parse_func_ret_struct_array_bounds(const Entity &e) {
  std::optional<FuncRetStructArrayInfo> info = parse_func_ret_struct_array(e);
  if (!info.has_value() || info->ndimen == 0)
    return std::nullopt;
  const std::vector<std::uint8_t> &s = e.specifics;
  std::size_t pos = round_up8(34 + 1); // body offset 104 when arg_count == 0
  std::optional<std::vector<NamedRef>> skip_args = read_named_ref_list(
      s, pos, static_cast<std::uint16_t>(info->arguments.size()));
  if (!skip_args.has_value())
    return std::nullopt;
  std::optional<NamedRef> skip_type_name = read_named_ref(s, pos);
  if (!skip_type_name.has_value())
    return std::nullopt;
  return read_dimen_bounds(s, pos, info->ndimen);
}

std::optional<SimpleArrayArgInfo> parse_simple_array_arg(const Entity &e) {
  if (e.decl_rec_kind != DeclRecKind::SimpleScalarArrayArg)
    return std::nullopt;
  const std::vector<std::uint8_t> &s = e.specifics;
  constexpr std::size_t kSize = 0;      // body offset 64
  constexpr std::size_t kNbytes = 8;    // body offset 72
  constexpr std::size_t kType = 16;     // body offset 80
  constexpr std::size_t kPassedBy = 18; // body offset 82
  constexpr std::size_t kNdimen = 20;   // body offset 84
  if (s.size() <= kNdimen)
    return std::nullopt;
  SimpleArrayArgInfo info;
  info.size = u32(s, kSize);
  info.nbytes = u32(s, kNbytes);
  info.type = static_cast<ScalarType>(s[kType]);
  info.passed_by = static_cast<PassedBy>(s[kPassedBy]);
  info.ndimen = s[kNdimen];
  return info;
}

std::optional<std::vector<ArrayBound>>
parse_simple_array_arg_bounds(const Entity &e) {
  std::optional<SimpleArrayArgInfo> info = parse_simple_array_arg(e);
  if (!info.has_value() || info->ndimen == 0)
    return std::nullopt;
  const std::vector<std::uint8_t> &s = e.specifics;
  std::size_t pos = 24; // round_up8(85 - 64) -- body offset 88
  return read_dimen_bounds(s, pos, info->ndimen);
}

std::optional<SimpleScalarArrayFieldInfo>
parse_simple_scalar_array_field(const Entity &e) {
  if (e.decl_rec_kind != DeclRecKind::SimpleScalarArrayField)
    return std::nullopt;
  const std::vector<std::uint8_t> &s = e.specifics;
  constexpr std::size_t kSize = 0;    // body offset 64
  constexpr std::size_t kNbytes = 16; // body offset 80
  constexpr std::size_t kType = 28;   // body offset 92
  constexpr std::size_t kNdimen = 29; // body offset 93
  if (s.size() <= kNdimen)
    return std::nullopt;
  SimpleScalarArrayFieldInfo info;
  info.size = u32(s, kSize);
  info.nbytes = u32(s, kNbytes);
  info.type = static_cast<ScalarType>(s[kType]);
  info.ndimen = s[kNdimen];
  return info;
}

std::optional<std::vector<ArrayBound>>
parse_simple_scalar_array_field_bounds(const Entity &e) {
  std::optional<SimpleScalarArrayFieldInfo> info =
      parse_simple_scalar_array_field(e);
  if (!info.has_value() || info->ndimen == 0)
    return std::nullopt;
  const std::vector<std::uint8_t> &s = e.specifics;
  std::size_t pos = 32; // round_up8(94 - 64) -- body offset 96
  return read_dimen_bounds(s, pos, info->ndimen);
}

std::optional<StructArrayFieldInfo> parse_struct_array_field(const Entity &e) {
  if (e.decl_rec_kind != DeclRecKind::StructArrayField)
    return std::nullopt;
  const std::vector<std::uint8_t> &s = e.specifics;
  constexpr std::size_t kSize = 0;    // body offset 64
  constexpr std::size_t kOffset = 8;  // body offset 72
  constexpr std::size_t kNbytes = 16; // body offset 80
  constexpr std::size_t kNdimen = 29; // body offset 93
  constexpr std::size_t kTypeNameStart = 32; // body offset 96
  if (s.size() < kTypeNameStart)
    return std::nullopt;
  // Only rank 1 is understood past this point -- same caveat as
  // StructArray (see parse_struct_array()'s comment).
  std::uint8_t ndimen = s[kNdimen];
  if (ndimen != 1)
    return std::nullopt;
  StructArrayFieldInfo info;
  info.size = u32(s, kSize);
  info.offset = u32(s, kOffset);
  info.nbytes = u32(s, kNbytes);
  info.ndimen = ndimen;
  std::size_t pos = kTypeNameStart;
  std::optional<NamedRef> type_name = read_named_ref(s, pos);
  if (!type_name.has_value())
    return std::nullopt;
  info.type_name = std::move(*type_name);
  return info;
}

std::optional<std::vector<ArrayBound>>
parse_struct_array_field_bounds(const Entity &e) {
  std::optional<StructArrayFieldInfo> info = parse_struct_array_field(e);
  if (!info.has_value() || info->ndimen == 0)
    return std::nullopt;
  const std::vector<std::uint8_t> &s = e.specifics;
  std::size_t pos = 32; // round_up8(94 - 64) -- body offset 96
  std::optional<NamedRef> skip_type_name = read_named_ref(s, pos);
  if (!skip_type_name.has_value())
    return std::nullopt;
  return read_dimen_bounds(s, pos, info->ndimen);
}

std::optional<StructArrayArgInfo> parse_struct_array_arg(const Entity &e) {
  if (e.decl_rec_kind != DeclRecKind::StructArrayArg)
    return std::nullopt;
  const std::vector<std::uint8_t> &s = e.specifics;
  constexpr std::size_t kSize = 0;      // body offset 64
  constexpr std::size_t kNbytes = 8;    // body offset 72
  constexpr std::size_t kNdimen = 19;   // body offset 83
  constexpr std::size_t kPassedBy = 20; // body offset 84
  constexpr std::size_t kDimInfoStart = 24; // body offset 88
  if (s.size() < kDimInfoStart)
    return std::nullopt;
  // Only rank 1 is understood past this point -- same caveat as
  // StructArray.
  std::uint8_t ndimen = s[kNdimen];
  if (ndimen != 1)
    return std::nullopt;
  StructArrayArgInfo info;
  info.size = u32(s, kSize);
  info.nbytes = u32(s, kNbytes);
  info.passed_by = static_cast<PassedBy>(s[kPassedBy]);
  info.ndimen = ndimen;
  std::size_t pos = kDimInfoStart;
  if (!read_dimen_bounds(s, pos, ndimen).has_value() ||
      !skip_dim_chain(s, pos, ndimen))
    return std::nullopt;
  constexpr std::size_t kTrailingGap = 16;
  if (pos + kTrailingGap > s.size())
    return std::nullopt;
  pos += kTrailingGap;
  std::optional<NamedRef> type_name = read_named_ref(s, pos);
  if (!type_name.has_value())
    return std::nullopt;
  info.type_name = std::move(*type_name);
  return info;
}

std::optional<std::vector<ArrayBound>>
parse_struct_array_arg_bounds(const Entity &e) {
  std::optional<StructArrayArgInfo> info = parse_struct_array_arg(e);
  if (!info.has_value() || info->ndimen == 0)
    return std::nullopt;
  const std::vector<std::uint8_t> &s = e.specifics;
  std::size_t pos = 24; // body offset 88
  return read_dimen_bounds(s, pos, info->ndimen);
}

std::optional<CharArrayArgInfo> parse_char_array_arg(const Entity &e) {
  if (e.decl_rec_kind != DeclRecKind::CharArrayArg)
    return std::nullopt;
  const std::vector<std::uint8_t> &s = e.specifics;
  constexpr std::size_t kSize = 0;    // body offset 64
  constexpr std::size_t kLength = 8;  // body offset 72
  constexpr std::size_t kNdimen = 20; // body offset 84
  if (s.size() <= kNdimen)
    return std::nullopt;
  CharArrayArgInfo info;
  info.size = u32(s, kSize);
  info.length = u32(s, kLength);
  info.ndimen = s[kNdimen];
  return info;
}

std::optional<std::vector<ArrayBound>>
parse_char_array_arg_bounds(const Entity &e) {
  std::optional<CharArrayArgInfo> info = parse_char_array_arg(e);
  if (!info.has_value() || info->ndimen == 0)
    return std::nullopt;
  const std::vector<std::uint8_t> &s = e.specifics;
  std::size_t pos = 24; // body offset 88
  return read_dimen_bounds(s, pos, info->ndimen);
}

std::optional<SimpleScalarArrayCommonInfo>
parse_simple_scalar_array_common(const Entity &e) {
  if (e.decl_rec_kind != DeclRecKind::SimpleScalarArrayCommon)
    return std::nullopt;
  const std::vector<std::uint8_t> &s = e.specifics;
  constexpr std::size_t kSize = 0;    // body offset 64
  constexpr std::size_t kNbytes = 8;  // body offset 72
  constexpr std::size_t kOffset = 16; // body offset 80
  constexpr std::size_t kNdimen = 24; // body offset 88
  constexpr std::size_t kType = 25;   // body offset 89
  if (s.size() <= kType)
    return std::nullopt;
  SimpleScalarArrayCommonInfo info;
  info.size = u32(s, kSize);
  info.nbytes = u32(s, kNbytes);
  info.offset = u32(s, kOffset);
  info.ndimen = s[kNdimen];
  info.type = static_cast<ScalarType>(s[kType]);
  return info;
}

std::optional<std::vector<ArrayBound>>
parse_simple_scalar_array_common_bounds(const Entity &e) {
  std::optional<SimpleScalarArrayCommonInfo> info =
      parse_simple_scalar_array_common(e);
  if (!info.has_value() || info->ndimen == 0)
    return std::nullopt;
  const std::vector<std::uint8_t> &s = e.specifics;
  std::size_t pos = 32; // body offset 96
  return read_dimen_bounds(s, pos, info->ndimen);
}

std::optional<StructArrayCommonInfo> parse_struct_array_common(const Entity &e) {
  if (e.decl_rec_kind != DeclRecKind::StructArrayCommon)
    return std::nullopt;
  const std::vector<std::uint8_t> &s = e.specifics;
  constexpr std::size_t kSize = 0;    // body offset 64
  constexpr std::size_t kNbytes = 8;  // body offset 72
  constexpr std::size_t kOffset = 16; // body offset 80
  constexpr std::size_t kNdimen = 25; // body offset 89
  if (s.size() <= kNdimen)
    return std::nullopt;
  // Only rank 1 is understood past this point -- same caveat as
  // StructArray.
  std::uint8_t ndimen = s[kNdimen];
  if (ndimen != 1)
    return std::nullopt;
  StructArrayCommonInfo info;
  info.size = u32(s, kSize);
  info.nbytes = u32(s, kNbytes);
  info.offset = u32(s, kOffset);
  info.ndimen = ndimen;
  std::size_t pos = 32; // body offset 96
  if (!read_dimen_bounds(s, pos, ndimen).has_value() ||
      !skip_dim_chain(s, pos, ndimen))
    return std::nullopt;
  constexpr std::size_t kTrailingGap = 16;
  if (pos + kTrailingGap > s.size())
    return std::nullopt;
  pos += kTrailingGap;
  std::optional<NamedRef> type_name = read_named_ref(s, pos);
  if (!type_name.has_value())
    return std::nullopt;
  info.type_name = std::move(*type_name);
  return info;
}

std::optional<std::vector<ArrayBound>>
parse_struct_array_common_bounds(const Entity &e) {
  std::optional<StructArrayCommonInfo> info = parse_struct_array_common(e);
  if (!info.has_value() || info->ndimen == 0)
    return std::nullopt;
  const std::vector<std::uint8_t> &s = e.specifics;
  std::size_t pos = 32; // body offset 96
  return read_dimen_bounds(s, pos, info->ndimen);
}

std::optional<CharArrayCommonInfo> parse_char_array_common(const Entity &e) {
  if (e.decl_rec_kind != DeclRecKind::CharArrayCommon)
    return std::nullopt;
  const std::vector<std::uint8_t> &s = e.specifics;
  constexpr std::size_t kSize = 0;    // body offset 64
  constexpr std::size_t kLength = 8;  // body offset 72
  constexpr std::size_t kOffset = 16; // body offset 80
  constexpr std::size_t kNdimen = 24; // body offset 88
  if (s.size() <= kNdimen)
    return std::nullopt;
  CharArrayCommonInfo info;
  info.size = u32(s, kSize);
  info.length = u32(s, kLength);
  info.offset = u32(s, kOffset);
  info.ndimen = s[kNdimen];
  return info;
}

std::optional<std::vector<ArrayBound>>
parse_char_array_common_bounds(const Entity &e) {
  std::optional<CharArrayCommonInfo> info = parse_char_array_common(e);
  if (!info.has_value() || info->ndimen == 0)
    return std::nullopt;
  const std::vector<std::uint8_t> &s = e.specifics;
  std::size_t pos = 32; // body offset 96
  return read_dimen_bounds(s, pos, info->ndimen);
}

std::optional<std::vector<ArrayBound>>
parse_char_array_field_bounds(const Entity &e) {
  std::optional<CharArrayFieldInfo> info = parse_char_array_field(e);
  if (!info.has_value() || info->ndimen == 0)
    return std::nullopt;
  const std::vector<std::uint8_t> &s = e.specifics;
  std::size_t pos = 32; // body offset 96
  return read_dimen_bounds(s, pos, info->ndimen);
}

bool is_supported_major_id(int major_id) {
  // Only version this reader has actually been checked against real bytes
  // for: ifx 2026.0.0, confirmed via two independent sample .mod files
  // cross-checked against the vendor's own dump tool output. The
  // compiler itself accepts a wider range (9-13, "obsolete" below 9,
  // "not yet supported" at 14+) but that's evidence about what one
  // compiler build tolerates, not confirmation that this decoder's
  // assumptions (byte layout, field offsets) hold for those other
  // versions -- so it isn't claimed as supported until checked the same
  // way "13" was.
  return major_id == 13;
}

bool looks_like_ifx_module(const std::string &path) {
  std::ifstream f(path, std::ios::binary);
  if (!f)
    return false;
  std::vector<char> buf(kMagicOffset + 8);
  f.read(buf.data(), static_cast<std::streamsize>(buf.size()));
  if (static_cast<std::size_t>(f.gcount()) != buf.size())
    return false;
  return std::memcmp(buf.data() + kMagicOffset, kMagic, 8) == 0;
}

Module read_module(const std::string &path) {
  std::vector<std::uint8_t> d = read_file(path);
  if (d.size() < kHeaderSize)
    throw fxmod::Error(path + ": too short to be an ifx module (no full "
                                "header)");
  if (std::memcmp(d.data() + kMagicOffset, kMagic, 8) != 0)
    throw fxmod::Error(path + ": not an ifx module (bad magic)");

  int major_id = u16(d, 0);
  int minor_id = u16(d, 2);
  std::uint32_t map_start_pos = u32(d, 4);
  std::uint32_t num_of_entities = u32(d, 8);
  std::uint32_t target_arch = u32(d, 20);
  std::uint32_t compiler_edit = u32(d, 24);
  std::string gemver = fixed_cstr(d, 28, 8);
  std::string compiler_ver = fixed_cstr(d, 36, 12);

  if (!is_supported_major_id(major_id)) {
    throw UnsupportedError(
        path + ": ifx module major_id " + std::to_string(major_id) +
        " has not been verified against real files (known good: 13, from "
        "ifx 2026.0.0). Extending this means checking a real sample "
        "compiled by that version against the byte offsets documented in "
        "src/ifx/module.hpp, the same way \"13\" was.");
  }

  if (map_start_pos > d.size())
    throw fxmod::Error(path + ": map_start_pos points past end of file");

  Module mod;
  mod.path = path;
  mod.major_id = major_id;
  mod.minor_id = minor_id;
  mod.target_arch = target_arch;
  mod.compiler_edit = compiler_edit;
  mod.gemver = std::move(gemver);
  mod.compiler_ver = std::move(compiler_ver);

  std::size_t off = map_start_pos;
  std::uint64_t total_directory_bytes = 0;
  for (std::uint32_t i = 0; i < num_of_entities; ++i) {
    if (off + 16 > d.size())
      throw fxmod::Error(path + ": entity directory record " +
                           std::to_string(i) + " runs past end of file");
    std::uint32_t total_len = u32(d, off);
    std::uint32_t body_offset = u32(d, off + 4);
    std::uint32_t body_len = u32(d, off + 8);
    if (total_len < 16 || off + total_len > d.size())
      throw fxmod::Error(path + ": entity directory record " +
                           std::to_string(i) + " has an invalid length");

    std::size_t name_start = off + 16;
    std::size_t name_end = off + total_len; // includes the NUL
    std::size_t nul = name_start;
    while (nul < name_end && d[nul] != 0)
      ++nul;
    if (nul == name_end)
      throw fxmod::Error(path + ": entity directory record " +
                           std::to_string(i) + " name is not NUL-terminated");

    if (static_cast<std::uint64_t>(body_offset) + body_len > d.size())
      throw fxmod::Error(path + ": entity '" +
                           std::string(reinterpret_cast<const char *>(
                                           d.data() + name_start),
                                       nul - name_start) +
                           "' body runs past end of file");
    if (body_len < 4)
      throw fxmod::Error(
          path + ": entity body too short to hold even decl_rec_kind -- "
                 "format assumption violated, refusing to guess");

    Entity e;
    e.name = std::string(
        reinterpret_cast<const char *>(d.data() + name_start), nul - name_start);
    e.decl_rec_kind = static_cast<DeclRecKind>(u32(d, body_offset + 0));
    // The FileName entity every module starts with does not have the
    // common envelope below -- its body is only 24-28 bytes
    // (decl_rec_kind + an unlabeled reserved u32 + two NUL-terminated
    // strings: the source file name and the module name). Everything
    // else observed so far does carry the full envelope. Rather than
    // assume every future kind does too, only read past decl_rec_kind
    // when the body is actually long enough to hold it.
    if (body_len >= 64) {
      e.flags = u64(d, body_offset + 8);
      e.sym_id = u32(d, body_offset + 56);
      e.klass = static_cast<SymbolClass>(d[body_offset + 62]);
      e.skind = static_cast<SymbolKind>(d[body_offset + 63]);
      e.specifics.assign(d.begin() + body_offset + 64,
                          d.begin() + body_offset + body_len);
    } else {
      e.specifics.assign(d.begin() + body_offset + 4,
                          d.begin() + body_offset + body_len);
    }
    mod.entities.push_back(std::move(e));

    off += total_len;
    total_directory_bytes += total_len;
  }

  if (off != d.size()) {
    throw fxmod::Error(
        path + ": entity directory does not exactly span the rest of the "
               "file (consumed " +
        std::to_string(total_directory_bytes) + " bytes from offset " +
        std::to_string(map_start_pos) + ", file is " +
        std::to_string(d.size()) +
        " bytes) -- format assumption violated, refusing to guess");
  }

  return mod;
}

} // namespace fxmod::ifx
