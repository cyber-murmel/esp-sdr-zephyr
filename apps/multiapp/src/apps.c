/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Runs one app per boot as an llext extension (doc/multiapp.md): loads it
 * into PSRAM, runs its constructors (include/multiapp/ext.h) and starts its
 * main() in a thread. Starting another app reboots into it.
 */

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <zephyr/app_version.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/llext/buf_loader.h>
#include <zephyr/llext/llext.h>
#include <zephyr/logging/log.h>
#include <zephyr/shell/shell.h>
#include <zephyr/shell/shell_uart.h>
#include <zephyr/sys/crc.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/usb/usbd.h>

#include <esp_attr.h>
#include <esp_rom_sys.h>
#include <rom/cache.h>
#include <soc/soc.h>

#include <esp_sdr/esp_sdr.h>
#include <esp_sdr/esp_sdr_place.h>
#if defined(CONFIG_ESP_SDR_IEEE802154)
#include <esp_sdr/ieee154_esp_sdr.h>
#endif

#include <multiapp/multiapp.h>

#include "multiapp_build_id.h"

#include "app_crash.h"
#include "app_usb.h"
#include "apps.h"

LOG_MODULE_REGISTER(multiapp, LOG_LEVEL_INF);

/* One MMU table maps both buses: PSRAM at 0x3c000000 + x executes at 0x42000000 + x. */
#define IBUS_DBUS_OFFSET 0x06000000

#define ROOTS_MAX       4
#define MODULES_MAX     8
#define REQUEST_MAGIC   0x6d617070U /* "mapp" */
#define TRIED_MAGIC     0x74726964U /* "trid": the boot that tried it has started */
#define REBOOT_DELAY_MS 100

/* No init: at boot, zeroing what llext zeroes as needed would cost every boot its time. */
static ESP_SDR_DATA_F(BULK, instr_heap, NOINIT) uint8_t
	instr_heap[KB(CONFIG_MULTIAPP_INSTR_HEAP_KB)] __aligned(64);
static ESP_SDR_DATA_F(BULK, data_heap, NOINIT) uint8_t
	data_heap[KB(CONFIG_MULTIAPP_DATA_HEAP_KB)] __aligned(64);

/*
 * For the modules placed as a standalone image: their IRAM_ATTR code at the instruction
 * bus alias of its data bus address, as for PSRAM. The rest stays in the default heaps.
 */
static uint8_t module_heap_mem[CONFIG_MULTIAPP_MODULE_HEAP_SIZE]
	ESP_SDR_DATA_F(STATE, module_heap_mem, NOINIT) __aligned(16);
static struct k_heap module_heap;
/* What the high region has left: for the bss of the module that fills the ring's banks. */
static uint8_t module_high_mem[CONFIG_MULTIAPP_MODULE_HIGH_HEAP_SIZE] __aligned(16)
	ESP_SDR_DATA_F(STATE, module_high_mem, NOINIT);
static struct k_heap module_high_heap;

static const struct llext_region_heap region_heaps[][LLEXT_MEM_COUNT] = {
	[MULTIAPP_PLACE_INTERNAL] = {
		[LLEXT_MEM_TEXT_ALT] = {&module_heap, SOC_I_D_OFFSET},
		[LLEXT_MEM_DATA] = {&module_heap, 0},
		[LLEXT_MEM_BSS] = {&module_heap, 0},
	},
	[MULTIAPP_PLACE_INTERNAL_HIGH_BSS] = {
		[LLEXT_MEM_TEXT_ALT] = {&module_heap, SOC_I_D_OFFSET},
		[LLEXT_MEM_DATA] = {&module_heap, 0},
		[LLEXT_MEM_BSS] = {&module_high_heap, 0},
	},
};

/* The modules this boot has loaded, in the order they came. */
static struct {
	const struct multiapp_embedded *def;
	struct llext *ext;
	const char *source;
} modules[MODULES_MAX];
static size_t module_count;

/* PSRAM like the app's data: internal RAM is kept for the dump banks. */
static Z_KERNEL_STACK_DEFINE_IN(app_stack, CONFIG_MULTIAPP_STACK_SIZE,
				ESP_SDR_DATA_F(STACK, app_stack, NOINIT));
static struct k_thread app_thread;

static ESP_SDR_DATA(BULK, loader) struct llext_buf_loader loader;
static struct llext *loaded;
static const struct multiapp_info *loaded_info;
static const char *loaded_source; /* "file" or "built-in" */
/* The app's main() waits for multiapp_usb_up(). */
static bool main_held;

