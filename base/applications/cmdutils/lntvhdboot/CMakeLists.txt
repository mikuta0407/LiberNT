
add_executable(lntvhdboot lntvhdboot.c vhdbcd.c)
set_module_type(lntvhdboot win32cui UNICODE)
add_importlibs(lntvhdboot rpcrt4 advapi32 msvcrt kernel32)
add_cd_file(TARGET lntvhdboot DESTINATION reactos/system32 FOR all)
