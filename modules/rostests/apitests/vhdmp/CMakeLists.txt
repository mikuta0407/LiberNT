
add_executable(vhdmp_apitest
    VhdBcd.c
    VhdBoot.c
    testlist.c
    ${REACTOS_SOURCE_DIR}/base/applications/cmdutils/lntvhdboot/vhdbcd.c)
target_link_libraries(vhdmp_apitest wine)
set_module_type(vhdmp_apitest win32cui)
add_importlibs(vhdmp_apitest powrprof advapi32 msvcrt kernel32 ntdll)
add_rostests_file(TARGET vhdmp_apitest)
