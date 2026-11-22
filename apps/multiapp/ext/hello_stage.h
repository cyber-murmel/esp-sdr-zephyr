/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * What hello's stage hello_op shares with its kernels (kernels/hello_x2.c, hello_x4.c): its type and
 * the global a kernel may import from the app. The ABI hash of the stage (multiapp_stage_abi() in
 * CMakeLists.txt) covers this file, so a kernel built before it changed does not load.
 */

#ifndef HELLO_STAGE_H_
#define HELLO_STAGE_H_

typedef int (*hello_op_t)(int v);

/* A global, so that a kernel may import it from the app (exports.py allows the app's globals). */
extern int hello_stage_bias;

#endif /* HELLO_STAGE_H_ */