/* What this host's apps carry in their trailers (buildid.py). */
static const uint8_t build_id[] = MULTIAPP_BUILD_ID;

/*
 * The app's file, read whole before llext sees it (drive.c): the top of PSRAM, above the shared
 * heap the linker reserves (multiapp_bank0.ld checks there is room), so that it is one block with
 * what multiapp_psram_spare() hands out.
 */
extern char _ext_ram_start[], _ext_ram_heap_start[];
#define FILE_BUF_SIZE KB(CONFIG_MULTIAPP_FILE_MAX_KB)
#define PSRAM_TOP     ((uintptr_t)_ext_ram_start + CONFIG_ESP_SPIRAM_SIZE)
#define FILE_BUF      ((uint8_t *)(PSRAM_TOP - FILE_BUF_SIZE))
static const struct shell_static_entry *roots[ROOTS_MAX];
static size_t root_count;

/* The app for the next boot and how far its boot got; survives a software reset. */
static RTC_NOINIT_ATTR struct {
	uint32_t magic;
	char name[24];
	uint32_t step;
} request;

enum boot_step {
	STEP_REQUESTED,
	STEP_LOADING,
	STEP_CONSTRUCTORS,
	STEP_MAIN,
	STEP_RUNNING,
	/* It did not load and said so, as with a file from another build: safe to try again. */
	STEP_REFUSED,
};

static const char *const step_names[] = {"requested", "loading", "constructors", "main",
					 "running", "refused"};

/* What the previous boot left: the request it tried and the step it reached. */
static char tried_name[sizeof(request.name)];
static uint32_t tried_step;
static bool tried;

/* This boot's app, from a request or CONFIG_MULTIAPP_AUTOSTART, and its USB profile. */
static char boot_name[sizeof(request.name)];
static uint32_t usb_profile;
static int boot_ret;
/* How long it took: the modules it needs, with the radio coming up, then the app itself. */
static uint32_t modules_ms, app_ms;

/*
 * Internal RAM for one user per boot: the boot's app if it has buffers there
 * (ESP_SDR_HIGH_BUF_DEFINE(), exports.py), else the 802.15.4 driver.
 */
uint8_t multiapp_area[CONFIG_MULTIAPP_AREA_SIZE] __aligned(16)
	ESP_SDR_DATA_F(STATE, multiapp_area, NOINIT);
static const struct multiapp_embedded *area_app;

#if defined(CONFIG_ESP_SDR_IEEE802154_HIGH_RAM_APP)
void *ieee154_esp_sdr_high_ram(size_t size)
{
	if (area_app != NULL || size > sizeof(multiapp_area)) {
		return NULL;
	}
	return multiapp_area;
}
#endif

extern int do_device_init(const struct device *dev);

/* Before the devices: the area's user and the USB profile follow the boot's app. */
static int boot_pick(void)
{
	const struct multiapp_embedded *app;

	if (request.magic == TRIED_MAGIC) {
		/* The boot that tried an app ended before "multiapp run" or a reset: report it. */
		tried = true;
		tried_step = request.step;
		memcpy(tried_name, request.name, sizeof(tried_name));
		tried_name[sizeof(tried_name) - 1] = '\0';
		request.magic = 0;
	}
	if (request.magic == REQUEST_MAGIC) {
		request.magic = TRIED_MAGIC;
		request.step = STEP_REQUESTED;
		memcpy(boot_name, request.name, sizeof(boot_name));
		boot_name[sizeof(boot_name) - 1] = '\0';
	} else if (!(tried && tried_step < STEP_RUNNING &&
		     strcmp(tried_name, CONFIG_MULTIAPP_AUTOSTART) == 0)) {
		/* Tried like a request, so one that died before running is skipped once. */
		strncpy(boot_name, CONFIG_MULTIAPP_AUTOSTART, sizeof(boot_name) - 1);
		if (boot_name[0] != '\0') {
			request.magic = TRIED_MAGIC;
			request.step = STEP_REQUESTED;
			memcpy(request.name, boot_name, sizeof(request.name));
		}
	}
	app = multiapp_embedded_find(boot_name);
	usb_profile = app != NULL ? app->usb : MULTIAPP_USB_MSC;
	area_app = app != NULL && app->area ? app : NULL;
	return 0;
}
SYS_INIT(boot_pick, PRE_KERNEL_1, 0);

