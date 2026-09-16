// A plugin with a blank id.
//
// Nothing can be keyed on an empty id, so the loader refuses rather than
// inventing one.

#include "marketscan/abi/plugin_abi.h"

extern "C" {

MS_EXPORT uint32_t ms_plugin_abi_version(void) {
    return MS_ABI_VERSION;
}

MS_EXPORT const MS_StrategyDesc* ms_plugin_describe(void) {
    static const MS_StrategyDesc desc = {
        MS_ABI_VERSION, "", "No Id", "test", "", nullptr, 0, nullptr, 0};
    return &desc;
}

MS_EXPORT void* ms_plugin_create(MS_StrategyVTable*) {
    return nullptr;
}

}  // extern "C"
