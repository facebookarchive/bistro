add_gtest(test_parallel_processor )  # No deps; update_cmake.sh needs the space
add_gtest(test_settings_map lib_bistro_utils)
add_gtest(test_symbol_table lib_bistro_utils)
add_gtest(test_shuffled_range lib_bistro_utils)
add_gtest(test_background_thread lib_bistro_utils)
add_gtest(test_log_writer lib_bistro_utils)
add_gtest(test_process_runner lib_bistro_utils)
add_gtest(test_killable_subprocess lib_bistro_utils)
add_gtest(test_shell lib_bistro_utils)
