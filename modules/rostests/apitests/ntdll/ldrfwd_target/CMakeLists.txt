
spec2def(ldrfwd_target.dll ldrfwd_target.spec)

add_library(ldrfwd_target MODULE ldrfwd_target.c ${CMAKE_CURRENT_BINARY_DIR}/ldrfwd_target.def)
set_module_type(ldrfwd_target win32dll ENTRYPOINT DllMain 12)
add_importlibs(ldrfwd_target kernel32 ntdll)
add_dependencies(ldrfwd_target psdk)
