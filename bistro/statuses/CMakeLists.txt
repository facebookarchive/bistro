add_library(
  bistro_statuses STATIC
  TaskStatus.cpp
  TaskStatusSnapshot.cpp
  TaskStatuses.cpp
  SQLiteTaskStore.cpp
)
bistro_link_libraries(
  bistro_statuses
  bistro_config
  bistro_sqlite
)

add_subdirectory(test)