static int heaps_init(void)
{
	k_heap_init(&module_heap, module_heap_mem, sizeof(module_heap_mem));
	k_heap_init(&module_high_heap, module_high_mem, sizeof(module_high_mem));
	return llext_heap_init_harvard_exec(instr_heap, sizeof(instr_heap), data_heap,
					    sizeof(data_heap), IBUS_DBUS_OFFSET);
}
SYS_INIT(heaps_init, APPLICATION, 0);

/* Only the boot's app's classes: the OTG controller has too few IN endpoints for all. */
void app_usb_classes(struct usbd_context *ctx)
{
	if (IS_ENABLED(CONFIG_USBD_CDC_NCM_CLASS) && (usb_profile & MULTIAPP_USB_NCM) == 0U) {
		(void)usbd_unregister_class(ctx, "cdc_ncm_0", USBD_SPEED_FS, 1);
	}
	if ((usb_profile & MULTIAPP_USB_VENDOR) == 0U) {
		(void)usbd_unregister_class(ctx, "esp_sdr_usb", USBD_SPEED_FS, 1);
	}
	if (IS_ENABLED(CONFIG_USBD_MSC_CLASS) && (usb_profile & MULTIAPP_USB_MSC) == 0U) {
		(void)usbd_unregister_class(ctx, "msc_0", USBD_SPEED_FS, 1);
	}
}

void multiapp_shell_register(const struct shell_static_entry *root)
{
	if (root_count == ARRAY_SIZE(roots)) {
		LOG_ERR("no room for root command %s", root->syntax);
		return;
	}
	roots[root_count++] = root;
}

static const struct shell_static_entry *root_find(const char *name)
{
	for (size_t i = 0; i < root_count; i++) {
		if (strcmp(roots[i]->syntax, name) == 0) {
			return roots[i];
		}
	}
	return NULL;
}

const struct multiapp_embedded *multiapp_embedded_find(const char *name)
{
	for (size_t i = 0; i < multiapp_embedded_count; i++) {
		if (strcmp(multiapp_embedded[i].name, name) == 0) {
			return &multiapp_embedded[i];
		}
	}
	return NULL;
}

/* llext wrote the code through the data bus, and the ESP cache driver has no icache ops. */
static void sync_text(const struct llext *ext)
{
	uintptr_t text = (uintptr_t)ext->mem[LLEXT_MEM_TEXT];
	size_t size = ext->mem_size[LLEXT_MEM_TEXT];

	Cache_WriteBack_Addr(text, size);
	Cache_Invalidate_Addr(text + IBUS_DBUS_OFFSET, size);
}

/* Record how far this boot's app got, until the next "multiapp run" or reset. */
static void boot_step(enum boot_step step)
{
	if (request.magic == TRIED_MAGIC) {
		request.step = step;
	}
}

static void app_main(void *p1, void *p2, void *p3)
{
	int (*entry)(void) = p1;
	int ret;

	ARG_UNUSED(p2);
	ARG_UNUSED(p3);
	ret = entry();
	boot_step(STEP_RUNNING);
	LOG_INF("%s: main returned %d", loaded_info->name, ret);
}

/* The ELF size and stack of an app file whose trailer holds (apptrail.py); else -errno. */
static int check_trailer(const char *name, const char *source, const uint8_t *data, size_t len,
			 size_t *elf_size, uint32_t *stack_need)
{
	struct multiapp_trailer t;

	if (len < sizeof(t)) {
		LOG_ERR("%s (%s): %zu bytes, no trailer", name, source, len);
		return -ENOEXEC;
	}
	memcpy(&t, data + len - sizeof(t), sizeof(t));
	if (t.magic != MULTIAPP_TRAILER_MAGIC || t.elf_size != len - sizeof(t)) {
		LOG_ERR("%s (%s): no trailer for %zu bytes", name, source, len);
		return -ENOEXEC;
	}
	if (crc32_ieee(data, t.elf_size) != t.crc32) {
		LOG_ERR("%s (%s): CRC mismatch", name, source);
		return -EBADMSG;
	}
	BUILD_ASSERT(sizeof(build_id) == sizeof(t.build_id));
	if (memcmp(t.build_id, build_id, sizeof(build_id)) != 0) {
		LOG_ERR("%s (%s): built for host %02x%02x%02x%02x, this is %02x%02x%02x%02x", name,
			source, t.build_id[0], t.build_id[1], t.build_id[2], t.build_id[3],
			build_id[0], build_id[1], build_id[2], build_id[3]);
		return -ENOEXEC;
	}
	*elf_size = t.elf_size;
	*stack_need = t.stack_size;
	return 0;
}

