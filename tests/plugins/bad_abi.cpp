// A plugin that exports everything correctly and speaks the wrong ABI.
//
// This is the failure the version check exists for, and it is the dangerous
// one: every export resolves, so without the check the host would go on to
// read a descriptor whose fields are at different offsets than it expects.

#include "marketscan/abi/plugin_abi.h"

extern "C" {

MS_EXPORT uint32_t ms_plugin_abi_version(void) {
    return MS_ABI_VERSION + 1000u;
}

MS_EXPORT const MS_StrategyDesc* ms_plugin_describe(void) {
    static const MS_StrategyDesc desc = {
        MS_ABI_VERSION + 1000u, "bad_abi", "Wrong ABI", "test", "", nullptr, 0, nullptr, 0};
    return &desc;
}

MS_EXPORT void* ms_plugin_create(MS_StrategyVTable*) {
    return nullptr;
}

}  // extern "C"
