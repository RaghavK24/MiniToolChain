# The benchmark suite is a plain executable, not a test: it is run on demand and
# its output is a report, not a pass/fail.
add_executable(bench_toolchain bench_toolchain.cpp)
target_link_libraries(bench_toolchain PRIVATE minitool::core)
