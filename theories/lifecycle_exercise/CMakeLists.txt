# NOT A THEORY, and therefore built where the host does not scan -- the same
# treatment decile_parity and _template get, for the same reason.
#
# It exists to make a venue produce every order state it can, so that the code
# handling those states has been run against a real one. It has no edge and
# loses money by design. On the leaderboard it would appear as a theory that
# trades constantly and bleeds, which is not information -- it is this file's
# purpose being mistaken for a result.
#
# ms_live loads it by path. Nothing discovers it.
marketscan_add_theory(lifecycle_exercise
    SOURCES lifecycle_exercise.cpp
    OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lifecycle_theory")
