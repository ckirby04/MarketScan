#include <string_view>

#include <catch2/catch_test_macros.hpp>

#include "marketscan/util/version.hpp"

// Guards the build plumbing itself: the generated version header is present
// and configured, and marketscan_core links. Everything else in this suite
// tests behaviour; this one test fails when the build tree is wrong.
TEST_CASE("version metadata is configured", "[build]") {
    using namespace marketscan::version;

    STATIC_REQUIRE(kMajor >= 0);
    STATIC_REQUIRE(kMinor >= 0);
    STATIC_REQUIRE(kPatch >= 0);

    REQUIRE(kString != nullptr);
    REQUIRE(std::string_view{kString}.find('@') == std::string_view::npos);
    REQUIRE_FALSE(std::string_view{kCompiler}.empty());
    REQUIRE_FALSE(std::string_view{kBuildType}.empty());
}
