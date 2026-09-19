set(DMOD_MODULE_NAME        test_dhcpc)
set(DMOD_MODULE_VERSION     ${PROJECT_VERSION})
set(DMOD_AUTHOR_NAME        "Patryk Kubiak")
set(DMOD_STACK_SIZE         1024)
set(DMOD_PRIORITY           0)

dmod_add_test(${DMOD_MODULE_NAME} ${DMOD_MODULE_VERSION}
    dhcpc_test.c
)

# `dhcpc` itself is never linked as a dependency here - it's invoked on
# demand through Dmod_RunModule() (see dhcpc_test.c), which loads/runs/
# unloads it per call (pulling dmdhcp in as its own linked dependency)
# instead of holding it resident for the whole test run.
# dmnetif: the "/dev/null"-backed fixture interface dhcpc resolves by name.
# dmroute: dmnetif.h #includes dmroute.h directly (dmroute_addr_t) - needed
#          for that header to resolve even though dhcpc_test.c itself never
#          calls a dmroute_* function (same reasoning tools/dhcpc's own
#          CMakeLists.txt gives for linking it).
dmod_link_modules(${DMOD_MODULE_NAME}
    dmnetif
    dmroute
)
