// The v1 reference plugin's twin, declaring ABI v2.
//
// Identical behaviour by construction -- both include reference_body.hpp and
// neither has any logic of its own -- and the only differences are the version
// each declares and the tenth vtable slot this one fills.
//
// That is what makes the comparison between them evidence rather than a
// coincidence. Two separately written plugins producing the same numbers would
// prove only that two people wrote similar code; one body exported twice proves
// that the version branch changes nothing except what it is supposed to.

#include "reference_body.hpp"

namespace {

/// This one tracks the header on purpose, unlike its v1 twin: it is meant to be
/// whatever the current version is, and to start failing the day the current
/// version adds a slot it does not fill.
constexpr std::uint32_t kCurrent = MS_ABI_VERSION;

}  // namespace

extern "C" {

MS_EXPORT uint32_t ms_plugin_abi_version(void) {
    return kCurrent;
}

MS_EXPORT const MS_StrategyDesc* ms_plugin_describe(void) {
    static const MS_StrategyDesc desc = {kCurrent,
                                         "v2_reference",
                                         "ABI v2 Reference",
                                         "test",
                                         "The v1 reference's twin, at the current ABI.",
                                         nullptr,
                                         0,
                                         nullptr,
                                         0};
    return &desc;
}

MS_EXPORT void* ms_plugin_create(MS_StrategyVTable* out_vtable) {
    if (out_vtable == nullptr) {
        return nullptr;
    }
    reference_body::fill_v1_slots(out_vtable);
    // The one slot that differs.
    out_vtable->on_order_status = &reference_body::on_order_status;
    return new (std::nothrow) reference_body::State{};
}

}  // extern "C"
