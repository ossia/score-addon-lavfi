#pragma once

/**
 * @file Export.hpp
 * @brief Visibility for the symbols the tests link against.
 *
 * Where score builds its plugins as shared libraries with hidden visibility
 * (the sanitizer build does), a test that links the plugin only sees what is
 * marked. CMake generates the macro next to the plugin; the headless tests
 * compile these sources directly and have no plugin to import from, so they
 * fall back to nothing.
 */
#if __has_include(<score_addon_lavfi_export.h>)
#include <score_addon_lavfi_export.h>
#else
#define SCORE_ADDON_LAVFI_EXPORT
#endif
