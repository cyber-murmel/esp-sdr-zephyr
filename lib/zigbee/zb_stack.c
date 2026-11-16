/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Stack assembly: see include/zigbee/zb_stack.h.
 */
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <zigbee/zb_stack.h>

#define TRACE_HEX_MAX 96

void zb_stack_trace(struct zb_stack *s, uint8_t level, const char *fmt, ...)
{
	char line[MT_MAX_PAYLOAD];
	va_list ap;
	int n;

	if (s->trace_level < level) {
		return;
	}
	va_start(ap, fmt);
	n = vsnprintf(line + 1, sizeof(line) - 1, fmt, ap);
	va_end(ap);
	if (n < 0) {
		return;
	}
	n = MIN(n, (int)sizeof(line) - 2);
	line[0] = (char)level;
	znp_send_areq(s->znp, MT_DEBUG, 0x80, line, (uint8_t)(n + 1));
}

/* Every frame the MAC sends or receives, as hex. */
static void mac_trace(void *ctx, bool tx, const uint8_t *f, size_t n, uint8_t lqi, int8_t rssi)
{
	struct zb_stack *s = CONTAINER_OF(ctx, struct zb_stack, net);
	char hex[2 * TRACE_HEX_MAX + 1];
	size_t m = MIN(n, (size_t)TRACE_HEX_MAX);

	if (s->trace_level < 2U) {
		return;
	}
	for (size_t i = 0; i < m; i++) {
		snprintf(&hex[2 * i], 3, "%02x", f[i]);
	}
	zb_stack_trace(s, 2, "%s %u lqi=%u rssi=%d %s%s", tx ? "TX" : "RX", (unsigned int)n, lqi,
		       rssi, hex, m < n ? "..." : "");
}

int zb_stack_init(struct zb_stack *s, struct znp *z, struct znp_nv *nv,
		  const struct zb_radio_ops *radio, void *rctx, const uint8_t default_eui[8],
		  k_thread_stack_t *mac_stack, size_t mac_stack_size, int mac_prio)
{
	struct zb_mac_up up;

	memset(s, 0, sizeof(*s));
	s->znp = z;
	s->nv = nv;
	memcpy(s->default_eui, default_eui, 8);
	z->stack = s;

	zb_net_mac_up(&s->net, &up);
	up.trace = mac_trace;
	zb_mac_init(&s->mac, radio, rctx, &up, default_eui);
	zb_mac_run(&s->mac, mac_stack, mac_stack_size, mac_prio);
	zb_net_init(&s->net, &s->mac, nv, &zb_stack_net_cb);
	s->net.cb.ctx = s;

	znp_register(z, MT_APP_CNF, znp_appcnf_handler);
	znp_register(z, MT_ZDO, znp_zdo_handler);
	znp_register(z, MT_AF, znp_af_handler);
	znp_register(z, MT_DEBUG, znp_debug_handler);

	zb_stack_reset(s, 0xFF);
	return 0;
}

/* The range the radio and Z-Stack hosts deal in. */
#define TXPOWER_MIN (-20)
#define TXPOWER_MAX 20

int8_t zb_stack_set_txpower(struct zb_stack *s, int8_t dbm)
{
	dbm = (int8_t)CLAMP(dbm, TXPOWER_MIN, TXPOWER_MAX);
	(void)zb_mac_set_txpower(&s->mac, dbm);
	return dbm;
}

void zb_stack_reset(struct zb_stack *s, uint8_t type)
{
	/* Flush what the host wrote, then boot from NV as a restart would. */
	(void)znp_nv_flush(s->nv);
	(void)zb_net_boot(&s->net, s->default_eui);
	memcpy(s->znp->cfg.ext_addr, zb_net_eui(&s->net), 8);
	memset(s->eps, 0, sizeof(s->eps));
	memset(s->groups, 0, sizeof(s->groups));
	if (type != 0xFFU) {
		znp_send_reset_ind(s->znp, ZNP_RESET_EXTERNAL);
	}
}
