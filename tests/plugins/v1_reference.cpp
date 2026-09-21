// A theory that speaks ABI v1, after the host moved to v2.
//
// The migration promise, as something that runs: **a v1 plugin keeps loading and
// keeps behaving byte-identically, and simply never receives the newer
// callback.** `SPEC_2_M1B.md` §4 built the version-tolerant loader for this day,
// and a promise nobody exercises is a promise nobody has kept.
//
// WHY THIS IS SOURCE RATHER THAN A COMMITTED DLL
//
// The obvious way to prove a v1 plugin still loads is to commit one, built back
// when v1 was current. That has two problems: a binary in the tree that nobody
// can rebuild is a fixture nobody can audit, and it would bind the test to one
// compiler and one CRT forever.
//
// So this declares v1 by hand instead. It exports the three symbols itself and
// fills exactly the nine slots v1 had, ignoring the tenth entirely -- which is
// precisely what a DLL compiled against the old header does. The difference
// between "compiled against v1" and "declares v1 and writes nine slots" is
// nothing the host can observe, because nine slots is all v1 ever was.
//
// Its behaviour lives in reference_body.hpp, shared with the v2 twin, so the two
// cannot differ in anything but their declared version. That is what makes the
// comparison between them evidence.
//
// It deliberately does NOT use MARKETSCAN_EXPORT_STRATEGY: that macro reports
// MS_ABI_VERSION, which is 2 now. Writing the exports out longhand is the point.

#include "reference_body.hpp"

namespace {

/// The ABI version this plugin was written against, and still speaks.
///
/// A literal, not MS_ABI_VERSION. The moment this tracks the header the fixture
/// stops testing anything: it would silently become a v3 plugin the day v3
/// landed and would prove only that the current version loads.
constexpr std::uint32_t kV1 = 1u;

}  // namespace

extern "C" {

MS_EXPORT uint32_t ms_plugin_abi_version(void) {
    return kV1;
}

MS_EXPORT const MS_StrategyDesc* ms_plugin_describe(void) {
    static const MS_StrategyDesc desc = {kV1,
                                         "v1_reference",
                                         "ABI v1 Reference",
                                         "test",
                                         "Speaks ABI v1 after the host moved to v2.",
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
    // on_order_status is NOT touched. A v1 plugin does not know it exists.
    return new (std::nothrow) reference_body::State{};
}

}  // extern "C"
