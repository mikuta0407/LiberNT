
add_executable(vhdmp_apitest VhdBoot.c testlist.c)
target_link_libraries(vhdmp_apitest wine)
set_module_type(vhdmp_apitest win32cui)
add_importlibs(vhdmp_apitest powrprof advapi32 msvcrt kernel32)
add_rostests_file(TARGET vhdmp_apitest)
