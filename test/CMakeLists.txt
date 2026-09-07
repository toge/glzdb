# wasm32 (wasip1/wasip2): Catch2 はビルドできないため素の main のスモークのみ
if(CMAKE_SYSTEM_PROCESSOR STREQUAL "wasm32")
  add_executable(smoke_wasi smoke_wasi.cpp)
  target_link_libraries(smoke_wasi PRIVATE glzdb::glzdb)
  target_compile_features(smoke_wasi PUBLIC cxx_std_23)

  # wasm バイナリは直接実行できないため wasmtime 経由で実行する。
  # smoke は相対パス smoke_wasi.json に読み書きするためカレントの preopen が必要。
  find_program(WASMTIME_EXECUTABLE wasmtime)
  if(WASMTIME_EXECUTABLE)
    add_test(NAME smoke_wasi COMMAND ${WASMTIME_EXECUTABLE} run --dir=. smoke_wasi)
    set_tests_properties(smoke_wasi PROPERTIES WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
  else()
    message(STATUS "wasmtime not found: run manually: wasmtime run --dir=<bindir> ./smoke_wasi")
  endif()
else()
  find_package(Catch2 REQUIRED CONFIG)

  add_executable(all_test test_glzdb.cpp)

  target_link_libraries(all_test PRIVATE
    glzdb::glzdb
    Catch2::Catch2
    Catch2::Catch2WithMain
  )
  target_compile_features(all_test PUBLIC cxx_std_23)

  add_test(NAME all_test COMMAND all_test -r junit)
endif()
