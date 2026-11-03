# Establishes the package root so that targets declared below are named
# gemmini_compiler::Target::... rather than being derived from the absolute
# source path (which produces unreadable, un-referenceable target names when
# the superproject lives outside the IREE tree).
iree_setup_c_src_root(
  PACKAGE_ROOT_PREFIX
    gemmini_compiler
)

add_subdirectory(Transforms)
add_subdirectory(Target)
