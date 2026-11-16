/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/**
 * @file
 * @brief Usage categories for code and data, placed by rules.
 */

#ifndef ESP_SDR_PLACE_H_
#define ESP_SDR_PLACE_H_

/**
 * @addtogroup esp_sdr_place
 * @{
 *
 * A function or buffer says how it is used; rules for the SoC, the board and the app say where
 * that goes (doc/code-placement.md). Every region expands to the attribute such code or data
 * carried before (IRAM_ATTR, DRAM_ATTR, EXT_RAM_BSS_ATTR, ESP_SDR_HIGH_RAM, ...), so the default
 * rules keep each section name as it was, and a multiapp extension's own definitions of those
 * attributes (multiapp/ext.h) still apply.
 *
 * Code, `ESP_SDR_CODE(cat, fn)` before a function definition, fn its name. cat:
 * - PACED_SAMPLE, PACED_CHUNK, PACED_EVENT: on a paced path, where a stall loses samples or misses
 *   a deadline (the receive ring and its sinks, DMA interrupts, transmit feeders, an ACK burst),
 *   per sample, per chunk or unit, per burst;
 * - SAMPLE, CHUNK, EVENT: a consumer with slack, per sample, per chunk or poll, per packet;
 * - COLD: init, shell, logging, errors: always flash, never in the hot block;
 * - NOCACHE: runs with the cache off: always IRAM.
 *
 * Data, `ESP_SDR_DATA(cat, name)` or `ESP_SDR_DATA_F(cat, name, flag)` on a definition, name the
 * object's name. cat: STATE (read and written per sample or chunk), TABLE (read-only, read per
 * sample), BULK (captures, histories, data touched only now and then), SCRATCH (reused between
 * calls). flag: DMA (internal RAM only), ATOMIC (never PSRAM: atomics, spinlocks, kernel objects),
 * NOINIT (the code initializes it itself; required in the high region, which is not zeroed at
 * boot). A clash between a flag and the region the rules chose stops the build.
 * `ESP_SDR_BUF_DEFINE()` and `ESP_SDR_STACK_DEFINE()` define a buffer or a thread stack the same
 * way.
 *
 * Rules (the generated esp_sdr_place_rules.h includes the app's, the board's and the SoC's rule
 * headers in that order; the first definition counts): `ESP_SDR_PLACE_<cat>` is a category's
 * region, `ESP_SDR_PIN_CODE_<fn>` and `ESP_SDR_PIN_DATA_<name>` one function's or object's. Rules
 * and pins can be narrowed to a scope, the compile definition ESP_SDR_PLACE_SCOPE of an app's or a
 * library's sources (`ESP_SDR_PLACE_<scope>_<cat>`, `ESP_SDR_PIN_CODE_<scope>_<fn>`), so that the
 * apps an image combines keep their own rules (apps/multiapp), and pins to a file that defines
 * ESP_SDR_PLACE_FILE (`ESP_SDR_PIN_CODE_<file>_<fn>`; static names repeat across files). Lookup
 * order: file pin, scope pin, pin, scope rule, rule.
 * Regions: ESP_SDR_IRAM, ESP_SDR_HOTBLOCK, ESP_SDR_FLASH for code; ESP_SDR_DRAM, ESP_SDR_DEFAULT,
 * ESP_SDR_HIGH, ESP_SDR_PSRAM, ESP_SDR_PSRAM_FILE, ESP_SDR_NOINIT, ESP_SDR_PSRAM_NOINIT for data;
 * a pin of ESP_SDR_RULE stands for no pin. Builds for other targets (native_sim) place nothing.
 */

