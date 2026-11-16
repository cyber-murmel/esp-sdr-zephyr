# SPDX-License-Identifier: GPL-3.0-or-later

zephyr_library()
zephyr_library_sources(
  mt_frame.c
  zb_crypto.c
  zb_frame.c
  zb_mac.c
  zb_mac_frame.c
  zb_net.c
  zb_nwk.c
  zb_nvdef.c
  znp_core.c
  znp_nv.c
  znp_sys.c
  znp_zdo.c
  zb_stack.c
)
