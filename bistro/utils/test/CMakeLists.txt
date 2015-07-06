add_gtest(test_parallel_processor )  # No deps; update_cmake.sh needs the space
add_gtest(test_settings_map bistro_utils)
add_gtest(test_symbol_table bistro_utils)
add_gtest(test_shuffled_range bistro_utils)
add_gtest(test_background_thread bistro_utils)
add_gtest(test_log_writer bistro_utils)
add_gtest(test_process_runner bistro_utils)
add_gtest(test_killable_subprocess bistro_utils)
add_gtest(test_shell bistro_utils)
