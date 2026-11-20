set(srcs
        "main.c"
        "sketch.cpp"
        "haltz_selftest.cpp")

set(requires "bluepad32" "bluepad32_arduino" "arduino" "btstack" "haltz_proto")

idf_component_register(SRCS "${srcs}"
                    INCLUDE_DIRS "."
                    REQUIRES "${requires}")

# HALTZ: first 8 hex chars of the git commit, or "00000000" without git.
include(${CMAKE_CURRENT_LIST_DIR}/../../../shared/haltz_proto/cmake/haltz_git_hash.cmake)
haltz_git_hash(HALTZ_GIT_HASH_VALUE)
target_compile_definitions(${COMPONENT_LIB} PRIVATE HALTZ_GIT_HASH="${HALTZ_GIT_HASH_VALUE}")
