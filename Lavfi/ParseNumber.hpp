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
#endif

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
}
