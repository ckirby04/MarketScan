# The USB port.
#
# Each subdirectory produces one DLL in <runtime>/theories, which the host
# discovers at startup. Nothing in the host names any of them.

add_subdirectory(buy_and_hold)
add_subdirectory(sma_crossover)

# The parity fixture. It reads its signal out of the volume column, which only
# research/parity.py writes anything meaningful into, so on real data it stands
# down rather than ranking on turnover. Shipped as a real theory because the
# M9 parity test should exercise the same binary the host would load.
add_subdirectory(decile_parity)

# The skeleton is compiled here so that CI notices if it stops building, but
# it is written somewhere the host does not scan: an empty template appearing
# on the leaderboard as a flat line would be noise, not information.
add_subdirectory(_template)
