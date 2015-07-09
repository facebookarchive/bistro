add_library(
  lib_bistro_utils STATIC
  BackgroundThreads.cpp
  hostname.cpp
  LogWriter.cpp
  ProcessRunner.cpp
  server_socket.cpp
  SettingsMap.cpp
  SubprocessTaskQueue.cpp
  TemporaryFile.cpp
  KillableSubprocess.cpp
  service_clients.cpp
  shell.cpp
)
bistro_link_libraries(
  lib_bistro_utils
  lib_bistro_if
  lib_bistro_sqlite
)

add_subdirectory(test)
