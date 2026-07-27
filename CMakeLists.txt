cmake_minimum_required(VERSION 3.18)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT DEFINED DMOD_MODULE_VERSION)
    set(DMOD_MODULE_VERSION "0.1" CACHE STRING "DMOD module version")
endif()

# If a local dmod checkout is provided, use it instead of fetching from git
if(DEFINED DMOD_DIR AND NOT "${DMOD_DIR}" STREQUAL "")
    set(FETCHCONTENT_SOURCE_DIR_DMOD "${DMOD_DIR}" CACHE PATH "" FORCE)
endif()

include(FetchContent)
FetchContent_Declare(
    dmod
    GIT_REPOSITORY https://github.com/choco-technologies/dmod.git
    GIT_TAG        develop
)

set(DMOD_MODE "DMOD_MODULE" CACHE STRING "DMOD build mode")
set(DMOD_BUILD_TESTS OFF CACHE BOOL "Build tests")
set(DMOD_BUILD_EXAMPLES OFF CACHE BOOL "Build examples")
set(DMOD_BUILD_TOOLS OFF CACHE BOOL "Build tools")
set(DMOD_BUILD_TEMPLATES OFF CACHE BOOL "Build templates")

FetchContent_MakeAvailable(dmod)

project(dmdhcp VERSION ${DMOD_MODULE_VERSION}
    DESCRIPTION "dmdhcp DMOD library module"
    LANGUAGES C CXX)

set(DMOD_DIR ${dmod_SOURCE_DIR} CACHE PATH "DMOD source directory")
set(DMOD_SCRIPTS_DIR ${DMOD_DIR}/scripts CACHE PATH "DMOD scripts directory")
include(${DMOD_DIR}/paths.cmake)
dmod_setup_external_module()

# Name of the module
set(DMOD_MODULE_NAME        dmdhcp)

# Author (should be string)
set(DMOD_AUTHOR_NAME        "Patryk Kubiak")

# Stack size for the module (should be integer)
set(DMOD_STACK_SIZE         1024)

# Path to the .dmr file - enables release package creation in build/packages/
set(DMOD_DMR_PATH           ${CMAKE_CURRENT_SOURCE_DIR}/dmdhcp.dmr)

# List of MAL interfaces implemented by this module (leave empty if none)
set(DMOD_MAL_IMPLS          )

# List of DIF interfaces implemented by this module (leave empty if none)
set(DMOD_DIF_IMPLS          )

#
#   dmod_add_library - create a library module
#   it has the same signature as add_library
#   and can be used in the same way after the creation
#   (for example, to link libraries)
#
dmod_add_library(${DMOD_MODULE_NAME} ${DMOD_MODULE_VERSION}
    # List of source files - can include C and C++ files
    src/dmdhcp.c
    src/dmdhcp_registrations.c
    src/dmdhcp_wire.c
    src/dmdhcp_options.c
    src/dmdhcp_lease_table.c
    src/dmdhcp_output.c
    src/dmdhcp_input.c
    src/dmdhcp_lifecycle.c
)

target_include_directories(${DMOD_MODULE_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_include_directories(${DMOD_MODULE_NAME}_if INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# dmudp:   dmudp_bind()/_unbind() to receive (see src/dmdhcp_input.c),
#          dmudp_send()/_send_on_iface() to transmit (see
#          src/dmdhcp_output.c) - broadcast sends always go through
#          _send_on_iface(), never the routed _send(), see that file's top
#          comment.
# dmip:    header-only - dmudp.h itself `#include`s "dmip.h" (for
#          dmip_addr_t, reused as the type of dmudp_send()'s `dst`), so
#          this needs to be linked purely to satisfy that transitive
#          header dependency. dmdhcp never calls a dmip_* function
#          directly - the same "linked for a header, not a function"
#          relationship dmtcp's own CMakeLists.txt documents for dmroute.
# dmnetif: dmnetif_iface_t, applying a lease (dmnetif_set_netmask()/
#          _set_ip_address()/_set_broadcast(), dmnetif_get_mac_address()/
#          _get_name()) - see src/dmdhcp_lifecycle.c / src/dmdhcp_output.c.
# dmroute: dmroute_addr_t (used throughout instead of dmip_addr_t - see
#          include/dmdhcp.h's top comment), dmroute_add()/_remove() for the
#          default route installed on BOUND.
# dmarp:   dmarp_resolve() as the RFC 2131 §4.4.1 conflict probe (see
#          src/dmdhcp_output.c's run_conflict_probe()).
# dmlist:  the lease table (src/dmdhcp_lease_table.c) and each lease's DNS
#          server list (option 6 - dmdhcp is the only owner of DNS
#          configuration storage in this ecosystem).
# dmosi:   mutexes (one per lease), dmosi_timer_t (retransmit_timer,
#          lease_timer), and dmosi_rand32() for XID/jitter generation - no
#          bespoke PRNG in this module.
dmod_link_modules(${DMOD_MODULE_NAME}
    dmudp
    dmip
    dmnetif
    dmroute
    dmarp
    dmlist
    dmosi
)

enable_testing()
add_subdirectory(tests)
