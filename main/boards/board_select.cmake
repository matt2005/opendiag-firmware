# SPDX-License-Identifier: GPL-3.0-only
#
# Board selection: the one place that decides which board is built.
#
# Included by the firmware CMakeLists.txt and by test/host/CMakeLists.txt, so
# both pick a board the same way. Nothing here names a board. The boards are
# found from the files in this directory, the way Arduino finds a variant in
# boards.txt:
#
#   main/boards/<name>.h       profile: defines BOARD_ID <id> and the board's
#                              pins and features (see board_spec.h)
#   main/boards/board_<id>.c   board file: the hooks in board_hooks.h
#   test/host/boards/<id>.cmake  host suites for the board
#
# Build a board with -DOPENDIAG_BOARD=<id> (default: stock). The alias
# -DOPENDIAG_BOARD_<ID>=ON means the same, one per board, for older scripts.
#
# Outputs, cache INTERNAL and rewritten on every configure:
#   OPENDIAG_BOARD_SELECTED  the board id in use
#   OPENDIAG_BOARD_PROFILE   path of its profile header
#   OPENDIAG_BOARD_IMPL      path of its board file
#   OPENDIAG_BOARD_INCLUDE   directory holding the generated board_profile.h,
#                            which the shared code includes

set(OPENDIAG_BOARDS_DIR "${CMAKE_CURRENT_LIST_DIR}")

# Discover the profiles. Files named board_*.h are shared board headers.
file(GLOB OPENDIAG_PROFILE_HEADERS CONFIGURE_DEPENDS "${OPENDIAG_BOARDS_DIR}/*.h")
set(OPENDIAG_BOARD_IDS "")
foreach(_header IN LISTS OPENDIAG_PROFILE_HEADERS)
    get_filename_component(_name "${_header}" NAME)
    if(_name MATCHES "^board_")
        continue()
    endif()
    file(STRINGS "${_header}" _id_line REGEX "^#define[ \t]+BOARD_ID[ \t]")
    if(NOT _id_line MATCHES "^#define[ \t]+BOARD_ID[ \t]+\"([a-z0-9_]+)\"")
        message(FATAL_ERROR "${_name} must define BOARD_ID as a lowercase id")
    endif()
    set(_id "${CMAKE_MATCH_1}")
    if(_id IN_LIST OPENDIAG_BOARD_IDS)
        message(FATAL_ERROR "BOARD_ID ${_id} is defined by two profiles")
    endif()
    list(APPEND OPENDIAG_BOARD_IDS "${_id}")
    set(OPENDIAG_PROFILE_FOR_${_id} "${_header}")
    string(TOUPPER "${_id}" _upper)
    option(OPENDIAG_BOARD_${_upper} "Alias for OPENDIAG_BOARD=${_id}" OFF)
endforeach()

if(NOT OPENDIAG_BOARD_IDS)
    message(FATAL_ERROR "No board profiles found in ${OPENDIAG_BOARDS_DIR}")
endif()

set(OPENDIAG_BOARD "stock" CACHE STRING "Board to build: ${OPENDIAG_BOARD_IDS}")
set_property(CACHE OPENDIAG_BOARD PROPERTY STRINGS ${OPENDIAG_BOARD_IDS})

# An alias that is ON chooses its board. Two aliases ON is an error.
set(_chosen "${OPENDIAG_BOARD}")
set(_aliased "")
foreach(_id IN LISTS OPENDIAG_BOARD_IDS)
    string(TOUPPER "${_id}" _upper)
    if(OPENDIAG_BOARD_${_upper})
        list(APPEND _aliased "${_id}")
    endif()
endforeach()
list(LENGTH _aliased _aliased_count)
if(_aliased_count GREATER 1)
    message(FATAL_ERROR "Only one OPENDIAG_BOARD_<ID> alias may be ON, got: ${_aliased}")
elseif(_aliased_count EQUAL 1)
    set(_chosen "${_aliased}")
endif()

if(NOT _chosen IN_LIST OPENDIAG_BOARD_IDS)
    message(FATAL_ERROR "No board '${_chosen}'. Boards: ${OPENDIAG_BOARD_IDS}")
endif()

set(OPENDIAG_BOARD_SELECTED "${_chosen}" CACHE INTERNAL "Board in use" FORCE)
set(OPENDIAG_BOARD_PROFILE "${OPENDIAG_PROFILE_FOR_${_chosen}}" CACHE INTERNAL "Board profile header" FORCE)
set(OPENDIAG_BOARD_IMPL "${OPENDIAG_BOARDS_DIR}/board_${_chosen}.c" CACHE INTERNAL "Board file" FORCE)
if(NOT EXISTS "${OPENDIAG_BOARD_IMPL}")
    message(FATAL_ERROR "Board '${_chosen}' has no board file: boards/board_${_chosen}.c")
endif()

# The generated header is the only thing the shared code includes for a board.
set(OPENDIAG_BOARD_INCLUDE "${CMAKE_BINARY_DIR}/opendiag_board" CACHE INTERNAL "Generated board header dir" FORCE)
configure_file("${OPENDIAG_BOARDS_DIR}/board_profile.h.in"
               "${OPENDIAG_BOARD_INCLUDE}/board_profile.h" @ONLY)

message(STATUS "OpenDiag board: ${OPENDIAG_BOARD_SELECTED} (${OPENDIAG_BOARD_PROFILE})")
