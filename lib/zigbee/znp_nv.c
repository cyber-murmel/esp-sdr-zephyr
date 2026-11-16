/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * ZNP item store: see include/zigbee/znp_nv.h.
 */
#include <errno.h>
#include <string.h>

#include <zigbee/znp_nv.h>

static bool key_eq(const struct znp_nv_key *a, const struct znp_nv_key *b)
{
	return a->sys == b->sys && a->item == b->item && a->sub == b->sub;
}

static struct znp_nv_item *find(struct znp_nv *nv, const struct znp_nv_key *key)
{
	for (int i = 0; i < CONFIG_ZNP_NV_ITEMS; i++) {
		if (nv->items[i].used && key_eq(&nv->items[i].key, key)) {
			return &nv->items[i];
		}
	}
	return NULL;
}

/* A free slot, @p want first when it is free. */
static struct znp_nv_item *alloc_slot(struct znp_nv *nv, int want)
{
	if (want >= 0 && want < CONFIG_ZNP_NV_ITEMS && !nv->items[want].used) {
		return &nv->items[want];
	}
	for (int i = 0; i < CONFIG_ZNP_NV_ITEMS; i++) {
		if (!nv->items[i].used) {
			return &nv->items[i];
		}
	}
	return NULL;
}

static int create(struct znp_nv *nv, int want_slot, struct znp_nv_key key, uint16_t len,
		  const void *init, uint16_t init_len)
{
	if (len == 0U || init_len > len || find(nv, &key) != NULL) {
		return find(nv, &key) != NULL ? -EEXIST : -EINVAL;
	}
	if (nv->pool_used + len > CONFIG_ZNP_NV_POOL_SIZE) {
		return -ENOMEM;
	}

	struct znp_nv_item *it = alloc_slot(nv, want_slot);

	if (it == NULL) {
		return -ENOMEM;
	}
	it->key = key;
	it->len = len;
	it->off = nv->pool_used;
	it->used = true;
	memset(&nv->pool[it->off], 0, len);
	if (init_len > 0U) {
		memcpy(&nv->pool[it->off], init, init_len);
	}
	nv->pool_used += len;
	return 0;
}

void znp_nv_init(struct znp_nv *nv, const struct znp_nv_backend *be)
{
	memset(nv->items, 0, sizeof(nv->items));
	nv->pool_used = 0;
	nv->be = be != NULL ? *be : (struct znp_nv_backend){0};
	k_mutex_init(&nv->lock);
}

uint16_t znp_nv_len(struct znp_nv *nv, struct znp_nv_key key)
{
	struct znp_nv_item *it;
	uint16_t len;

	k_mutex_lock(&nv->lock, K_FOREVER);
	it = find(nv, &key);
	len = it != NULL ? it->len : 0U;
	k_mutex_unlock(&nv->lock);
	return len;
}

int znp_nv_create(struct znp_nv *nv, struct znp_nv_key key, uint16_t len, const void *init,
		  uint16_t init_len)
{
	int ret;

	k_mutex_lock(&nv->lock, K_FOREVER);
	ret = create(nv, -1, key, len, init, init_len);
	if (ret == 0) {
		find(nv, &key)->dirty = true;
	}
	k_mutex_unlock(&nv->lock);
	return ret;
}

int znp_nv_read(struct znp_nv *nv, struct znp_nv_key key, uint16_t off, void *buf, uint16_t n)
{
	struct znp_nv_item *it;
	int ret;

	k_mutex_lock(&nv->lock, K_FOREVER);
	it = find(nv, &key);
	if (it == NULL) {
		ret = -ENOENT;
	} else if (off > it->len) {
		ret = -EINVAL;
	} else {
		ret = MIN(n, it->len - off);
		memcpy(buf, &nv->pool[it->off + off], ret);
	}
	k_mutex_unlock(&nv->lock);
	return ret;
}

int znp_nv_write(struct znp_nv *nv, struct znp_nv_key key, uint16_t off, const void *buf,
		 uint16_t n)
{
	struct znp_nv_item *it;
	int ret = 0;

	k_mutex_lock(&nv->lock, K_FOREVER);
	it = find(nv, &key);
	if (it == NULL) {
		ret = -ENOENT;
	} else if ((uint32_t)off + n > it->len) {
		ret = -EINVAL;
	} else {
		memcpy(&nv->pool[it->off + off], buf, n);
		it->dirty = true;
	}
	k_mutex_unlock(&nv->lock);
	return ret;
}

static int del(struct znp_nv *nv, struct znp_nv_item *it)
{
	uint16_t off = it->off, len = it->len;

	memmove(&nv->pool[off], &nv->pool[off + len], nv->pool_used - off - len);
	nv->pool_used -= len;
	for (int i = 0; i < CONFIG_ZNP_NV_ITEMS; i++) {
		if (nv->items[i].used && nv->items[i].off > off) {
			nv->items[i].off -= len;
		}
	}
	/* Kept dirty and unused: the flush erases the slot in the backend. */
	it->used = false;
	it->dirty = true;
	it->len = 0;
	return 0;
}

int znp_nv_delete(struct znp_nv *nv, struct znp_nv_key key)
{
	struct znp_nv_item *it;
	int ret = 0;

	k_mutex_lock(&nv->lock, K_FOREVER);
	it = find(nv, &key);
	ret = it == NULL ? -ENOENT : del(nv, it);
	k_mutex_unlock(&nv->lock);
	return ret;
}

int znp_nv_set(struct znp_nv *nv, struct znp_nv_key key, const void *data, uint16_t len)
{
	struct znp_nv_item *it;
	int ret = 0;

	k_mutex_lock(&nv->lock, K_FOREVER);
	it = find(nv, &key);
	if (it != NULL && it->len == len) {
		memcpy(&nv->pool[it->off], data, len);
		it->dirty = true;
	} else {
		if (it != NULL) {
			del(nv, it);
		}
		ret = create(nv, -1, key, len, data, len);
		if (ret == 0) {
			find(nv, &key)->dirty = true;
		}
	}
	k_mutex_unlock(&nv->lock);
	return ret;
}

int znp_nv_restore(struct znp_nv *nv, uint16_t slot, struct znp_nv_key key, const void *data,
		   uint16_t len)
{
	int ret;

	k_mutex_lock(&nv->lock, K_FOREVER);
	ret = create(nv, slot, key, len, data, len);
	k_mutex_unlock(&nv->lock);
	return ret;
}

int znp_nv_flush(struct znp_nv *nv)
{
	int first = 0;

	k_mutex_lock(&nv->lock, K_FOREVER);
	for (int i = 0; i < CONFIG_ZNP_NV_ITEMS; i++) {
		struct znp_nv_item *it = &nv->items[i];

		if (!it->dirty) {
			continue;
		}
		if (nv->be.store != NULL) {
			int r = nv->be.store(nv->be.ctx, (uint16_t)i, &it->key,
					     it->used ? &nv->pool[it->off] : NULL, it->len);

			if (r != 0) {
				first = first != 0 ? first : r;
				continue;
			}
		}
		it->dirty = false;
	}
	k_mutex_unlock(&nv->lock);
	return first;
}

bool znp_nv_dirty(struct znp_nv *nv)
{
	bool d = false;

	k_mutex_lock(&nv->lock, K_FOREVER);
	for (int i = 0; i < CONFIG_ZNP_NV_ITEMS && !d; i++) {
		d = nv->items[i].dirty;
	}
	k_mutex_unlock(&nv->lock);
	return d;
}
