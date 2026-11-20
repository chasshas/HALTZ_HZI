idf_component_register(SRCS "main.c"
                       INCLUDE_DIRS "../include"
                       REQUIRES haltz_proto esp_psram spi_flash esp_partition esp_timer)

# First 8 hex chars of the git commit, or "00000000" without git.
include(${CMAKE_CURRENT_LIST_DIR}/../../../shared/haltz_proto/cmake/haltz_git_hash.cmake)
haltz_git_hash(HALTZ_GIT_HASH_VALUE)
target_compile_definitions(${COMPONENT_LIB} PRIVATE HALTZ_GIT_HASH="${HALTZ_GIT_HASH_VALUE}")
