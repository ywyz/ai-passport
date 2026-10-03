/**
 * passport_events.h - travel event journal (D1b).
 *
 * Guarantees (contract sections 2/7/10, DR04):
 * - identity: stable device_id + boot-independent device_epoch + monotonic
 *   sequence; re-initialization keeps epoch, identity loss starts a new one.
 * - every accepted save is in the DURABLE journal before success is
 *   reported; a save failure never produces success feedback (A04).
 * - pending capacity: PASSPORT_EVENT_CAPACITY entries; additions beyond the
 *   journal full capacity fail with EVT_FULL without losing history.
 * - ack clears only after durable receipt recording (ack receipt first,
 *   deletion second); duplicate acknowledgments are no-ops.
 */
#ifndef PASSPORT_EVENTS_H
#define PASSPORT_EVENTS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "passport_kv.h"

#define PASSPORT_EVENT_CAPACITY 256
#define PASSPORT_EVENT_ID_MAX 64
#define PASSPORT_EVENT_TARGET_MAX 64
#define PASSPORT_EVENT_CONTEXT_MAX 64
#define PASSPORT_EVENT_EPOCH_MAX 64

enum passport_ev_op {
    PASSPORT_EV_VISIT_SET = 0,
    PASSPORT_EV_WISH_SET,
    PASSPORT_EV_GUIDE_PROGRESS_SET,
    PASSPORT_EV_OP_COUNT
};

/* serialized journal entry (le, field order fixed by version) */
struct passport_event {
    char event_id[PASSPORT_EVENT_ID_MAX + 1];
    char context_id[PASSPORT_EVENT_CONTEXT_MAX + 1];
    char target_id[PASSPORT_EVENT_TARGET_MAX + 1];
    uint64_t sequence;
    char predecessor_event_id[PASSPORT_EVENT_ID_MAX + 1];
    uint8_t op;         /* enum passport_ev_op */
    uint8_t value;      /* boolean explicit-set value */
    uint32_t base_revision;
    uint8_t in_flight;  /* submitted but not yet acknowledged */
};

#define PASSPORT_EPOCH_CHANGED 1
#define PASSPORT_EV_OK 0
#define PASSPORT_EV_FULL (-1)
#define PASSPORT_EV_IO (-2)

int passport_events_open(const struct passport_kv *kv);

/* enqueue one explicit-set event; persistence happens before returning OK */
int passport_events_enqueue(const struct passport_event *event);

/* number of pending entries (queued + in flight) */
size_t passport_events_pending(void);

/* begin a submission cycle: copies out up to `max` in-flight entries */
size_t passport_events_take(struct passport_event *out, size_t max);

/* durable acknowledgment of delivered ids; unknown ids are no-ops */
int passport_events_ack(const char *const *event_ids, size_t count, size_t *acked_len);

/* mark a submission cycle failed: entries return to queued */
void passport_events_return_inflight(void);

int passport_events_clear_identity(void);

const char *passport_events_device_id(void);
const char *passport_events_device_epoch(void);
uint64_t passport_events_next_sequence(void);

/* read/write a derived acknowledged record base state (record_id key) */
int passport_events_record_state(const char *record_id, bool *value,
                                 uint32_t *revision);
/* persist acknowledged base state durably before success feedback */
int passport_events_record_save(const char *record_id, bool value,
                                uint32_t revision);

#endif /* PASSPORT_EVENTS_H */