/* Read one extension, from its file if the volume has a good one, and load it. */
static int load_extension(const struct multiapp_embedded *app, struct llext **ext,
			  const char **source, uint32_t *stack_need)
{
	struct llext_load_param param = LLEXT_LOAD_PARAM_DEFAULT;
	const uint8_t *data;
	size_t elf_size;
	ssize_t n;
	int ret;

	n = multiapp_drive_status() == 0 ? multiapp_drive_read(app->name, FILE_BUF, FILE_BUF_SIZE)
					 : multiapp_drive_status();
	if (n >= 0 && check_trailer(app->name, "file", FILE_BUF, n, &elf_size, stack_need) == 0) {
		data = FILE_BUF;
		*source = "file";
	} else if (app->elf != NULL &&
		   check_trailer(app->name, "built-in", app->elf, app->size, &elf_size,
				 stack_need) == 0) {
		if (n != -ENOENT && n != -ENODEV) {
			LOG_WRN("%s: the file is not usable (%d), the built-in copy runs",
				app->name, (int)n);
		}
		data = app->elf;
		*source = "built-in";
	} else {
		LOG_ERR("%s: no usable file (%d)%s", app->name, (int)n,
			app->elf != NULL ? " or built-in copy" : "");
		return n < 0 ? (int)n : -ENOEXEC;
	}
	if (app->place != MULTIAPP_PLACE_PSRAM) {
		param.region_heaps = region_heaps[app->place];
		param.text_alt_section = ".module_fast";
	}
	/*
	 * Temporary, so llext copies every region: in place, it would not relocate
	 * .got.loc, as .rela.dyn names no target section.
	 */
	loader = (struct llext_buf_loader)LLEXT_TEMPORARY_BUF_LOADER(data, elf_size);
	ret = llext_load(&loader.loader, app->name, ext, &param);
	if (ret != 0) {
		LOG_ERR("%s: load failed (%d)", app->name, ret);
		*ext = NULL;
		return ret;
	}
	sync_text(*ext);
	return 0;
}

/* What a module's devices and SYS_INIT entries do at boot (ext/module_devices.c). */
static int module_init_table(const struct multiapp_embedded *def, struct llext *ext)
{
	const struct multiapp_init_table *table;
	int result = 0;

	if (!def->devices) {
		return 0;
	}
	/* Without this check, a lost table leaves devices that never initialized. */
	table = llext_find_sym(&ext->exp_tab, "multiapp_init_table");
	if (table == NULL || table->start == table->end) {
		LOG_ERR("%s: no init table", def->name);
		return -ENOENT;
	}
	for (const struct init_entry *entry = table->start; entry < table->end; entry++) {
		int ret = entry->dev != NULL ? do_device_init(entry->dev) : entry->init_fn();

		if (ret != 0) {
			LOG_ERR("%s: init failed (%d)", def->name, ret);
			result = result != 0 ? result : ret;
		}
	}
	return result;
}

static const struct device *table_find(const struct llext *ext, const char *name)
{
	const struct multiapp_init_table *table =
		llext_find_sym(&ext->exp_tab, "multiapp_init_table");

	if (table == NULL) {
		return NULL;
	}
	for (const struct init_entry *entry = table->start; entry < table->end; entry++) {
		if (entry->dev != NULL && strcmp(entry->dev->name, name) == 0) {
			return entry->dev;
		}
	}
	return NULL;
}

/* A device of a loaded module or app, which the kernel's own lookup cannot see. */
const struct device *multiapp_device_get_binding(const char *name)
{
	const struct device *dev = loaded != NULL ? table_find(loaded, name) : NULL;

	for (size_t i = 0; dev == NULL && i < module_count; i++) {
		dev = table_find(modules[i].ext, name);
	}
	if (dev != NULL) {
		return device_is_ready(dev) ? dev : NULL;
	}
	return device_get_binding(name);
}

