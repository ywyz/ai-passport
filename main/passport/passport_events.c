#include "passport_events.h"

#include <inttypes.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NS_KEY "_ns"
#define NS_VALUE 1

struct passport_events_ctx {
    const struct passport_kv *kv;
    char device_id[PASSPORT_EVENT_ID_MAX + 1];
    char epoch[PASSPORT_EVENT_EPOCH_MAX + 1];
    uint64_t sequence;
    struct passport_event pending[PASSPORT_EVENT_CAPACITY];
    size_t pending_len;
    bool loaded;
};
static struct passport_events_ctx ctx;

static int kv_get_str(const struct passport_kv *kv, const char *key,
                      char *out, size_t cap)
{
    char buf[PASSPORT_KV_MAX_VALUE];
    size_t out_len = 0;
    if (kv->get(kv->ctx, key, buf, sizeof(buf), &out_len) != 0)
        return -1;
    if (out_len >= cap)
        out_len = cap - 1;
    memcpy(out, buf, out_len);
    out[out_len] = '\0';
    return 0;
}

static void id_from_random(char *out, size_t cap, const char *prefix)
{
    uint32_t rnd[4];
    for (size_t i = 0; i < 4; ++i) {
        rnd[i] = (uint32_t)rand();
    }
    size_t pos = 0;
    pos += (size_t)snprintf(out, cap, "%s-", prefix);
    for (size_t i = 0; i < 4 && pos < cap; ++i) {
        pos += (size_t)snprintf(out + pos, cap - pos, "%08" PRIx32, rnd[i]);
    }
}

int passport_events_open(const struct passport_kv *kv)
{
    memset(&ctx, 0, sizeof(ctx));
    ctx.kv = kv;
    if (kv_get_str(kv, "devid", ctx.device_id, sizeof(ctx.device_id)) != 0) {
        id_from_random(ctx.device_id, sizeof(ctx.device_id), "dv");
        if (kv->set(kv->ctx, "devid", ctx.device_id,
                    strlen(ctx.device_id)) != 0)
            return PASSPORT_EV_IO;
        ctx.epoch[0] = '\0';
    }
    if (kv_get_str(kv, "epoch", ctx.epoch, sizeof(ctx.epoch)) != 0) {
        id_from_random(ctx.epoch, sizeof(ctx.epoch), "ep");
        if (kv->set(kv->ctx, "epoch", ctx.epoch, strlen(ctx.epoch)) != 0)
            return PASSPORT_EV_IO;
    }
    char buf[32];
    size_t out_len = 0;
    if (kv->get(kv->ctx, "seq", buf, sizeof(buf), &out_len) == 0) {
        buf[out_len < sizeof(buf) - 1 ? out_len : sizeof(buf) - 1] = '\0';
        ctx.sequence = strtoull(buf, NULL, 10);
    }
    /* journal load: entry keys are "e<"slot index>"; order is by slot */
    for (size_t slot = 0; slot < PASSPORT_EVENT_CAPACITY; ++slot) {
        char key[PASSPORT_KV_MAX_KEY];
        (void)snprintf(key, sizeof(key), "e%03zu", slot);
        char blob[PASSPORT_KV_MAX_VALUE];
        size_t len = 0;
        if (kv->get(kv->ctx, key, blob, sizeof(blob), &len) != 0)
            continue;
        if (ctx.pending_len >= PASSPORT_EVENT_CAPACITY)
            break;
        /* little-endian fixed layout consistent with passport_events_flush */
        struct passport_event event;
        if (len != sizeof(event))
            continue;
        memcpy(&event, blob, sizeof(event));
        event.event_id[PASSPORT_EVENT_ID_MAX] = '\0';
        event.target_id[PASSPORT_EVENT_TARGET_MAX] = '\0';
        event.context_id[PASSPORT_EVENT_CONTEXT_MAX] = '\0';
        event.predecessor_event_id[PASSPORT_EVENT_ID_MAX] = '\0';
        ctx.pending[ctx.pending_len++] = event;
    }
    ctx.loaded = true;
    return 0;
}

static int journal_write(size_t slot, const struct passport_event *event)
{
    char key[PASSPORT_KV_MAX_KEY];
    (void)snprintf(key, sizeof(key), "e%03zu", slot);
    return ctx.kv->set(ctx.kv->ctx, key, event, sizeof(*event));
}

static int journal_erase(size_t slot)
{
    char key[PASSPORT_KV_MAX_KEY];
    (void)snprintf(key, sizeof(key), "e%03zu", slot);
    return ctx.kv->del(ctx.kv->ctx, key);
}

int passport_events_enqueue(const struct passport_event *event)
{
    if (!ctx.loaded)
        return PASSPORT_EV_IO;
    if (ctx.pending_len >= PASSPORT_EVENT_CAPACITY)
        return PASSPORT_EV_FULL;
    /* dedup: identical pending op/target/value/revison reuses the queue */
    for (size_t i = 0; i < ctx.pending_len; ++i) {
        struct passport_event *p = &ctx.pending[i];
        if (p->op == event->op && p->value == event->value &&
                strcmp(p->target_id, event->target_id) == 0 &&
                strcmp(p->context_id, event->context_id) == 0 &&
                strcmp(p->predecessor_event_id,
                       event->predecessor_event_id) == 0 &&
                p->base_revision == event->base_revision &&
                !p->in_flight) {
            return PASSPORT_EV_OK; /* identical retry, no duplicate entry */
        }
    }
    struct passport_event out = *event;
    ctx.sequence += 1;
    out.sequence = ctx.sequence;
    {
        int wrote = snprintf(out.event_id, sizeof(out.event_id), "%s-%llu",
                             ctx.device_id, (unsigned long long)ctx.sequence);
        if (wrote < 0 || (size_t)wrote >= sizeof(out.event_id))
            out.event_id[sizeof(out.event_id) - 1] = '\0';
    }
    if (journal_write(ctx.pending_len, &out) != 0) {
        ctx.sequence -= 1; /* sequence allocation succeeds only with journal */
        return PASSPORT_EV_IO;
    }
    char seqbuf[24];
    (void)snprintf(seqbuf, sizeof(seqbuf), "%llu",
                   (unsigned long long)ctx.sequence);
    if (ctx.kv->set(ctx.kv->ctx, "seq", seqbuf, strlen(seqbuf)) != 0)
        return PASSPORT_EV_IO;
    ctx.pending[ctx.pending_len++] = out;
    return PASSPORT_EV_OK;
}