/**
 * @def ESP_SDR_CODE(cat, fn)
 * @brief Placement of function @p fn, used as @p cat (before its definition).
 *
 * @def ESP_SDR_DATA(cat, nm)
 * @brief Placement of object @p nm, used as @p cat (on its definition).
 *
 * @def ESP_SDR_DATA_F(cat, nm, f)
 * @brief As ESP_SDR_DATA(), with constraint @p f (DMA, ATOMIC or NOINIT) checked.
 *
 * @def ESP_SDR_DATA_F2(cat, nm, f1, f2)
 * @brief As ESP_SDR_DATA_F(), with two constraints.
 *
 * @def ESP_SDR_BUF_DEFINE(cat, type, nm, dims, align, f)
 * @brief `static type nm dims __aligned(align)` in its region, with constraint @p f (or NONE).
 *
 * @def ESP_SDR_STACK_DEFINE(nm, size)
 * @brief A static kernel thread stack in the region of category STACK.
 *
 * @def ESP_SDR_ASM_SECTION(cat, fn)
 * @brief The `.section` line of assembler function @p fn, used as @p cat.
 *
 * @def ESP_SDR_REGION_CODE(cat, fn)
 * @brief The region token the rules choose for function @p fn used as @p cat, for tools
 * (scripts/place/place_report.py).
 */

#if defined(CONFIG_SOC_FAMILY_ESPRESSIF_ESP32)

#if !defined(__ASSEMBLER__)
#include <esp_attr.h>
#include <zephyr/kernel.h>
#include <zephyr/linker/section_tags.h>
#include <esp_sdr/esp_sdr.h>
#endif

#include <esp_sdr_place_rules.h>

#define Z_PL_CAT(a, b)       Z_PL_CAT_(a, b)
#define Z_PL_CAT_(a, b)      a##b
#define Z_PL_2ND(...)        Z_PL_2ND_(__VA_ARGS__)
#define Z_PL_2ND_(a, b, ...) b
#define Z_PL_STR(x)          #x
#define Z_PL_XSTR(x)         Z_PL_STR(x)

/* x when it expands to a region token, else d. */
#define Z_PL_OR(x, d)                    Z_PL_2ND(Z_PL_CAT(Z_PL_IS_, x), d)
#define Z_PL_IS_ESP_SDR_IRAM             ~, ESP_SDR_IRAM
#define Z_PL_IS_ESP_SDR_HOTBLOCK         ~, ESP_SDR_HOTBLOCK
#define Z_PL_IS_ESP_SDR_FLASH            ~, ESP_SDR_FLASH
#define Z_PL_IS_ESP_SDR_DRAM             ~, ESP_SDR_DRAM
#define Z_PL_IS_ESP_SDR_DEFAULT          ~, ESP_SDR_DEFAULT
#define Z_PL_IS_ESP_SDR_HIGH             ~, ESP_SDR_HIGH
#define Z_PL_IS_ESP_SDR_PSRAM            ~, ESP_SDR_PSRAM
#define Z_PL_IS_ESP_SDR_PSRAM_FILE       ~, ESP_SDR_PSRAM_FILE
#define Z_PL_IS_ESP_SDR_NOINIT           ~, ESP_SDR_NOINIT
#define Z_PL_IS_ESP_SDR_PSRAM_NOINIT     ~, ESP_SDR_PSRAM_NOINIT

/*
 * The region of a function or object, the first of: its pin qualified by the file
 * (ESP_SDR_PLACE_FILE), its pin qualified by the scope (ESP_SDR_PLACE_SCOPE, a compile definition
 * of an app's or a library's sources), its plain pin, its category's rule for the scope, its
 * category's rule. The callers paste every token they are given with a prefix right away, so that
 * a category, a name or a flag that is also a macro (CHUNK, say) is never expanded.
 */
#define Z_PL_FILEQ(kind, _nm)  Z_PL_CAT(Z_PL_CAT(kind, ESP_SDR_PLACE_FILE), _nm)
#define Z_PL_SCOPEQ(kind, _nm) Z_PL_CAT(Z_PL_CAT(kind, ESP_SDR_PLACE_SCOPE), _nm)
#define Z_PL_PICK(fq, sq, pin, sr, rule)                                                           \
	Z_PL_OR(fq, Z_PL_OR(sq, Z_PL_OR(pin, Z_PL_OR(sr, Z_PL_OR(rule, Z_PL_NORULE)))))
