/* Stub for the snap-packaged libmgba-dev headers, which ship
 * mgba/dllexports.h but omit the CMake-generated mgba-util/dllexports.h
 * companion. Pure build-time shim (visibility macros only) so this
 * project's headless GBA reference driver (tools/gba-refcheck/) can build
 * against the installed snap; does not touch mGBA's own source/behavior.
 * See docs/PLAN.md §4.3 item 8. */
#ifndef MGBA_UTIL_EXPORT_H
#define MGBA_UTIL_EXPORT_H

#ifdef BUILD_STATIC
#  define MGBA_UTIL_EXPORT
#  define MGBA_UTIL_NO_EXPORT
#else
#  ifndef MGBA_UTIL_EXPORT
#    define MGBA_UTIL_EXPORT __attribute__((visibility("default")))
#  endif
#  ifndef MGBA_UTIL_NO_EXPORT
#    define MGBA_UTIL_NO_EXPORT __attribute__((visibility("hidden")))
#  endif
#endif

#endif
