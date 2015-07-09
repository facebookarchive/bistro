add_library(
  lib_bistro_statuses STATIC
  TaskStatus.cpp
  TaskStatusSnapshot.cpp
  TaskStatuses.cpp
  SQLiteTaskStore.cpp
)
bistro_link_libraries(
  lib_bistro_statuses
  lib_bistro_config
  lib_bistro_sqlite
)

add_subdirectory(test)
