
list(APPEND SOURCE
    disk.c
    scsi.c
    vhdmp.c)

add_library(vhdmp MODULE ${SOURCE} vhdmp.rc)
set_module_type(vhdmp kernelmodedriver)
target_link_libraries(vhdmp vhdx)
add_importlibs(vhdmp ntoskrnl hal)
add_registry_inf(vhdmp_reg.inf)
add_cd_file(TARGET vhdmp DESTINATION reactos/system32/drivers NO_CAB FOR all)
