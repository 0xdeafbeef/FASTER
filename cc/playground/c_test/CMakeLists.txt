set(C_TEST_HEADERS
  c_test.h
)

add_executable(c_test ${C_TEST_HEADERS} c_test.cc)
target_link_libraries(c_test ${FASTER_BENCHMARK_LINK_LIBS})

