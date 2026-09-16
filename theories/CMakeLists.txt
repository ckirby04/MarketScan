# The USB port.
#
# Each subdirectory produces one DLL in <runtime>/theories, which the host
# discovers at startup. Nothing in the host names any of them.

add_subdirectory(buy_and_hold)

# The skeleton is compiled here so that CI notices if it stops building, but
# it is written somewhere the host does not scan: an empty template appearing
# on the leaderboard as a flat line would be noise, not information.
add_subdirectory(_template)
