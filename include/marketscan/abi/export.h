/*
 * MarketScan -- plugin export macros.
 *
 * Pure C. Include this from plugin_abi.h only.
 *
 * MS_EXPORT marks the three symbols a theory plugin must expose. It expands to
 * dllexport when a plugin is being compiled (MARKETSCAN_PLUGIN_BUILD, set by
 * the marketscan_add_theory CMake helper) and to nothing otherwise.
 *
 * It deliberately never expands to dllimport: the host resolves plugin symbols
 * with GetProcAddress at runtime and never links against a plugin, so an
 * import declaration would be a lie that costs a link error.
 */
#ifndef MARKETSCAN_ABI_EXPORT_H
#define MARKETSCAN_ABI_EXPORT_H

#if defined(_WIN32)
#    define MS_DLL_EXPORT __declspec(dllexport)
#elif defined(__GNUC__) || defined(__clang__)
#    define MS_DLL_EXPORT __attribute__((visibility("default")))
#else
#    define MS_DLL_EXPORT
#endif

#if defined(MARKETSCAN_PLUGIN_BUILD)
#    define MS_EXPORT MS_DLL_EXPORT
#else
#    define MS_EXPORT
#endif

#endif /* MARKETSCAN_ABI_EXPORT_H */
