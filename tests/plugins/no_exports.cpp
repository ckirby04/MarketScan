// A DLL that is not a theory.
//
// Stands in for whatever else ends up in the folder: a dependency, something
// copied there by mistake, an unrelated library. The loader must say so and
// carry on.

extern "C" __declspec(dllexport) int marketscan_test_not_a_theory(void) {
    return 42;
}