#define Z_PL_CODE_REGION(rule, _cat, pin, _fn)                                                     \
	Z_PL_PICK(Z_PL_FILEQ(ESP_SDR_PIN_CODE_, _fn), Z_PL_SCOPEQ(ESP_SDR_PIN_CODE_, _fn), pin,    \
		  Z_PL_SCOPEQ(ESP_SDR_PLACE_, _cat), rule)
#define Z_PL_DATA_REGION(rule, _cat, pin, _nm)                                                     \
	Z_PL_PICK(Z_PL_FILEQ(ESP_SDR_PIN_DATA_, _nm), Z_PL_SCOPEQ(ESP_SDR_PIN_DATA_, _nm), pin,    \
		  Z_PL_SCOPEQ(ESP_SDR_PLACE_, _cat), rule)

#define Z_PL_ERR(msg) _Pragma(Z_PL_XSTR(GCC error msg))
#define Z_PL_NORULE_MSG   "esp_sdr_place: no ESP_SDR_PLACE_ rule for this category"
#define Z_PL_KIND_MSG     "esp_sdr_place: a data region for code, or a code region for data"

#if !defined(__ASSEMBLER__)

/* Code. */
#define ESP_SDR_CODE(cat, fn)        Z_PL_CODE_##cat(ESP_SDR_PIN_CODE_##fn, _##fn)
#define Z_PL_CODE_NOCACHE(pin, _fn)  IRAM_ATTR
#define Z_PL_CODE_COLD(pin, _fn)
#define Z_PL_CODE_PACED_SAMPLE(pin, _fn)                                                           \
	Z_PL_CODE_AT(Z_PL_CODE_REGION(ESP_SDR_PLACE_PACED_SAMPLE, _PACED_SAMPLE, pin, _fn))
#define Z_PL_CODE_PACED_CHUNK(pin, _fn)                                                            \
	Z_PL_CODE_AT(Z_PL_CODE_REGION(ESP_SDR_PLACE_PACED_CHUNK, _PACED_CHUNK, pin, _fn))
#define Z_PL_CODE_PACED_EVENT(pin, _fn)                                                            \
	Z_PL_CODE_AT(Z_PL_CODE_REGION(ESP_SDR_PLACE_PACED_EVENT, _PACED_EVENT, pin, _fn))
#define Z_PL_CODE_SAMPLE(pin, _fn)                                                                 \
	Z_PL_CODE_AT(Z_PL_CODE_REGION(ESP_SDR_PLACE_SAMPLE, _SAMPLE, pin, _fn))
#define Z_PL_CODE_CHUNK(pin, _fn)                                                                  \
	Z_PL_CODE_AT(Z_PL_CODE_REGION(ESP_SDR_PLACE_CHUNK, _CHUNK, pin, _fn))
#define Z_PL_CODE_EVENT(pin, _fn)                                                                  \
	Z_PL_CODE_AT(Z_PL_CODE_REGION(ESP_SDR_PLACE_EVENT, _EVENT, pin, _fn))
#define Z_PL_CODE_AT(r)              Z_PL_CAT(Z_PL_CA_, r)

/* The region itself, for tools. */
#define ESP_SDR_REGION_CODE(cat, fn) Z_PL_RG_##cat(ESP_SDR_PIN_CODE_##fn, _##fn)
#define Z_PL_RG_NOCACHE(pin, _fn)    ESP_SDR_IRAM
#define Z_PL_RG_COLD(pin, _fn)       ESP_SDR_FLASH
#define Z_PL_RG_PACED_SAMPLE(pin, _fn)                                                             \
	Z_PL_CODE_REGION(ESP_SDR_PLACE_PACED_SAMPLE, _PACED_SAMPLE, pin, _fn)
#define Z_PL_RG_PACED_CHUNK(pin, _fn)                                                              \
	Z_PL_CODE_REGION(ESP_SDR_PLACE_PACED_CHUNK, _PACED_CHUNK, pin, _fn)
#define Z_PL_RG_PACED_EVENT(pin, _fn)                                                              \
	Z_PL_CODE_REGION(ESP_SDR_PLACE_PACED_EVENT, _PACED_EVENT, pin, _fn)