/* The modules @p app needs, each after those it needs itself, up and running. */
static int load_modules(const struct multiapp_embedded *app)
{
	for (const char *const *req = app->requires; *req != NULL; req++) {
		const struct multiapp_embedded *def = multiapp_embedded_find(*req);
		bool have = false;
		uint32_t stack_need;
		int ret;

		for (size_t i = 0; i < module_count; i++) {
			have = have || modules[i].def == def;
		}
		if (have) {
			continue;
		}
		if (def == NULL || !def->module) {
			LOG_ERR("%s: no module %s", app->name, *req);
			return -ENOENT;
		}
		ret = load_modules(def);
		if (ret != 0) {
			return ret;
		}
		if (module_count == ARRAY_SIZE(modules)) {
			LOG_ERR("%s: more than %d modules", app->name, MODULES_MAX);
			return -ENOMEM;
		}
		ret = load_extension(def, &modules[module_count].ext,
				     &modules[module_count].source, &stack_need);
		if (ret != 0) {
			return ret;
		}
		struct llext *ext = modules[module_count].ext;
		int (*up)(void);

		modules[module_count].def = def;
		module_count++;
		ret = llext_bringup(ext);
		if (ret != 0) {
			LOG_ERR("%s: constructors failed (%d)", def->name, ret);
			return ret;
		}
		ret = module_init_table(def, ext);
		if (ret != 0) {
			return ret;
		}
		up = llext_find_sym(&ext->exp_tab, "multiapp_module_up");
		if (up != NULL) {
			ret = up();
			if (ret != 0) {
				LOG_ERR("%s: up failed (%d)", def->name, ret);
				return ret;
			}
		}
	}
	return 0;
}

/* The shell gives up the CDC-ACM port. USB has not started, so nobody is talking to it. */
static int console_take(void)
{
	const struct shell *sh = shell_backend_uart_get_ptr();

	shell_uninit(sh, NULL);
	return k_thread_join(sh->thread, K_SECONDS(1));
}

int multiapp_start(const char *name)
{
	const struct multiapp_embedded *app = multiapp_embedded_find(name);
	const struct multiapp_info *info;
	int (*entry)(void);
	size_t stack;
	uint32_t stack_need, started;
	int ret;

	if (app == NULL || app->module) {
		return -ENOENT;
	}
	if (loaded != NULL) {
		return -EBUSY;
	}
	boot_step(STEP_LOADING);
	started = k_uptime_get_32();
	ret = load_modules(app);
	if (ret != 0) {
		return ret;
	}
	modules_ms = k_uptime_get_32() - started;
	started = k_uptime_get_32();
	ret = load_extension(app, &loaded, &loaded_source, &stack_need);
	if (ret != 0) {
		loaded = NULL;
		return ret;
	}
	stack = K_THREAD_STACK_SIZEOF(app_stack);
	if (stack_need > stack) {
		LOG_ERR("%s: needs a %u byte stack, the host has %zu (CONFIG_MULTIAPP_STACK_SIZE)",
			name, stack_need, stack);
		ret = -ENOMEM;
		goto unload;
	}
	info = llext_find_sym(&loaded->exp_tab, "multiapp_info");
	if (info == NULL || info->abi != MULTIAPP_ABI) {
		LOG_ERR("%s: built for ABI %d, the host has %u", name,
			info != NULL ? (int)info->abi : -1, MULTIAPP_ABI);
		ret = -ENOEXEC;
		goto unload;
	}
	entry = info->main;
	if (entry == NULL) {
		LOG_ERR("%s: no main()", name);
		ret = -ENOEXEC;
		goto unload;
	}
	if (app->console) {
		ret = console_take();
		if (ret != 0) {
			LOG_ERR("%s: the shell keeps the console (%d)", name, ret);
			goto unload;
		}
	}
	loaded_info = info;
	boot_step(STEP_CONSTRUCTORS);
	ret = llext_bringup(loaded);
	if (ret != 0) {
		LOG_ERR("%s: constructors failed (%d)", name, ret);
		goto unload;
	}
	/* A device that fails stays not ready, as in a standalone image: the app can still run. */
	(void)module_init_table(app, loaded);
	app_ms = k_uptime_get_32() - started;
	LOG_INF("%s (%s): %zu bytes of code at %p, starting main()", name, loaded_source,
		loaded->mem_size[LLEXT_MEM_TEXT],
		(void *)((uintptr_t)loaded->mem[LLEXT_MEM_TEXT] + IBUS_DBUS_OFFSET));
	boot_step(STEP_MAIN);
	k_thread_create(&app_thread, app_stack, stack, app_main, entry, NULL, NULL,
			CONFIG_MAIN_THREAD_PRIORITY, 0, app->console ? K_FOREVER : K_NO_WAIT);
	k_thread_name_set(&app_thread, info->name);
	main_held = app->console;
	return 0;

unload:
	/* Nothing of it has run yet, so it can go. */
	loaded_info = NULL;
	root_count = 0;
	llext_unload(&loaded);
	loaded = NULL;
	return ret;
}