size_t passport_events_pending(void)
{
    return ctx.pending_len;
}

size_t passport_events_take(struct passport_event *out, size_t max)
{
    size_t taken = 0;
    for (size_t i = 0; i < ctx.pending_len && taken < max; ++i) {
        if (!ctx.pending[i].in_flight) {
            out[taken++] = ctx.pending[i];
        }
    }
    if (taken > 0) {
        /* mark in flight durably before submission */
        for (size_t i = 0; i < ctx.pending_len; ++i) {
            for (size_t j = 0; j < taken; ++j) {
                if (strcmp(ctx.pending[i].event_id, out[j].event_id) == 0) {
                    ctx.pending[i].in_flight = 1;
                }
            }
        }
        for (size_t i = 0; i < ctx.pending_len; ++i) {
            (void)journal_write(i, &ctx.pending[i]);
        }
    }
    return taken;
}

int passport_events_ack(const char *const *event_ids, size_t count,
                        size_t *acked_len)
{
    if (!ctx.loaded)
        return PASSPORT_EV_IO;
    bool removed_slots[PASSPORT_EVENT_CAPACITY];
    memset(removed_slots, 0, sizeof(removed_slots));
    size_t removed = 0;
    for (size_t slot = 0; slot < ctx.pending_len; ++slot) {
        for (size_t i = 0; i < count; ++i) {
            if (removed_slots[slot])
                continue;
            if (strcmp(ctx.pending[slot].event_id, event_ids[i]) != 0)
                continue;
            removed_slots[slot] = true;
            ++removed;
            break;
        }
    }
    /* canonical journal rewrite: slots < pending_len hold the survivors,
     * slots beyond must be erased so reload cannot resurrect acked ids */
    if (removed > 0) {
        size_t write = 0;
        for (size_t slot = 0; slot < ctx.pending_len; ++slot) {
            if (removed_slots[slot])
                continue;
            ctx.pending[write++] = ctx.pending[slot];
        }
        ctx.pending_len = write;
    }
    for (size_t slot = 0; slot < ctx.pending_len; ++slot) {
        if (journal_write(slot, &ctx.pending[slot]) != 0)
            return PASSPORT_EV_IO;
    }
    for (size_t slot = ctx.pending_len; slot < PASSPORT_EVENT_CAPACITY; ++slot) {
        if (journal_erase(slot) != 0 && removed == 0)
            return PASSPORT_EV_IO; /* erased slots may already be absent */
    }
    if (acked_len)
        *acked_len = removed;
    return PASSPORT_EV_OK;
}

void passport_events_return_inflight(void)
{
    for (size_t i = 0; i < ctx.pending_len; ++i) {
        if (ctx.pending[i].in_flight) {
            ctx.pending[i].in_flight = 0;
            (void)journal_write(i, &ctx.pending[i]);
        }
    }
}

int passport_events_clear_identity(void)
{
    memset(ctx.device_id, 0, sizeof(ctx.device_id));
    ctx.epoch[0] = '\0';
    ctx.pending_len = 0;
    ctx.loaded = false;
    return PASSPORT_EV_OK;
}

const char *passport_events_device_id(void)
{
    return ctx.device_id;
}

const char *passport_events_device_epoch(void)
{
    return ctx.epoch;
}

uint64_t passport_events_next_sequence(void)
{
    return ctx.sequence + 1;
}

int passport_events_record_state(const char *record_id, bool *value,
                                 uint32_t *revision)
{
    char key[PASSPORT_KV_MAX_KEY];
    if (strlen(record_id) >= PASSPORT_KV_MAX_KEY - 4)
        return PASSPORT_EV_IO;
    (void)snprintf(key, sizeof(key), "r_%s", record_id);
    char blob[1 + sizeof(uint32_t)];
    size_t len = 0;
    if (ctx.kv->get(ctx.kv->ctx, key, blob, sizeof(blob), &len) != 0)
        return 1;
    if (len < 1)
        return 1;
    if (value)
        *value = blob[0] != 0;
    if (revision && len >= 5) {
        memcpy(revision, blob + 1, sizeof(*revision));
    }
    return 0;
}

/* Persist the acknowledged/base state before success feedback
 * (A04/DR04: receipt first, cleanup second). */
int passport_events_record_save(const char *record_id, bool value,
                                uint32_t revision)
{
    char key[PASSPORT_KV_MAX_KEY];
    if (strlen(record_id) >= PASSPORT_KV_MAX_KEY - 4)
        return PASSPORT_EV_IO;
    (void)snprintf(key, sizeof(key), "r_%s", record_id);
    char blob[1 + sizeof(uint32_t)];
    blob[0] = value ? 1 : 0;
    memcpy(blob + 1, &revision, sizeof(revision));
    return ctx.kv->set(ctx.kv->ctx, key, blob, sizeof(blob)) == 0 ? 0 : PASSPORT_EV_IO;
}
