/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Right after the wifi module's Kconfig values. The hal maps Zephyr's Wi-Fi options to ESP-IDF's
 * names in a header it puts first on every compile line (zephyr/CMakeLists.txt), so it saw the
 * host's values, which have no Wi-Fi. Once more, with the module's.
 */

#undef ZEPHYR_COMPAT_H
#include <zephyr_compat.h>
