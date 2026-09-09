#pragma once

#include <optional>
#include <string_view>
#include <version>

// std::from_chars' floating-point overloads cannot be called on every platform
// score builds for: libc++ deletes them where its runtime has no support, and
// Apple's availability annotations gate them behind a macOS newer than the
// deployment target. The integer overloads are available everywhere. Same
// guard, and same reasoning, as score-plugin-avnd/AvndProcesses/
// ValueSerialization.hpp. Definable from the build to exercise the fallback on
// a platform that does not need it.
//
// ossia::parse_strict is the natural fit for the semantics below, but it keys
// on __cpp_lib_to_chars alone, which libc++ defines while deleting the very
// overloads this needs.
#if !defined(SCORE_HAS_STD_FLOAT_FROM_CHARS)
#if defined(__cpp_lib_to_chars) && !defined(_LIBCPP_VERSION)
#define SCORE_HAS_STD_FLOAT_FROM_CHARS 1
#else
#define SCORE_HAS_STD_FLOAT_FROM_CHARS 0
#endif
#endif

#if SCORE_HAS_STD_FLOAT_FROM_CHARS
#include <charconv>
#else
#include <boost/lexical_cast/try_lexical_convert.hpp>

#include <iomanip>
#include <limits>
#include <locale>
#include <sstream>
#endif

#include <string>

namespace Lavfi
{
/**
 * @brief Read a metadata value as a number, or report that it is not one.
 *
 * Metadata arrives as text and is published as a number where it reads as one.
 * The whole string must parse, so "1.5 dB" stays text.
 *
 * Boost differs from from_chars in two ways that are corrected here, so the
 * two implementations cannot disagree about which tokens are valid: it accepts
 * a leading '+', and it returns zero for a token that underflows where
 * from_chars fails outright.
 */
inline std::optional<double> parseNumber(std::string_view text) noexcept
{
  if(text.empty())
    return std::nullopt;

  double out{};
#if SCORE_HAS_STD_FLOAT_FROM_CHARS
  const auto* const last = text.data() + text.size();
  const auto parsed = std::from_chars(text.data(), last, out);
  if(parsed.ec != std::errc{} || parsed.ptr != last)
    return std::nullopt;
#else
  if(text.starts_with('+'))
    return std::nullopt;
  if(!boost::conversion::try_lexical_convert(text, out))
    return std::nullopt;
  if(out == 0.)
  {
    const auto mantissa = text.substr(0, text.find_first_of("eE"));
    if(mantissa.find_first_of("123456789") != std::string_view::npos)
      return std::nullopt;
  }
#endif
  return out;
}

/**
 * @brief Render a float as text that reads back as the same value.
 *
 * std::to_chars gives the shortest such text and ignores the locale, but its
 * floating-point overloads carry the same availability problem as from_chars --
 * on Apple they are annotated as introduced in macOS 13.3, past the deployment
 * target. The fallback asks for max_digits10 through a classic-locale stream:
 * not the shortest form, but it round-trips, and the decimal separator does not
 * follow LC_NUMERIC the way std::to_string would.
 */
inline std::string formatNumber(float f)
{
#if SCORE_HAS_STD_FLOAT_FROM_CHARS
  char buf[64];
  const auto [p, ec] = std::to_chars(buf, buf + sizeof(buf), f);
  if(ec == std::errc{})
    return std::string(buf, p);
  return std::to_string(f);
#else
  // Shortest text that still reads back as f, so the two branches agree.
  // max_digits10 alone would always print nine significant digits and turn 0.1
  // into "0.100000001"; taking the FIRST precision that round-trips is not
  // enough either, because the stream switches to scientific at low precision
  // and would render 16000 as "1.6e+04". to_chars picks the shortest form, so
  // this keeps the shortest round-tripping candidate rather than the first.
  std::string best;
  for(int prec = 1; prec <= std::numeric_limits<float>::max_digits10; ++prec)
  {
    std::ostringstream os;
    os.imbue(std::locale::classic());
    os << std::setprecision(prec) << f;
    const auto candidate = os.str();

    float back{};
    std::istringstream is(candidate);
    is.imbue(std::locale::classic());
    is >> back;
    if(!is || back != f)
      continue;

    if(best.empty() || candidate.size() < best.size())
      best = candidate;
  }
  if(!best.empty())
    return best;

  std::ostringstream os;
  os.imbue(std::locale::classic());
  os << std::setprecision(std::numeric_limits<float>::max_digits10) << f;
  return os.str();
#endif
}
}
