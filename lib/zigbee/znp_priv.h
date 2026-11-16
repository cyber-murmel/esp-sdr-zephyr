/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef ZNP_PRIV_H_
#define ZNP_PRIV_H_

#include <zigbee/znp.h>

int znp_sys_handler(struct znp *z, const struct mt_frame *req, struct mt_frame *rsp);
int znp_util_handler(struct znp *z, const struct mt_frame *req, struct mt_frame *rsp);

#endif /* ZNP_PRIV_H_ */