#define Z_PL_RG_SAMPLE(pin, _fn) Z_PL_CODE_REGION(ESP_SDR_PLACE_SAMPLE, _SAMPLE, pin, _fn)
#define Z_PL_RG_CHUNK(pin, _fn)  Z_PL_CODE_REGION(ESP_SDR_PLACE_CHUNK, _CHUNK, pin, _fn)
#define Z_PL_RG_EVENT(pin, _fn)  Z_PL_CODE_REGION(ESP_SDR_PLACE_EVENT, _EVENT, pin, _fn)

#define Z_PL_CA_ESP_SDR_IRAM         IRAM_ATTR
#define Z_PL_CA_ESP_SDR_FLASH
#if defined(CONFIG_ESP_SDR_HOT_TEXT)
#define Z_PL_CA_ESP_SDR_HOTBLOCK                                                                   \
	__attribute__((section(".text.esp_sdr_hot." Z_PL_XSTR(__COUNTER__))))
#else
#define Z_PL_CA_ESP_SDR_HOTBLOCK
#endif
#define Z_PL_CA_Z_PL_NORULE          Z_PL_ERR(Z_PL_NORULE_MSG)
#define Z_PL_CA_ESP_SDR_DRAM         Z_PL_ERR(Z_PL_KIND_MSG)
#define Z_PL_CA_ESP_SDR_DEFAULT      Z_PL_ERR(Z_PL_KIND_MSG)
#define Z_PL_CA_ESP_SDR_HIGH         Z_PL_ERR(Z_PL_KIND_MSG)
#define Z_PL_CA_ESP_SDR_PSRAM        Z_PL_ERR(Z_PL_KIND_MSG)
#define Z_PL_CA_ESP_SDR_PSRAM_FILE   Z_PL_ERR(Z_PL_KIND_MSG)
#define Z_PL_CA_ESP_SDR_NOINIT       Z_PL_ERR(Z_PL_KIND_MSG)
#define Z_PL_CA_ESP_SDR_PSRAM_NOINIT Z_PL_ERR(Z_PL_KIND_MSG)

