// A plugin whose constructor refuses.
//
// Exports are all present and the ABI matches, but create returns null --
// which is what MARKETSCAN_EXPORT_STRATEGY does when a theory's constructor
// throws. The host must report it and stay up.

#include "marketscan/abi/plugin_abi.h"

extern "C" {

MS_EXPORT uint32_t ms_plugin_abi_version(void) {
    return MS_ABI_VERSION;
}

MS_EXPORT const MS_StrategyDesc* ms_plugin_describe(void) {
    static const MS_StrategyDesc desc = {
        MS_ABI_VERSION, "null_create", "Refuses To Construct", "test", "", nullptr, 0, nullptr, 0};
    return &desc;
}

MS_EXPORT void* ms_plugin_create(MS_StrategyVTable*) {
    return nullptr;
}

}  // extern "C"
