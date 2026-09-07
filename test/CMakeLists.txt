find_package(Catch2 REQUIRED CONFIG)

add_executable(all_test test_glzdb.cpp)

target_link_libraries(all_test PRIVATE
  glzdb::glzdb
  Catch2::Catch2
  Catch2::Catch2WithMain
)
target_compile_features(all_test PUBLIC cxx_std_23)

add_test(NAME all_test COMMAND all_test -r junit)
