// The test that is supposed to fail.
//
// "All green" is only information if red is reachable. A suite where every test
// passes and a suite where the harness has quietly stopped evaluating assertions
// look exactly the same from the outside, and this project has twice shipped a
// check that had stopped checking -- once because a fixture theory appeared in a
// scanned directory, once because an empty directory turned a skip into a
// measurement of nothing.
//
// So one assertion here is false on purpose. CTest runs this binary and requires
// its output to contain the failure, which means the pass condition for the
// whole suite includes *Catch2 having evaluated a false expression and reported
// it*. If the harness ever stops being able to fail, this stops being able to
// pass.
//
// It is a separate binary rather than a tagged case in an existing one because a
// deliberate failure inside a normal test binary makes that binary exit non-zero,
// which is indistinguishable from a real regression to everything downstream --
// including `scripts/check_skips.py`.
//
// DO NOT "FIX" THE ASSERTION BELOW. Deleting it removes the only evidence that
// the rest of the suite means anything.

#include <catch2/catch_test_macros.hpp>

TEST_CASE("the harness can still detect a true assertion", "[negative-control]") {
    // The control for the control: if this failed too, the binary would be
    // failing for some reason other than the one intended.
    CHECK(1 + 1 == 2);
}

TEST_CASE("the harness can still detect a false assertion", "[negative-control]") {
    // Deliberately false. The string below is what tests/CMakeLists.txt matches
    // on, so changing it without changing the CMake will turn this from a
    // working control into a silent pass -- which is the failure this file
    // exists to make impossible.
    const int expected = 1;
    const int measured = 2;
    INFO("NEGATIVE CONTROL: this failure is deliberate and proves the harness works");
    CHECK(measured == expected);
}
