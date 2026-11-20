# haltz_git_hash(<out_var>)
#
# Sets <out_var> to the first 8 hex characters of the repo's HEAD commit, or "00000000"
# when git or the repository isn't available. Runs at CMake configure time and asks CMake
# to re-configure when HEAD moves (works for plain CMake/Ninja builds; with PlatformIO run
# a clean build if the hash looks stale).
#
# Build tooling only: no board-specific code here.

set(_HALTZ_REPO_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../..")
get_filename_component(_HALTZ_REPO_ROOT "${_HALTZ_REPO_ROOT}" ABSOLUTE)

function(haltz_git_hash out_var)
    set(hash "00000000")
    find_package(Git QUIET)
    if(GIT_FOUND)
        execute_process(
            COMMAND "${GIT_EXECUTABLE}" -C "${_HALTZ_REPO_ROOT}" rev-parse HEAD
            OUTPUT_VARIABLE full
            RESULT_VARIABLE rc
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET)
        if(rc EQUAL 0 AND full MATCHES "^[0-9a-f]+$")
            string(SUBSTRING "${full}" 0 8 hash)
        endif()

        # Re-run configure when the checked-out commit changes.
        set(git_dir "${_HALTZ_REPO_ROOT}/.git")
        if(EXISTS "${git_dir}/HEAD")
            set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${git_dir}/HEAD")
            file(READ "${git_dir}/HEAD" head)
            if(head MATCHES "^ref: ([^\n]+)")
                set(ref_file "${git_dir}/${CMAKE_MATCH_1}")
                if(EXISTS "${ref_file}")
                    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${ref_file}")
                endif()
            endif()
        endif()
    endif()
    message(STATUS "HALTZ git hash: ${hash}")
    set(${out_var} "${hash}" PARENT_SCOPE)
endfunction()
