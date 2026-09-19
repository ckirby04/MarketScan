# A fixture, not a theory, and therefore built somewhere the host does not scan
# -- the same treatment _template gets, for the same reason.
#
# It reads its signal out of the volume column and stands down on any bar whose
# volume is not an encoded exposure, so on real data it would appear on the
# leaderboard as a flat line that never traded. Worse, it broke two determinism
# tests by existing: one asserts the reference theories are exactly two, and one
# requires every theory to show less slippage under optimistic fills, which a
# theory that never trades cannot do.
marketscan_add_theory(decile_parity
    SOURCES decile_parity.cpp
    OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/parity_theory")
