# Included by IREE's compiler/CMakeLists.txt through IREE_CMAKE_PLUGIN_PATHS.
# The only job is to add our compiler directory to the build; the actual plugin
# registration happens in compiler/Target/CMakeLists.txt.
add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../../compiler gemmini_compiler)