#if defined(CONFIG_APP_USB_DFU)
/*
 * Before a DFU download writes flash: the loaded app's own hook, by name
 * (wpan stops its radio, which keeps a CPU from taking flash stall requests).
 */
void app_usb_dfu_prepare(void)
{
	void (*prepare)(void);

	if (loaded == NULL) {
		return;
	}
	prepare = llext_find_sym(&loaded->exp_tab, "app_usb_dfu_prepare");
	if (prepare != NULL) {
		prepare();
	}
}
#endif

static void reboot_handler(struct k_work *work)
{
	ARG_UNUSED(work);
	sys_reboot(SYS_REBOOT_WARM);
}
static K_WORK_DELAYABLE_DEFINE(reboot_work, reboot_handler);

int multiapp_request(const char *name)
{
	const struct multiapp_embedded *app = multiapp_embedded_find(name);

	if (app == NULL) {
		return -ENOENT;
	}
	if (app->module) {
		return -EINVAL;
	}
	if (loaded == NULL && app->usb == usb_profile && (!app->area || area_app == app) &&
	    !app->console) {
		return multiapp_start(name);
	}
	/*
	 * An app cannot stop, USB enumerates once, the area has one user per boot, and the shell
	 * this runs on gives up the port before an app that wants it starts.
	 */
	request.magic = REQUEST_MAGIC;
	strncpy(request.name, name, sizeof(request.name) - 1);
	request.name[sizeof(request.name) - 1] = '\0';
	k_work_schedule(&reboot_work, K_MSEC(REBOOT_DELAY_MS));
	return 1;
}

bool multiapp_boot(void)
{
	int ret;

	if (boot_name[0] == '\0') {
		return false;
	}
	ret = multiapp_start(boot_name);
	if (ret != 0) {
		boot_step(STEP_REFUSED);
		LOG_ERR("%s: not started (%d)", boot_name, ret);
		/* USB has not started: it comes up without the app's classes, with the drive. */
		usb_profile = MULTIAPP_USB_MSC;
	} else if ((usb_profile & MULTIAPP_USB_MSC) != 0U) {
		multiapp_drive_app_disks();
	}
	/* Before the shell is open: "multiapp info" tells. */
	boot_ret = ret;
	return ret == 0;
}

void multiapp_usb_up(void)
{
	if (main_held) {
		main_held = false;
		k_thread_start(&app_thread);
	}
}

void *multiapp_psram_spare(size_t *size)
{
	*size = PSRAM_TOP - (uintptr_t)_ext_ram_heap_start;
	return _ext_ram_heap_start;
}

void *multiapp_app_fn(const char *name)
{
	return loaded != NULL ? (void *)llext_find_sym(&loaded->exp_tab, name) : NULL;
}

/* The host main()'s stack use, measured as it returns, and the CPU that started USB. */
static size_t main_used, main_size;
static int main_cpu = -1;

void multiapp_note_main_stack(void)
{
	main_cpu = arch_curr_cpu()->id;
#if defined(CONFIG_INIT_STACKS) && defined(CONFIG_THREAD_STACK_INFO)
	size_t unused;

	if (k_thread_stack_space_get(k_current_get(), &unused) == 0) {
		main_size = k_current_get()->stack_info.size;
		main_used = main_size - unused;
	}
#endif
}

/* An app's app_usb_init() (include/multiapp/ext.h): the host's main() starts USB. */
int multiapp_ext_usb_init(void)
{
	return 0;
}

/*
 * Proxy roots: the apps' root commands by name, so the shell looks the same
 * as in the standalone app. The subcommands come from the loaded app's table.
 */
static void proxy_get(const char *name, size_t idx, struct shell_static_entry *entry)
{
	const struct shell_static_entry *root = root_find(name);
	const struct shell_static_entry *sub;

	entry->syntax = NULL;
	if (root == NULL || root->subcmd == NULL) {
		return;
	}
	sub = root->subcmd->entry;
	for (size_t i = 0; i <= idx; i++) {
		if (sub[i].syntax == NULL) {
			return;
		}
	}
	*entry = sub[idx];
}

