add_llbuild_library(llbuildCore STATIC
  FlatBuildDB.cpp
  BuildDB.cpp
  BuildEngine.cpp
  BuildEngineTrace.cpp
  DependencyInfoParser.cpp
  MakefileDepsParser.cpp
  SQLiteBuildDB.cpp
)

target_link_libraries(llbuildCore PRIVATE
  absl::flat_hash_map
  absl::node_hash_map
  flatbuffers
  llbuildBasic
  llvmSupport
  SQLite::SQLite3)
