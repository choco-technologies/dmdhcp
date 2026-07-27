set(DMOD_MODULE_NAME        test_dmdhcp)
set(DMOD_MODULE_VERSION     ${PROJECT_VERSION})
set(DMOD_AUTHOR_NAME        "Patryk Kubiak")
set(DMOD_STACK_SIZE         1024)
set(DMOD_PRIORITY           0)

dmod_add_test(${DMOD_MODULE_NAME} ${DMOD_MODULE_VERSION}
    dmdhcp_test.c
)

target_include_directories(${DMOD_MODULE_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
)

target_link_libraries(${DMOD_MODULE_NAME} dmdhcp_if)

# dmip/dmnetbridge/dmarp/dmnetif/dmroute: drive dmdhcp's registered
# dmudp_bind() handler (via dmudp, transitively through dmip/dmnetbridge)
# with hand-built fixture packets, and seed/inspect fixture routes/ARP
# entries for the state-machine tests - the same set dmtcp/tests/CMakeLists.txt
# links for the same reason.
# dmudp: the state-machine tests build a UDP segment by hand
#        (dmudp_build_header()) to wrap each fixture DHCP message.
# dmosi: dmosi_thread_sleep() to wait out the RFC 2131 §4.4.1 conflict
#        probe hand-off (see wait_while_state() in dmdhcp_test.c).
dmod_link_modules(${DMOD_MODULE_NAME}
    dmudp
    dmip
    dmnetbridge
    dmarp
    dmnetif
    dmroute
    dmosi
)

# Register the test module with ctest - this replays the same two steps CI
# runs (see .github/templates/ci.yml.template): install the test module's
# dependencies with dmf-get, then execute it through dmod_loader. The test's
# exit code equals its number of failed steps (see dmod_add_test).
find_program(DMF_GET_EXECUTABLE dmf-get HINTS ${DMOD_TOOLS_BIN_DIR} PATHS ENV PATH)
find_program(DMOD_LOADER_EXECUTABLE dmod_loader HINTS ${DMOD_TOOLS_BIN_DIR} PATHS ENV PATH)

if(DMF_GET_EXECUTABLE AND DMOD_LOADER_EXECUTABLE)
    add_test(
        NAME ${DMOD_MODULE_NAME}
        COMMAND ${CMAKE_COMMAND} -E env DMOD_DMF_DIR=${DMOD_DMF_DIR}
            bash -c "${DMF_GET_EXECUTABLE} install -d ${DMOD_DMF_DIR}/${DMOD_MODULE_NAME}-local.dmd -y && ${DMOD_LOADER_EXECUTABLE} ${DMOD_DMF_DIR}/${DMOD_MODULE_NAME}.dmf"
    )
else()
    message(WARNING "dmf-get/dmod_loader not found - 'ctest' will not run ${DMOD_MODULE_NAME}. Build the tools first with -DDMOD_BUILD_TOOLS=ON")
endif()