static int proxy_cmd(const char *name, const struct shell *sh, size_t argc, char **argv)
{
	const struct shell_static_entry *root = root_find(name);

	if (root == NULL) {
		shell_error(sh, "%s: no loaded app has this command (multiapp list)", name);
		return -ENOEXEC;
	}
	if (root->handler == NULL) {
		if (argc > 1) {
			shell_error(sh, "%s: unknown parameter: %s", name, argv[1]);
		}
		shell_help(sh);
		return argc > 1 ? -EINVAL : SHELL_CMD_HELP_PRINTED;
	}
	/* As the shell checks a static root: mandatory counts the command, 0 checks nothing. */
	if (root->args.mandatory != 0 &&
	    (argc < root->args.mandatory ||
	     (root->args.optional != SHELL_OPT_ARG_CHECK_SKIP &&
	      argc > (size_t)root->args.mandatory + root->args.optional))) {
		shell_error(sh, "%s: wrong parameter count", name);
		return -EINVAL;
	}
	return root->handler(sh, argc, argv);
}

#define PROXY(_name)                                                                               \
	static void UTIL_CAT(proxy_get_, _name)(size_t idx, struct shell_static_entry *entry)      \
	{                                                                                          \
		proxy_get(STRINGIFY(_name), idx, entry);                                           \
	}                                                                                          \
	SHELL_DYNAMIC_CMD_CREATE(UTIL_CAT(proxy_sub_, _name), UTIL_CAT(proxy_get_, _name));        \
	static int UTIL_CAT(proxy_cmd_, _name)(const struct shell *sh, size_t argc, char **argv)  \
	{                                                                                          \
		return proxy_cmd(STRINGIFY(_name), sh, argc, argv);                                \
	}                                                                                          \
	SHELL_CMD_ARG_REGISTER(_name, &UTIL_CAT(proxy_sub_, _name),                                \
			       "Command of the loaded app (multiapp list)", UTIL_CAT(proxy_cmd_, _name), \
			       1, SHELL_OPT_ARG_CHECK_SKIP)

/* Every root command the apps register: hello, rfprobe, wpan, osmosdr and stream, link. */
PROXY(hello);
PROXY(probe);
PROXY(wpan);
PROXY(sdr);
PROXY(link);
PROXY(ofdm);
#if defined(CONFIG_MULTIAPP_WIFI)
PROXY(wifi);
#endif

static int cmd_list(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	for (size_t i = 0; i < multiapp_embedded_count; i++) {
		const struct multiapp_embedded *app = &multiapp_embedded[i];
		char built_in[24] = "     -";
		char file[40] = "";
		ssize_t size;
		bool same;

		if (multiapp_drive_file(app->name, &size, &same) == 0) {
			if (size < 0) {
				(void)snprintf(file, sizeof(file), ", no file (%d)", (int)size);
			} else {
				(void)snprintf(file, sizeof(file), ", file %zd bytes%s", size,
					       same || app->elf == NULL ? "" : " (differs)");
			}
		}
		if (app->elf != NULL) {
			(void)snprintf(built_in, sizeof(built_in), "%6zu bytes", app->size);
		}
		shell_print(sh, "%-12s %s%s%s%s", app->name, built_in, file,
			    app->module ? ", module" : "",
			    loaded_info != NULL && strcmp(loaded_info->name, app->name) == 0
				    ? ", running"
				    : "");
	}
	if (multiapp_drive_status() != 0) {
		shell_print(sh, "no FAT volume (%d)", multiapp_drive_status());
	}
	return 0;
}

static int cmd_run(const struct shell *sh, size_t argc, char **argv)
{
	int ret = multiapp_request(argv[1]);

	ARG_UNUSED(argc);
	if (ret == -ENOENT) {
		shell_error(sh, "%s: no such app (multiapp list)", argv[1]);
	} else if (ret == -EINVAL) {
		shell_error(sh, "%s: a module, it loads with the apps that need it", argv[1]);
	} else if (ret == 1) {
		shell_print(sh, "%s: rebooting into it%s", argv[1],
			    (usb_profile & MULTIAPP_USB_MSC) != 0U
				    ? " (eject the drive first: the PC may not have written it out)"
				    : "");
		ret = 0;
	} else if (ret != 0) {
		shell_error(sh, "%s: failed (%d)", argv[1], ret);
	} else {
		shell_print(sh, "%s: started", argv[1]);
	}
	return ret;
}

