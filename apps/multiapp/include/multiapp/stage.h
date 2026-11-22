/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * A DSP stage of an app that a kernel can replace while the app runs (doc/apps/multiapp-internals.md,
 * "Hot-swapping a stage"), and the descriptor a kernel exports.
 *
 * In the app, one stage is one function pointer behind a name:
 *
 *	MULTIAPP_STAGE_DEFINE(ring_sink, esp_sdr_diag16_sink_t, ring_sink_default);
 *	...
 *	sink = ring_sink_stage_sync();   // before the stage runs: the quiescent point
 *	run(sink);                       // only this function is called, by any thread
 *	ring_sink_stage_idle();          // nobody is inside the stage or holds the pointer now
 *
 * One thread is inside a stage at a time: the thread whose sync found the stage idle owns it until
 * its own idle call. The pointer sync returns may be handed to other threads (the sink of a run
 * is called by the ring's thread), but only the owner calls sync and idle; another thread's idle
 * is ignored, and another thread's sync while the owner is inside is a breach of the contract that
 * makes the host refuse every swap of the stage (-EBUSY) until the app is loaded again.
 *
 * The host swaps the pointer (multiapp stage ...) only when no thread can be inside the old
 * function: while the stage is idle, or at the owner's next sync or idle call. So the old kernel
 * is never mid-call on another CPU when it is unloaded, and the app needs no lock of its own. A
 * stage the app never syncs times out and the swap is undone.
 *
 * What a kernel and the app share beyond the function type (a state struct, a buffer) is the
 * stage's ABI: multiapp_stage_abi() in apps/multiapp/CMakeLists.txt hashes the headers that
 * declare it into MULTIAPP_STAGE_ABI_<stage>, for the app's build and the kernel's, and the host
 * refuses a kernel whose hash is not the app's.
 *
 * Standalone (no MULTIAPP_NAME, or CONFIG_MULTIAPP_STAGES off) the macros are a function call of
 * the default: no object, no registration, the same code as before the stage was declared.
 *
 * A kernel (multiapp_kernel() in apps/multiapp/CMakeLists.txt) is one function of the stage's
 * type, exported through MULTIAPP_KERNEL_DEFINE(). It may import what the host already exports and
 * the target app's global symbols, nothing else (exports.py checks that when it is built).
 */

#ifndef MULTIAPP_STAGE_H_
#define MULTIAPP_STAGE_H_

#include <stdint.h>

#if defined(CONFIG_MULTIAPP_STAGES)

/* The app's side of a stage, in its own data; the host keeps the rest (src/stage.c). */
struct multiapp_stage {
	/* The stage's name and the spelling of its function type, for the host's checks. */
	const char *name;
	const char *sig_text;
	/* The hash of the headers the stage shares with its kernels (multiapp_stage_abi()). */
	uint32_t abi;
	/* What the app was built with: where "revert" goes. */
	void *dflt;
	/* What runs now; written by the host under its lock only, read by anyone. */
	void *volatile cur;
	/* The host's slot, set when the stage registers. */
	void *host;
};

/* Called from the constructor and destructor below, by the host's loader. */
void multiapp_stage_register(struct multiapp_stage *st);
void multiapp_stage_unregister(struct multiapp_stage *st);
/* The quiescent point: apply a pending swap, mark the stage busy, return what runs now. */
void *multiapp_stage_sync(struct multiapp_stage *st);
/* The app is outside the stage until its next sync: the host may swap at once. */
void multiapp_stage_idle(struct multiapp_stage *st);

#endif /* CONFIG_MULTIAPP_STAGES */

#if defined(MULTIAPP_NAME) && defined(CONFIG_MULTIAPP_STAGES)

/*
 * Declare a stage: @p _name a C identifier, @p _fn_type the function pointer type (a typedef, so
 * that a kernel spells the same), @p _default the function the app is built with. Defines
 * <_name>_stage_sync() and <_name>_stage_idle(); MULTIAPP_STAGE_ABI_<_name> must be defined by the
 * build (multiapp_stage_abi()). It ends in a declaration that wants the semicolon after it.
 */
#define MULTIAPP_STAGE_DEFINE(_name, _fn_type, _default)                                           \
	static struct multiapp_stage _name##_stage_obj = {                                         \
		.name = #_name,                                                                    \
		.sig_text = #_fn_type,                                                             \
		.abi = MULTIAPP_STAGE_ABI_##_name,                                                 \
		.dflt = (void *)(_default),                                                        \
		.cur = (void *)(_default),                                                         \
	};                                                                                         \
	static inline _fn_type _name##_stage_sync(void)                                            \
	{                                                                                          \
		return (_fn_type)multiapp_stage_sync(&_name##_stage_obj);                          \
	}                                                                                          \
	static inline void _name##_stage_idle(void)                                                \
	{                                                                                          \
		multiapp_stage_idle(&_name##_stage_obj);                                           \
	}                                                                                          \
	static void __attribute__((constructor)) _name##_stage_up(void)                            \
	{                                                                                          \
		multiapp_stage_register(&_name##_stage_obj);                                       \
	}                                                                                          \
	/* Reverts and unregisters as the app unloads (the host has dropped its kernels by then). */ \
	static void __attribute__((destructor)) _name##_stage_down(void)                           \
	{                                                                                          \
		multiapp_stage_unregister(&_name##_stage_obj);                                     \
	}                                                                                          \
	struct multiapp_stage_decl_##_name

#else /* standalone */

#define MULTIAPP_STAGE_DEFINE(_name, _fn_type, _default)                                           \
	static inline _fn_type _name##_stage_sync(void)                                            \
	{                                                                                          \
		return (_default);                                                                 \
	}                                                                                          \
	static inline void _name##_stage_idle(void)                                                \
	{                                                                                          \
	}                                                                                          \
	struct multiapp_stage_decl_##_name

#endif /* MULTIAPP_NAME */

/* The descriptor a kernel exports as "multiapp_kernel" (one per kernel file). */
#define MULTIAPP_KERNEL_MAGIC 0x314b504dU /* "MPK1" */

struct multiapp_kernel_desc {
	uint32_t magic;
	/* The stage it replaces and the spelling of the function type it was built for. */
	const char *stage;
	const char *sig_text;
	/* The stage's ABI hash it was built against (MULTIAPP_STAGE_ABI_<stage>). */
	uint32_t abi;
	/* The function: where it runs (internal RAM for an INTERNAL kernel). */
	void *fn;
};

#if defined(MULTIAPP_NAME)
#include <zephyr/llext/symbol.h>
#include <zephyr/sys/util.h>
/*
 * In a kernel's source: MULTIAPP_KERNEL_DEFINE(stage name, function type, function). The stage
 * name is the multiapp_kernel(STAGE ...) value, so that the build and the file agree.
 */
#define MULTIAPP_KERNEL_DEFINE(_stage, _fn_type, _fn)                                              \
	BUILD_ASSERT(__builtin_types_compatible_p(__typeof__(&(_fn)), _fn_type),                   \
		     "multiapp: the kernel's function does not have the stage's type");           \
	const struct multiapp_kernel_desc multiapp_kernel = {                                      \
		.magic = MULTIAPP_KERNEL_MAGIC,                                                    \
		.stage = #_stage,                                                                  \
		.sig_text = #_fn_type,                                                             \
		.abi = MULTIAPP_STAGE_ABI_##_stage,                                                \
		.fn = (void *)(_fn),                                                               \
	};                                                                                         \
	EXPORT_SYMBOL(multiapp_kernel)
#endif

#endif /* MULTIAPP_STAGE_H_ */
