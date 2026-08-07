add_executable(json_unified json_unified.cpp)
target_link_libraries(json_unified PRIVATE glzdb::glzdb)
target_compile_features(json_unified PUBLIC cxx_std_23)

add_executable(json_partitioned json_partitioned.cpp)
target_link_libraries(json_partitioned PRIVATE glzdb::glzdb)
target_compile_features(json_partitioned PUBLIC cxx_std_23)

add_executable(todos todos.cpp)
target_link_libraries(todos PRIVATE glzdb::glzdb)
target_compile_features(todos PUBLIC cxx_std_23)