static int cmd_info(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	shell_print(sh, "host %s, build %02x%02x%02x%02x", APP_VERSION_STRING, build_id[0],
		    build_id[1], build_id[2], build_id[3]);
	if (main_size != 0U) {
		shell_print(sh, "host main: %zu of %zu bytes of stack used, USB started on CPU %d",
			    main_used, main_size, main_cpu);
	}
	if (boot_name[0] != '\0') {
		shell_print(sh, "boot: %s, start %d, USB profile 0x%x, modules %u ms, app %u ms",
			    boot_name, boot_ret, usb_profile, modules_ms, app_ms);
	}
	for (size_t i = 0; i < module_count; i++) {
		const struct llext *ext = modules[i].ext;

		shell_print(sh, "module %s (%s): text %zu at %p, fast %zu at %p, data %zu at %p, "
			    "bss %zu at %p, rodata %zu",
			    modules[i].def->name, modules[i].source,
			    ext->mem_size[LLEXT_MEM_TEXT],
			    (void *)((uintptr_t)ext->mem[LLEXT_MEM_TEXT] +
				     ext->mem_exec_offset[LLEXT_MEM_TEXT]),
			    ext->mem_size[LLEXT_MEM_TEXT_ALT],
			    (void *)((uintptr_t)ext->mem[LLEXT_MEM_TEXT_ALT] +
				     ext->mem_exec_offset[LLEXT_MEM_TEXT_ALT]),
			    ext->mem_size[LLEXT_MEM_DATA], ext->mem[LLEXT_MEM_DATA],
			    ext->mem_size[LLEXT_MEM_BSS], ext->mem[LLEXT_MEM_BSS],
			    ext->mem_size[LLEXT_MEM_RODATA]);
	}
#if defined(CONFIG_SYS_HEAP_RUNTIME_STATS)
	struct sys_memory_stats heap_stats;

	if (sys_heap_runtime_stats_get(&module_heap.heap, &heap_stats) == 0) {
		shell_print(sh, "module heap: %zu of %zu bytes used", heap_stats.allocated_bytes,
			    heap_stats.allocated_bytes + heap_stats.free_bytes);
	}
	if (sys_heap_runtime_stats_get(&module_high_heap.heap, &heap_stats) == 0) {
		shell_print(sh, "module high heap: %zu of %zu bytes used",
			    heap_stats.allocated_bytes,
			    heap_stats.allocated_bytes + heap_stats.free_bytes);
	}
#endif
	if (loaded == NULL) {
		shell_print(sh, "no app loaded");
		return 0;
	}
	shell_print(sh, "%s (%s): text %zu at %p, data %zu, bss %zu, rodata %zu, %zu bytes in heaps",
		    loaded_info->name, loaded_source, loaded->mem_size[LLEXT_MEM_TEXT],
		    (void *)((uintptr_t)loaded->mem[LLEXT_MEM_TEXT] + IBUS_DBUS_OFFSET),
		    loaded->mem_size[LLEXT_MEM_DATA], loaded->mem_size[LLEXT_MEM_BSS],
		    loaded->mem_size[LLEXT_MEM_RODATA], loaded->alloc_size);
	for (size_t i = 0; i < root_count; i++) {
		shell_print(sh, "  command %s", roots[i]->syntax);
	}
	return 0;
}

/* What ended the previous boot, if it ended badly (apps/common app_crash.h). */
static int cmd_crash(const struct shell *sh, size_t argc, char **argv)
{
	size_t used, size;

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	shell_print(sh, "reset reason cpu0 %d cpu1 %d (esp_rom_get_reset_reason())",
		    (int)esp_rom_get_reset_reason(0), (int)esp_rom_get_reset_reason(1));
	if (tried) {
		shell_print(sh, "the boot into %s ended at step %s", tried_name,
			    tried_step < ARRAY_SIZE(step_names) ? step_names[tried_step] : "?");
	}
	if (app_usb_dfu_stack(&used, &size)) {
		shell_print(sh, "last DFU download: %zu of %zu bytes of the USB thread's stack used",
			    used, size);
	}
	if (!app_crash_print(sh)) {
		shell_print(sh, "no fatal error or watchdog reset recorded");
	}
	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(app_cmds,
	SHELL_CMD(list, NULL, "The apps in this image", cmd_list),
	SHELL_CMD_ARG(run, NULL, "<name>: start an app (reboots if one runs)", cmd_run, 2, 0),
	SHELL_CMD(info, NULL, "The loaded app", cmd_info),
	SHELL_CMD(crash, NULL, "What ended the previous boot", cmd_crash),
	SHELL_SUBCMD_SET_END);
SHELL_CMD_REGISTER(multiapp, &app_cmds, "Apps run as extensions", NULL);
