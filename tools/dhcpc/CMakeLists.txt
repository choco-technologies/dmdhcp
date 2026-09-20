# Name of the module
set(DMOD_MODULE_NAME        dhcpc)

# Author (should be string)
set(DMOD_AUTHOR_NAME        "Patryk Kubiak")

# Stack size for the module (should be integer)
set(DMOD_STACK_SIZE         4096)

# Priority for the thread of the module (should be integer)
set(DMOD_PRIORITY           0)

# Path to the .dmr file - enables release package creation in build/packages/
set(DMOD_DMR_PATH           ${CMAKE_CURRENT_SOURCE_DIR}/dhcpc.dmr)

#
#   dmod_add_executable - create an executable module
#   it has the same signature as add_executable
#   and can be used in the same way after the creation
#   (for example, to link libraries)
#
dmod_add_executable(${DMOD_MODULE_NAME} ${DMOD_MODULE_VERSION}
    src/dhcpc.c
)

# dmdhcp is this same repository's own top-level module, already built as
# part of this CMake configure by the time tools/ is added (see the root
# CMakeLists.txt) - link its dmdhcp_if target directly, exactly like
# tests/CMakeLists.txt does, instead of fetching dmdhcp's own published
# headers back via dmod_link_modules() as if it were an external module.
target_link_libraries(${DMOD_MODULE_NAME} dmdhcp_if)

# dmnetif: dmnetif_find_by_name() to resolve argv[1] (the interface name)
#          into the dmnetif_iface_t dmdhcp_start() needs - also part of the
#          header chain dmdhcp.h itself pulls in (dmnetif_iface_t), the same
#          reasoning dmicmp's own CMakeLists.txt gives for linking
#          dmroute/dmnetif header-only.
# dmroute: dmdhcp.h needs dmroute_addr_t directly - dhcpc.c never calls a
#          dmroute_* function itself.
dmod_link_modules(${DMOD_MODULE_NAME}
    dmnetif
    dmroute
)

add_subdirectory(tests)
