
spec2def(ldrfwd_source.dll ldrfwd_source.spec)

add_library(ldrfwd_source MODULE ldrfwd_source.c ${CMAKE_CURRENT_BINARY_DIR}/ldrfwd_source.def)
set_module_type(ldrfwd_source win32dll ENTRYPOINT DllMain 12)
add_importlibs(ldrfwd_source kernel32 ntdll)
add_dependencies(ldrfwd_source psdk)