/* Data: the region's attribute after the flags' checks. */
#define ESP_SDR_DATA(cat, nm)                                                                      \
	Z_PL_DATA_AT(Z_PL_DATA_REGION(ESP_SDR_PLACE_##cat, _##cat, ESP_SDR_PIN_DATA_##nm, _##nm),  \
		     __NONE)
#define ESP_SDR_DATA_F(cat, nm, f)                                                                 \
	Z_PL_DATA_AT(Z_PL_DATA_REGION(ESP_SDR_PLACE_##cat, _##cat, ESP_SDR_PIN_DATA_##nm, _##nm),  \
		     __##f)
#define ESP_SDR_DATA_F2(cat, nm, f1, f2)                                                           \
	Z_PL_DATA_AT2(Z_PL_DATA_REGION(ESP_SDR_PLACE_##cat, _##cat, ESP_SDR_PIN_DATA_##nm, _##nm), \
		      __##f1, __##f2)
#define Z_PL_DATA_AT(r, ff)          Z_PL_DATA_AT_(r, ff)
#define Z_PL_DATA_AT_(r, ff)         Z_PL_CAT(Z_PL_CK_, Z_PL_CAT(r, ff)) Z_PL_CAT(Z_PL_DA_, r)
#define Z_PL_DATA_AT2(r, f1, f2)     Z_PL_DATA_AT2_(r, f1, f2)
#define Z_PL_DATA_AT2_(r, f1, f2)                                                                  \
	Z_PL_CAT(Z_PL_CK_, Z_PL_CAT(r, f1))                                                        \
	Z_PL_CAT(Z_PL_CK2_, Z_PL_CAT(r, f2)) Z_PL_CAT(Z_PL_DA_, r)

#define Z_PL_DA_ESP_SDR_DRAM         DRAM_ATTR
#define Z_PL_DA_ESP_SDR_DEFAULT
#define Z_PL_DA_ESP_SDR_HIGH         ESP_SDR_HIGH_RAM
#define Z_PL_DA_ESP_SDR_PSRAM        EXT_RAM_BSS_ATTR
#define Z_PL_DA_ESP_SDR_PSRAM_FILE   __attribute__((section(".ext_ram.bss")))
#define Z_PL_DA_ESP_SDR_NOINIT       __noinit
#define Z_PL_DA_ESP_SDR_PSRAM_NOINIT EXT_RAM_NOINIT_ATTR
#define Z_PL_DA_Z_PL_NORULE
#define Z_PL_DA_ESP_SDR_IRAM
#define Z_PL_DA_ESP_SDR_HOTBLOCK
#define Z_PL_DA_ESP_SDR_FLASH

/* Flag checks: region__flag, NONE for no flag. */
#define Z_PL_DMA_MSG      "esp_sdr_place: DMA data outside internal RAM"
#define Z_PL_ATOMIC_MSG   "esp_sdr_place: ATOMIC data in PSRAM"
#define Z_PL_NOINIT_MSG   "esp_sdr_place: data in a region not zeroed at boot needs NOINIT"
#define Z_PL_CK_Z_PL_NORULE__NONE               Z_PL_ERR(Z_PL_NORULE_MSG)
#define Z_PL_CK_Z_PL_NORULE__DMA                Z_PL_ERR(Z_PL_NORULE_MSG)
#define Z_PL_CK_Z_PL_NORULE__ATOMIC             Z_PL_ERR(Z_PL_NORULE_MSG)
#define Z_PL_CK_Z_PL_NORULE__NOINIT             Z_PL_ERR(Z_PL_NORULE_MSG)
#define Z_PL_CK_ESP_SDR_IRAM__NONE              Z_PL_ERR(Z_PL_KIND_MSG)
#define Z_PL_CK_ESP_SDR_IRAM__DMA               Z_PL_ERR(Z_PL_KIND_MSG)
#define Z_PL_CK_ESP_SDR_IRAM__ATOMIC            Z_PL_ERR(Z_PL_KIND_MSG)
#define Z_PL_CK_ESP_SDR_IRAM__NOINIT            Z_PL_ERR(Z_PL_KIND_MSG)
#define Z_PL_CK_ESP_SDR_HOTBLOCK__NONE          Z_PL_ERR(Z_PL_KIND_MSG)
#define Z_PL_CK_ESP_SDR_HOTBLOCK__DMA           Z_PL_ERR(Z_PL_KIND_MSG)
#define Z_PL_CK_ESP_SDR_HOTBLOCK__ATOMIC        Z_PL_ERR(Z_PL_KIND_MSG)
#define Z_PL_CK_ESP_SDR_HOTBLOCK__NOINIT        Z_PL_ERR(Z_PL_KIND_MSG)
#define Z_PL_CK_ESP_SDR_FLASH__NONE             Z_PL_ERR(Z_PL_KIND_MSG)
#define Z_PL_CK_ESP_SDR_FLASH__DMA              Z_PL_ERR(Z_PL_KIND_MSG)
#define Z_PL_CK_ESP_SDR_FLASH__ATOMIC           Z_PL_ERR(Z_PL_KIND_MSG)
#define Z_PL_CK_ESP_SDR_FLASH__NOINIT           Z_PL_ERR(Z_PL_KIND_MSG)
#define Z_PL_CK_ESP_SDR_DRAM__NONE
#define Z_PL_CK_ESP_SDR_DRAM__DMA
#define Z_PL_CK_ESP_SDR_DRAM__ATOMIC
#define Z_PL_CK_ESP_SDR_DRAM__NOINIT
#define Z_PL_CK_ESP_SDR_DEFAULT__NONE
#define Z_PL_CK_ESP_SDR_DEFAULT__DMA
#define Z_PL_CK_ESP_SDR_DEFAULT__ATOMIC
#define Z_PL_CK_ESP_SDR_DEFAULT__NOINIT
#define Z_PL_CK_ESP_SDR_HIGH__NONE              Z_PL_ERR(Z_PL_NOINIT_MSG)
#define Z_PL_CK_ESP_SDR_HIGH__DMA               Z_PL_ERR(Z_PL_NOINIT_MSG)
#define Z_PL_CK_ESP_SDR_HIGH__ATOMIC            Z_PL_ERR(Z_PL_NOINIT_MSG)
#define Z_PL_CK_ESP_SDR_HIGH__NOINIT
#define Z_PL_CK_ESP_SDR_PSRAM__NONE
#define Z_PL_CK_ESP_SDR_PSRAM__DMA              Z_PL_ERR(Z_PL_DMA_MSG)
#define Z_PL_CK_ESP_SDR_PSRAM__ATOMIC           Z_PL_ERR(Z_PL_ATOMIC_MSG)
#define Z_PL_CK_ESP_SDR_PSRAM__NOINIT
#define Z_PL_CK_ESP_SDR_PSRAM_FILE__NONE
#define Z_PL_CK_ESP_SDR_PSRAM_FILE__DMA         Z_PL_ERR(Z_PL_DMA_MSG)
#define Z_PL_CK_ESP_SDR_PSRAM_FILE__ATOMIC      Z_PL_ERR(Z_PL_ATOMIC_MSG)
#define Z_PL_CK_ESP_SDR_PSRAM_FILE__NOINIT
#define Z_PL_CK_ESP_SDR_NOINIT__NONE            Z_PL_ERR(Z_PL_NOINIT_MSG)
#define Z_PL_CK_ESP_SDR_NOINIT__DMA             Z_PL_ERR(Z_PL_NOINIT_MSG)
#define Z_PL_CK_ESP_SDR_NOINIT__ATOMIC          Z_PL_ERR(Z_PL_NOINIT_MSG)
#define Z_PL_CK_ESP_SDR_NOINIT__NOINIT
#define Z_PL_CK_ESP_SDR_PSRAM_NOINIT__NONE      Z_PL_ERR(Z_PL_NOINIT_MSG)
#define Z_PL_CK_ESP_SDR_PSRAM_NOINIT__DMA       Z_PL_ERR(Z_PL_DMA_MSG)
#define Z_PL_CK_ESP_SDR_PSRAM_NOINIT__ATOMIC    Z_PL_ERR(Z_PL_ATOMIC_MSG)
#define Z_PL_CK_ESP_SDR_PSRAM_NOINIT__NOINIT

/* A second flag: checked as the first, without asking for NOINIT again. */
#define Z_PL_CK2_ESP_SDR_DRAM__DMA
#define Z_PL_CK2_ESP_SDR_DRAM__ATOMIC
#define Z_PL_CK2_ESP_SDR_DRAM__NOINIT
#define Z_PL_CK2_ESP_SDR_DEFAULT__DMA
#define Z_PL_CK2_ESP_SDR_DEFAULT__ATOMIC
#define Z_PL_CK2_ESP_SDR_DEFAULT__NOINIT
#define Z_PL_CK2_ESP_SDR_HIGH__DMA
#define Z_PL_CK2_ESP_SDR_HIGH__ATOMIC
#define Z_PL_CK2_ESP_SDR_HIGH__NOINIT
#define Z_PL_CK2_ESP_SDR_PSRAM__DMA             Z_PL_ERR(Z_PL_DMA_MSG)
#define Z_PL_CK2_ESP_SDR_PSRAM__ATOMIC          Z_PL_ERR(Z_PL_ATOMIC_MSG)
#define Z_PL_CK2_ESP_SDR_PSRAM__NOINIT
#define Z_PL_CK2_ESP_SDR_PSRAM_FILE__DMA        Z_PL_ERR(Z_PL_DMA_MSG)
#define Z_PL_CK2_ESP_SDR_PSRAM_FILE__ATOMIC     Z_PL_ERR(Z_PL_ATOMIC_MSG)
#define Z_PL_CK2_ESP_SDR_PSRAM_FILE__NOINIT
#define Z_PL_CK2_ESP_SDR_NOINIT__DMA
#define Z_PL_CK2_ESP_SDR_NOINIT__ATOMIC
#define Z_PL_CK2_ESP_SDR_NOINIT__NOINIT
#define Z_PL_CK2_ESP_SDR_PSRAM_NOINIT__DMA      Z_PL_ERR(Z_PL_DMA_MSG)
#define Z_PL_CK2_ESP_SDR_PSRAM_NOINIT__ATOMIC   Z_PL_ERR(Z_PL_ATOMIC_MSG)
#define Z_PL_CK2_ESP_SDR_PSRAM_NOINIT__NOINIT

/* `static type name dims __aligned(align)` in its category's region (ESP_SDR_*_BUF_DEFINE). */
#define ESP_SDR_BUF_DEFINE(cat, type, nm, dims, align, f)                                          \
	Z_PL_BUF_AT(Z_PL_DATA_REGION(ESP_SDR_PLACE_##cat, _##cat, ESP_SDR_PIN_DATA_##nm, _##nm),   \
		    __##f, type, nm, dims, align)
#define Z_PL_BUF_AT(r, ff, type, nm, dims, align) Z_PL_BUF_AT_(r, ff, type, nm, dims, align)
#define Z_PL_BUF_AT_(r, ff, type, nm, dims, align)                                                 \
	Z_PL_CAT(Z_PL_CK_, Z_PL_CAT(r, ff)) Z_PL_CAT(Z_PL_BUF_, r)(type, nm, dims, align)
#define Z_PL_BUF_ESP_SDR_HIGH(type, nm, dims, align)  ESP_SDR_HIGH_BUF_DEFINE(type, nm, dims, align)
#define Z_PL_BUF_ESP_SDR_PSRAM(type, nm, dims, align)                                              \
	ESP_SDR_PSRAM_BUF_DEFINE(type, nm, dims, align)
#define Z_PL_BUF_ESP_SDR_DEFAULT(type, nm, dims, align) static type nm dims __aligned(align)
#define Z_PL_BUF_ESP_SDR_DRAM(type, nm, dims, align)                                               \
	static type nm dims __aligned(align) DRAM_ATTR
#define Z_PL_BUF_ESP_SDR_PSRAM_FILE(type, nm, dims, align)                                         \
	static type nm dims __aligned(align) Z_PL_DA_ESP_SDR_PSRAM_FILE
#define Z_PL_BUF_ESP_SDR_NOINIT(type, nm, dims, align) static type nm dims __aligned(align) __noinit
#define Z_PL_BUF_ESP_SDR_PSRAM_NOINIT(type, nm, dims, align)                                       \
	static type nm dims __aligned(align) EXT_RAM_NOINIT_ATTR

/* A kernel thread stack, `static` like ESP_SDR_HIGH_STACK_DEFINE(). */
#define ESP_SDR_STACK_DEFINE(nm, size)                                                             \
	Z_PL_STACK_AT(Z_PL_DATA_REGION(ESP_SDR_PLACE_STACK, _STACK, ESP_SDR_PIN_DATA_##nm, _##nm), \
		      nm, size)
#define Z_PL_STACK_AT(r, nm, size)     Z_PL_STACK_AT_(r, nm, size)
#define Z_PL_STACK_AT_(r, nm, size)    Z_PL_CAT(Z_PL_STACK_, r)(nm, size)
#define Z_PL_STACK_ESP_SDR_HIGH(nm, size)    ESP_SDR_HIGH_STACK_DEFINE(nm, size)
#define Z_PL_STACK_ESP_SDR_NOINIT(nm, size)  static K_KERNEL_STACK_DEFINE(nm, size)
#define Z_PL_STACK_ESP_SDR_DEFAULT(nm, size) static K_KERNEL_STACK_DEFINE(nm, size)
#define Z_PL_STACK_ESP_SDR_PSRAM(nm, size)                                                         \
	static Z_KERNEL_STACK_DEFINE_IN(nm, size, EXT_RAM_BSS_ATTR)
#define Z_PL_STACK_ESP_SDR_PSRAM_FILE(nm, size)                                                    \
	static Z_KERNEL_STACK_DEFINE_IN(nm, size, Z_PL_DA_ESP_SDR_PSRAM_FILE)
#define Z_PL_STACK_Z_PL_NORULE(nm, size) Z_PL_ERR(Z_PL_NORULE_MSG)

#else /* __ASSEMBLER__ */

/* `.section` line of an assembler function: the region's section, today's name kept. */
#define ESP_SDR_ASM_SECTION(cat, fn)     Z_PL_ASM_##cat(ESP_SDR_PIN_CODE_##fn, _##fn, fn)
#define Z_PL_ASM_NOCACHE(pin, _fn, fn)   Z_PL_ASM_ESP_SDR_IRAM(fn)
#define Z_PL_ASM_COLD(pin, _fn, fn)      Z_PL_ASM_ESP_SDR_FLASH(fn)
#define Z_PL_ASM_PACED_SAMPLE(pin, _fn, fn)                                                        \
	Z_PL_ASM_AT(Z_PL_CODE_REGION(ESP_SDR_PLACE_PACED_SAMPLE, _PACED_SAMPLE, pin, _fn), fn)
#define Z_PL_ASM_PACED_CHUNK(pin, _fn, fn)                                                         \
	Z_PL_ASM_AT(Z_PL_CODE_REGION(ESP_SDR_PLACE_PACED_CHUNK, _PACED_CHUNK, pin, _fn), fn)
#define Z_PL_ASM_PACED_EVENT(pin, _fn, fn)                                                         \
	Z_PL_ASM_AT(Z_PL_CODE_REGION(ESP_SDR_PLACE_PACED_EVENT, _PACED_EVENT, pin, _fn), fn)
#define Z_PL_ASM_SAMPLE(pin, _fn, fn)                                                              \
	Z_PL_ASM_AT(Z_PL_CODE_REGION(ESP_SDR_PLACE_SAMPLE, _SAMPLE, pin, _fn), fn)
#define Z_PL_ASM_CHUNK(pin, _fn, fn)                                                               \
	Z_PL_ASM_AT(Z_PL_CODE_REGION(ESP_SDR_PLACE_CHUNK, _CHUNK, pin, _fn), fn)
#define Z_PL_ASM_EVENT(pin, _fn, fn)                                                               \
	Z_PL_ASM_AT(Z_PL_CODE_REGION(ESP_SDR_PLACE_EVENT, _EVENT, pin, _fn), fn)
#define Z_PL_ASM_AT(r, fn)               Z_PL_ASM_AT_(r, fn)
#define Z_PL_ASM_AT_(r, fn)              Z_PL_CAT(Z_PL_ASM_, r)(fn)
#define Z_PL_ASM_ESP_SDR_IRAM(fn)        .section Z_PL_STR(.iram1.fn), "ax"
#define Z_PL_ASM_ESP_SDR_FLASH(fn)       .section Z_PL_STR(.text.fn), "ax"
#if defined(CONFIG_ESP_SDR_HOT_TEXT)
#define Z_PL_ASM_ESP_SDR_HOTBLOCK(fn)    .section Z_PL_STR(.text.esp_sdr_hot.fn), "ax"
#else
#define Z_PL_ASM_ESP_SDR_HOTBLOCK(fn)    .section Z_PL_STR(.text.fn), "ax"
#endif
#define Z_PL_ASM_Z_PL_NORULE(fn)         Z_PL_ERR(Z_PL_NORULE_MSG)

#endif /* __ASSEMBLER__ */

#else /* !CONFIG_SOC_FAMILY_ESPRESSIF_ESP32: nothing is placed */

#define ESP_SDR_CODE(cat, fn)
#define ESP_SDR_DATA(cat, nm)
#define ESP_SDR_DATA_F(cat, nm, f)
#define ESP_SDR_DATA_F2(cat, nm, f1, f2)
#define ESP_SDR_BUF_DEFINE(cat, type, nm, dims, align, f) static type nm dims __aligned(align)
#define ESP_SDR_STACK_DEFINE(nm, size)                    static K_KERNEL_STACK_DEFINE(nm, size)
#define ESP_SDR_ASM_SECTION(cat, fn)                      .section .text, "ax"
#define ESP_SDR_REGION_CODE(cat, fn)                      ESP_SDR_FLASH

#endif /* CONFIG_SOC_FAMILY_ESPRESSIF_ESP32 */

/** @} */

#endif /* ESP_SDR_PLACE_H_ */
