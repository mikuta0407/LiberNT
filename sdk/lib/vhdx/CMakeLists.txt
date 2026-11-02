# PROJECT:     LiberNT VHDX Library
# LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
# PURPOSE:     VHDX image parsing and block translation ([MS-VHDX])
# COPYRIGHT:   Copyright 2026 mikuta0407 <mikuta0407@mikuta0407.net>

add_library(vhdx vhdx.c)
target_include_directories(vhdx PUBLIC ${REACTOS_SOURCE_DIR}/sdk/include/reactos/libs/vhdx)
add_dependencies(vhdx psdk xdk)
