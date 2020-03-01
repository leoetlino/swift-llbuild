add_llbuild_library(llbuildBuildSystem STATIC
  BuildDescription.cpp
  BuildFile.cpp
  BuildKey.cpp
  BuildNode.cpp
  BuildSystem.cpp
  BuildSystemExtensionManager.cpp
  BuildSystemFrontend.cpp
  BuildValue.cpp
  ExternalCommand.cpp
  ShellCommand.cpp
  )

target_link_libraries(llbuildBuildSystem PUBLIC absl::flat_hash_map PRIVATE
  flatbuffers
  llbuildCore
  llbuildBasic
  llvmSupport)
