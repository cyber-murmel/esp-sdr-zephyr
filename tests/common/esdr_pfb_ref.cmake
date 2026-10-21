# SPDX-License-Identifier: GPL-3.0-or-later
#
# The float channelizer and detector (esdr_pfb_ref.h), the reference of the P4's fixed-point
# bank, for a unit test: built at -O2 with multiply-adds fused, with the prototype design
# (apps/common/esdr_pfb.cmake).

include(${CMAKE_CURRENT_LIST_DIR}/../../apps/common/esdr_pfb.cmake)
target_include_directories(app PRIVATE ${CMAKE_CURRENT_LIST_DIR}/include
  ${CMAKE_CURRENT_LIST_DIR}/../../apps/common/src)
target_sources(app PRIVATE ${CMAKE_CURRENT_LIST_DIR}/src/esdr_pfb_ref.c)
set_source_files_properties(${CMAKE_CURRENT_LIST_DIR}/src/esdr_pfb_ref.c PROPERTIES
  COMPILE_OPTIONS "-O2;-ffp-contract=fast")
