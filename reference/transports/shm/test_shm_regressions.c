/* Regression tests for SHM metadata discrimination and reply bounds checks. */
#include "fss/transport_abi.h"

#include <dlfcn.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define SLOT_DATA_CAP 4096u

typedef fss_transport_t *(*create_fn_t)(const char *);
typedef void (*destroy_fn_t)(fss_transport_t *);

static int check_transport_abi(void *dl)
{
    fss_transport_abi_version_fn_t version =
        (fss_transport_abi_version_fn_t)dlsym(dl, "fss_transport_abi_version");
    fss_transport_capabilities_fn_t capabilities =
        (fss_transport_capabilities_fn_t)dlsym(dl, "fss_transport_capabilities");
    return version && capabilities &&
           fss_transport_abi_compatible(version(), capabilities());
}

/* Darwin limits POSIX semaphore names to 31 bytes.  The transport adds a
 * 9-byte prefix and up to a 3-byte suffix, so keep test channel names short.
 * PID plus this process-local counter keeps independent test runs and cases
 * distinct without relaxing the transport's production name validation. */
static uint32_t next_channel_id;
static void make_channel_name(char *name, size_t size)
{
    snprintf(name, size, "r%08x%08x", (uint32_t)getpid(), next_channel_id++);
}

/* Minimal valid meta arrays.  They have distinct flat metadata but deliberately
 * share the caller-supplied carrier id. */
static const uint8_t meta_a[] = {
    0x81, 0xa2, 0x65, 'q','n','a','m','e', 0x65, 'a','l','p','h','a',
    0x6c, 's','e','r','i','a','l','i','z','a','b','l','e', 0xf5
};
static const uint8_t meta_b[] = {
    0x81, 0xa2, 0x65, 'q','n','a','m','e', 0x65, 'b','r','a','v','o',
    0x6c, 's','e','r','i','a','l','i','z','a','b','l','e', 0xf5
};

typedef struct { unsigned count; int failed; } meta_record;
static int meta_sink(void *self, fss_carrier_t *c)
{
    meta_record *r = self;
    const uint8_t *want = (r->count & 1u) ? meta_b : meta_a;
    size_t want_size = (r->count & 1u) ? sizeof(meta_b) : sizeof(meta_a);
    if (!c || c->meta_size != want_size ||
        memcmp(c->meta_ptr, want, want_size) != 0)
        r->failed = 1;
    r->count++;
    return FSS_SEND_OK;
}

typedef struct { int mode; } reply_record;
static int reply_sink(void *self, fss_carrier_t *c)
{
    reply_record *r = self;
    if (r->mode == 0) {                 /* exact capacity is safe */
        memset(c->data_ptr, 0x5a, SLOT_DATA_CAP);
        c->data_size = SLOT_DATA_CAP;
    } else if (r->mode == 1) {          /* must not overflow reply slot */
        c->data_size = SLOT_DATA_CAP + 1u;
    } else if (r->mode == 2) {          /* origin has no output storage */
        ((uint8_t *)c->data_ptr)[0] = 1;
        c->data_size = 1;
    }
    return FSS_SEND_OK;
}

typedef struct { unsigned count; } count_record;
static int count_sink(void *self, fss_carrier_t *c)
{
    count_record *r = self;
    if (c) r->count++;
    return FSS_SEND_OK;
}

/* Concurrent local callers must still present as the one producer required by
 * the shared request ring.  The reply marker also proves that each RR caller
 * consumes the reply for its own request. */
#define CONCURRENT_SENDERS 8u
#define CONCURRENT_SENDS   64u
#define CONCURRENT_TOTAL   (CONCURRENT_SENDERS * CONCURRENT_SENDS)

typedef struct {
    atomic_uint received;
    atomic_uint seen[CONCURRENT_TOTAL];
    atomic_int bad;
    int round_trip;
} concurrent_record;

static int concurrent_sink(void *self, fss_carrier_t *c)
{
    concurrent_record *r = self;
    uint32_t id;
    if (!c || c->data_size != sizeof(id) || !c->data_ptr) {
        atomic_store(&r->bad, 1);
        return FSS_SEND_UNBOUND;
    }
    memcpy(&id, c->data_ptr, sizeof(id));
    if (id >= CONCURRENT_TOTAL || c->id != UINT64_C(0x1000) + id)
        atomic_store(&r->bad, 1);
    else if (atomic_fetch_add(&r->seen[id], 1) != 0)
        atomic_store(&r->bad, 1);
    atomic_fetch_add(&r->received, 1);
    if (r->round_trip) {
        uint32_t reply = id ^ UINT32_C(0xa5a55a5a);
        memcpy(c->data_ptr, &reply, sizeof(reply));
    }
    return FSS_SEND_OK;
}

typedef struct {
    fss_endpoint_t *sender;
    unsigned base;
    int round_trip;
    atomic_int *failed;
} concurrent_send_arg;

static void *concurrent_send(void *arg)
{
    concurrent_send_arg *a = arg;
    for (unsigned i = 0; i < CONCURRENT_SENDS; ++i) {
        uint32_t id = a->base + i;
        uint32_t data = id;
        fss_carrier_t c = { .id = UINT64_C(0x1000) + id,
                            .data_ptr = &data, .data_size = sizeof(data) };
        int rc = a->sender->send(a->sender->send_ctx, &c);
        if (rc != FSS_SEND_OK ||
            (a->round_trip && data != (id ^ UINT32_C(0xa5a55a5a)))) {
            atomic_store(a->failed, 1);
            break;
        }
    }
    return NULL;
}

static int setup_pair(create_fn_t create, const char *name, int rr,
                      fss_transport_t **tx, fss_transport_t **rx,
                      fss_endpoint_t *sender, fss_endpoint_t *sink)
{
    fss_channel_t *a, *b;
    *tx = create(NULL); *rx = create(NULL);
    if (!*tx || !*rx) return 0;
    a = (*tx)->channel((*tx)->self, name, rr, 1, NULL, 0);
    b = (*rx)->channel((*rx)->self, name, rr, 0, NULL, 0);
    return a && b && (*tx)->attach((*tx)->self, a, sender, FSS_ROLE_INITIATOR) == FSS_SEND_OK &&
           (*rx)->attach((*rx)->self, b, sink, FSS_ROLE_TARGET) == FSS_SEND_OK;
}

static int concurrent_sender_regression(create_fn_t create, destroy_fn_t destroy,
                                        int round_trip)
{
    char name[64];
    fss_transport_t *tx = NULL, *rx = NULL;
    fss_endpoint_t sender = {0}, sink = {0};
    concurrent_record record = {0};
    pthread_t workers[CONCURRENT_SENDERS];
    concurrent_send_arg args[CONCURRENT_SENDERS];
    atomic_int failed;
    int ok;

    atomic_init(&record.received, 0);
    atomic_init(&record.bad, 0);
    for (unsigned i = 0; i < CONCURRENT_TOTAL; ++i)
        atomic_init(&record.seen[i], 0);
    atomic_init(&failed, 0);
    record.round_trip = round_trip;
    sink.self = &record;
    sink.receive = concurrent_sink;
    make_channel_name(name, sizeof(name));
    ok = setup_pair(create, name, round_trip, &tx, &rx, &sender, &sink);
    for (unsigned i = 0; ok && i < CONCURRENT_SENDERS; ++i) {
        args[i] = (concurrent_send_arg){ .sender = &sender,
            .base = i * CONCURRENT_SENDS, .round_trip = round_trip,
            .failed = &failed };
        if (pthread_create(&workers[i], NULL, concurrent_send, &args[i]) != 0) {
            ok = 0;
            for (unsigned j = 0; j < i; ++j) pthread_join(workers[j], NULL);
            break;
        }
    }
    if (ok)
        for (unsigned i = 0; i < CONCURRENT_SENDERS; ++i) pthread_join(workers[i], NULL);
    for (unsigned i = 0; ok && !round_trip && i < 500 &&
         atomic_load(&record.received) != CONCURRENT_TOTAL; ++i) {
        struct timespec delay = { 0, 1000000L };
        nanosleep(&delay, NULL);
    }
    ok = ok && !atomic_load(&failed) && !atomic_load(&record.bad) &&
         atomic_load(&record.received) == CONCURRENT_TOTAL;
    for (unsigned i = 0; ok && i < CONCURRENT_TOTAL; ++i)
        ok = atomic_load(&record.seen[i]) == 1;
    if (tx) destroy(tx);
    if (rx) destroy(rx);
    return ok;
}

/* Queue a frame before its receiver starts, then replace its no-meta marker
 * with an unpublished table index.  The SHM format keeps the first request
 * slot immediately after its fixed 64-byte header; meta_index is byte 12 of
 * that slot.  This simulates a corrupted peer without exposing transport
 * internals through its public ABI. */
static int queue_bad_meta_index(const char *channel, int wait_for_head)
{
    char name[96];
    snprintf(name, sizeof(name), "/fss_shm_%s", channel);
    int fd = shm_open(name, O_RDWR, 0600);
    if (fd < 0) return 0;
    uint8_t *map = mmap(NULL, 80, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (map == MAP_FAILED) return 0;
    int ready = !wait_for_head;
    for (unsigned i = 0; !ready && i < 200; ++i) {
        uint64_t head;
        memcpy(&head, map + 16, sizeof(head));
        ready = head != 0;
        if (!ready) {
            struct timespec delay = { 0, 1000000L };
            nanosleep(&delay, NULL);
        }
    }
    uint32_t bad_index = 0;
    if (ready) {
        memcpy(map + 64 + 12, &bad_index, sizeof(bad_index));
        msync(map, 80, MS_SYNC);
    }
    munmap(map, 80);
    return ready;
}

#if !defined(__APPLE__)
/* A provisioner can allocate a larger backing object than the transport's
 * fixed layout.  The attacher must accept it, while the transport maps only
 * its own layout. */
static int extend_backing_file(const char *channel)
{
    char name[96];
    struct stat st;
    snprintf(name, sizeof(name), "/fss_shm_%s", channel);
    int fd = shm_open(name, O_RDWR, 0600);
    if (fd < 0) return 0;
    int ok = fstat(fd, &st) == 0 && st.st_size >= 0 &&
             ftruncate(fd, st.st_size + 4096) == 0;
    close(fd);
    return ok;
}

static int truncate_backing_file(const char *channel)
{
    char name[96];
    struct stat st;
    snprintf(name, sizeof(name), "/fss_shm_%s", channel);
    int fd = shm_open(name, O_RDWR, 0600);
    if (fd < 0) return 0;
    int ok = fstat(fd, &st) == 0 && st.st_size > 0 &&
             ftruncate(fd, st.st_size - 1) == 0;
    close(fd);
    return ok;
}
#endif

/* Change the queued request id before the receiver starts.  The receiver
 * echoes this id on its reply ring, as a malformed or mismatched peer could. */
static int queue_mismatched_carrier_id(const char *channel, uint64_t id)
{
    char name[96];
    snprintf(name, sizeof(name), "/fss_shm_%s", channel);
    int fd = shm_open(name, O_RDWR, 0600);
    if (fd < 0) return 0;
    uint8_t *map = mmap(NULL, 80, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (map == MAP_FAILED) return 0;
    int ready = 0;
    for (unsigned i = 0; !ready && i < 200; ++i) {
        uint64_t head;
        memcpy(&head, map + 16, sizeof(head));
        ready = head != 0;
        if (!ready) {
            struct timespec delay = { 0, 1000000L };
            nanosleep(&delay, NULL);
        }
    }
    if (ready) {
        memcpy(map + 64, &id, sizeof(id));
        msync(map, 80, MS_SYNC);
    }
    munmap(map, 80);
    return ready;
}

typedef struct {
    fss_endpoint_t *sender;
    fss_carrier_t *carrier;
    int result;
    atomic_int *done;
} send_thread_arg;

static void *send_thread(void *arg)
{
    send_thread_arg *a = arg;
    a->result = a->sender->send(a->sender->send_ctx, a->carrier);
    if (a->done)
        atomic_store_explicit(a->done, 1, memory_order_release);
    return NULL;
}

typedef struct {
    destroy_fn_t destroy;
    fss_transport_t *transport;
    atomic_int done;
} destroy_thread_arg;

static void *destroy_thread(void *arg)
{
    destroy_thread_arg *a = arg;
    a->destroy(a->transport);
    atomic_store_explicit(&a->done, 1, memory_order_release);
    return NULL;
}

/* Do not use platform-specific timed joins.  This gives a finite diagnostic
 * wait, then joins only a thread that has already reported completion. */
static int join_bounded(pthread_t thread, atomic_int *done)
{
    for (unsigned i = 0; i < 500; ++i) {
        if (atomic_load_explicit(done, memory_order_acquire)) {
            pthread_join(thread, NULL);
            return 1;
        }
        struct timespec delay = { 0, 1000000L };
        nanosleep(&delay, NULL);
    }
    return 0;
}

static int mismatch_reply_sink(void *self, fss_carrier_t *c)
{
    (void)self;
    ((uint8_t *)c->data_ptr)[0] = 0x5a;
    return FSS_SEND_OK;
}

int main(int argc, char **argv)
{
    if (argc != 2) return 2;
    void *dl = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!dl || !check_transport_abi(dl)) return 1;
    create_fn_t create = (create_fn_t)dlsym(dl, "fss_transport_create");
    destroy_fn_t destroy = (destroy_fn_t)dlsym(dl, "fss_transport_destroy");
    if (!create || !destroy) return 1;

    int ok = 1;
    char name[64];
    fss_transport_t *tx = NULL, *rx = NULL;
    fss_endpoint_t sender = {0}, sink = {0};
    send_thread_arg send_arg;
    meta_record mr = {0}; sink.self = &mr; sink.receive = meta_sink;
    make_channel_name(name, sizeof(name));
    ok = setup_pair(create, name, 0, &tx, &rx, &sender, &sink);
    for (unsigned i = 0; ok && i < 4; ++i) {
        uint8_t byte = (uint8_t)i;
        fss_carrier_t c = { .id = UINT64_C(0x99),
                            .meta_ptr = (void *)((i & 1u) ? meta_b : meta_a),
                            .meta_size = (i & 1u) ? sizeof(meta_b) : sizeof(meta_a),
                            .data_ptr = &byte, .data_size = 1 };
        ok = sender.send(sender.send_ctx, &c) == FSS_SEND_OK;
    }
    for (unsigned i = 0; ok && i < 200 && mr.count < 4; ++i) {
        struct timespec delay = { 0, 1000000L };
        nanosleep(&delay, NULL);
    }
    ok = ok && mr.count == 4 && !mr.failed;
    if (tx) destroy(tx); if (rx) destroy(rx);

    /* Local callers may be concurrent even though the shared transport ring is
     * SPSC.  Exercise request publication and the complete RR reply pairing. */
    ok = ok && concurrent_sender_regression(create, destroy, 0) &&
         concurrent_sender_regression(create, destroy, 1);

    /* Teardown must wake every one-way sender blocked on a full request ring.
     * Two waiters catch the old single-sem_post teardown deadlock. */
    sender = (fss_endpoint_t){0}; tx = create(NULL); rx = NULL;
    make_channel_name(name, sizeof(name));
    fss_channel_t *close_ow = tx ? tx->channel(tx->self, name, 0, 1, NULL, 0) : NULL;
    uint8_t close_byte = 0;
    fss_carrier_t close_carrier = { .id = 1, .data_ptr = &close_byte, .data_size = 1 };
    ok = ok && close_ow && tx->attach(tx->self, close_ow, &sender, FSS_ROLE_INITIATOR) == FSS_SEND_OK;
    for (unsigned i = 0; ok && i < 256; ++i)
        ok = sender.send(sender.send_ctx, &close_carrier) == FSS_SEND_OK;
    atomic_int send_done[2];
    send_thread_arg close_send_args[2];
    pthread_t close_send_workers[2];
    for (unsigned i = 0; i < 2; ++i) {
        atomic_init(&send_done[i], 0);
        close_send_args[i] = (send_thread_arg){ .sender = &sender,
            .carrier = &close_carrier, .result = -1, .done = &send_done[i] };
    }
    if (ok && pthread_create(&close_send_workers[0], NULL, send_thread,
                             &close_send_args[0]) == 0 &&
        pthread_create(&close_send_workers[1], NULL, send_thread,
                       &close_send_args[1]) == 0) {
        /* Give both workers time to enter sem_wait before teardown. */
        struct timespec delay = { 0, 10000000L };
        nanosleep(&delay, NULL);
        destroy_thread_arg close_destroy = { .destroy = destroy, .transport = tx };
        atomic_init(&close_destroy.done, 0);
        pthread_t close_destroy_worker;
        if (pthread_create(&close_destroy_worker, NULL, destroy_thread, &close_destroy) != 0 ||
            !join_bounded(close_destroy_worker, &close_destroy.done)) return 1;
        tx = NULL;
        ok = join_bounded(close_send_workers[0], &send_done[0]) &&
             join_bounded(close_send_workers[1], &send_done[1]) &&
             close_send_args[0].result == FSS_SEND_UNBOUND &&
             close_send_args[1].result == FSS_SEND_UNBOUND;
    } else ok = 0;

    /* Round-trip teardown must also wake all senders awaiting reply_full. */
    sender = (fss_endpoint_t){0}; tx = create(NULL); rx = create(NULL);
    make_channel_name(name, sizeof(name));
    fss_channel_t *close_rr_tx = tx ? tx->channel(tx->self, name, 1, 1, NULL, 0) : NULL;
    fss_channel_t *close_rr_rx = rx ? rx->channel(rx->self, name, 1, 0, NULL, 0) : NULL;
    ok = ok && close_rr_tx && close_rr_rx &&
         tx->attach(tx->self, close_rr_tx, &sender, FSS_ROLE_INITIATOR) == FSS_SEND_OK;
    for (unsigned i = 0; i < 2; ++i) {
        atomic_init(&send_done[i], 0);
        close_send_args[i] = (send_thread_arg){ .sender = &sender,
            .carrier = &close_carrier, .result = -1, .done = &send_done[i] };
    }
    int close_rr_first_started = ok &&
        pthread_create(&close_send_workers[0], NULL, send_thread,
                       &close_send_args[0]) == 0;
    /* The transport ring is SPSC.  Let the first sender publish its request
     * and block for a reply before the second sender writes its own request. */
    if (close_rr_first_started) {
        struct timespec delay = { 0, 10000000L };
        nanosleep(&delay, NULL);
    }
    if (close_rr_first_started &&
        pthread_create(&close_send_workers[1], NULL, send_thread,
                       &close_send_args[1]) == 0) {
        struct timespec delay = { 0, 10000000L };
        nanosleep(&delay, NULL);
        destroy_thread_arg close_destroy = { .destroy = destroy, .transport = tx };
        atomic_init(&close_destroy.done, 0);
        pthread_t close_destroy_worker;
        if (pthread_create(&close_destroy_worker, NULL, destroy_thread, &close_destroy) != 0 ||
            !join_bounded(close_destroy_worker, &close_destroy.done)) return 1;
        tx = NULL;
        ok = join_bounded(close_send_workers[0], &send_done[0]) &&
             join_bounded(close_send_workers[1], &send_done[1]) &&
             close_send_args[0].result == FSS_SEND_UNBOUND &&
             close_send_args[1].result == FSS_SEND_UNBOUND;
    } else ok = 0;
    if (rx) destroy(rx);

    /* A reader asleep on sem_full must be joined before its map is removed. */
    sender = (fss_endpoint_t){0}; sink = (fss_endpoint_t){0};
    count_record close_reader_count = {0}; sink.self = &close_reader_count; sink.receive = count_sink;
    make_channel_name(name, sizeof(name));
    tx = rx = NULL;
    ok = ok && setup_pair(create, name, 0, &tx, &rx, &sender, &sink);
    destroy_thread_arg close_reader = { .destroy = destroy, .transport = rx };
    atomic_init(&close_reader.done, 0);
    pthread_t close_reader_worker;
    if (ok && pthread_create(&close_reader_worker, NULL, destroy_thread, &close_reader) == 0) {
        ok = join_bounded(close_reader_worker, &close_reader.done);
        rx = NULL;
    } else ok = 0;
    if (tx) destroy(tx);

    /* An index other than UINT32_MAX must resolve to published metadata.  Do
     * not deliver a corrupt frame as an untyped carrier. */
    count_record bad_meta = {0}; sender = (fss_endpoint_t){0}; sink = (fss_endpoint_t){0};
    sink.self = &bad_meta; sink.receive = count_sink;
    make_channel_name(name, sizeof(name));
    tx = create(NULL); rx = create(NULL);
    fss_channel_t *bad_tx = tx ? tx->channel(tx->self, name, 0, 1, NULL, 0) : NULL;
    fss_channel_t *bad_rx = rx ? rx->channel(rx->self, name, 0, 0, NULL, 0) : NULL;
    uint8_t bad_byte = 0;
    fss_carrier_t bad_carrier = { .id = 8, .data_ptr = &bad_byte, .data_size = 1 };
    ok = ok && bad_tx && bad_rx &&
         tx->attach(tx->self, bad_tx, &sender, FSS_ROLE_INITIATOR) == FSS_SEND_OK &&
         sender.send(sender.send_ctx, &bad_carrier) == FSS_SEND_OK &&
         queue_bad_meta_index(name, 0) &&
         rx->attach(rx->self, bad_rx, &sink, FSS_ROLE_TARGET) == FSS_SEND_OK;
    for (unsigned i = 0; ok && i < 20; ++i) {
        struct timespec delay = { 0, 1000000L };
        nanosleep(&delay, NULL);
    }
    ok = ok && bad_meta.count == 0;
    if (tx) destroy(tx); if (rx) destroy(rx);

    /* The same corruption on a request/reply channel must release the sender
     * with the transport invalid-frame error, not leave it blocked. */
    bad_meta = (count_record){0}; sender = (fss_endpoint_t){0}; sink = (fss_endpoint_t){0};
    sink.self = &bad_meta; sink.receive = count_sink;
    make_channel_name(name, sizeof(name));
    tx = create(NULL); rx = create(NULL);
    bad_tx = tx ? tx->channel(tx->self, name, 1, 1, NULL, 0) : NULL;
    bad_rx = rx ? rx->channel(rx->self, name, 1, 0, NULL, 0) : NULL;
    bad_carrier = (fss_carrier_t){ .id = 9, .data_ptr = &bad_byte, .data_size = 1 };
    send_arg = (send_thread_arg){ .sender = &sender, .carrier = &bad_carrier, .result = -1 };
    pthread_t send_worker;
    int thread_started = 0;
    ok = ok && bad_tx && bad_rx &&
         tx->attach(tx->self, bad_tx, &sender, FSS_ROLE_INITIATOR) == FSS_SEND_OK;
    if (ok && pthread_create(&send_worker, NULL, send_thread, &send_arg) == 0)
        thread_started = 1;
    else
        ok = 0;
    int corrupt_ok = queue_bad_meta_index(name, 1);
    /* Always start the reader once the send thread exists, so a failed test
     * setup cannot strand that thread on the reply semaphore. */
    int attach_ok = rx && bad_rx &&
        rx->attach(rx->self, bad_rx, &sink, FSS_ROLE_TARGET) == FSS_SEND_OK;
    ok = ok && corrupt_ok && attach_ok;
    if (thread_started) pthread_join(send_worker, NULL);
    ok = ok && send_arg.result == 3 && bad_meta.count == 0;
    if (tx) destroy(tx); if (rx) destroy(rx);

    /* Do not apply status or data from a reply whose id differs from the
     * request's transmitted identity. */
    sender = (fss_endpoint_t){0}; sink = (fss_endpoint_t){0};
    sink.receive = mismatch_reply_sink;
    make_channel_name(name, sizeof(name));
    tx = create(NULL); rx = create(NULL);
    fss_channel_t *id_tx = tx ? tx->channel(tx->self, name, 1, 1, NULL, 0) : NULL;
    fss_channel_t *id_rx = rx ? rx->channel(rx->self, name, 1, 0, NULL, 0) : NULL;
    uint8_t id_byte = 0xa5;
    fss_carrier_t id_carrier = { .id = UINT64_C(0x1234),
                                 .data_ptr = &id_byte, .data_size = 1 };
    send_arg = (send_thread_arg){ .sender = &sender, .carrier = &id_carrier, .result = -1 };
    thread_started = 0;
    ok = ok && id_tx && id_rx &&
         tx->attach(tx->self, id_tx, &sender, FSS_ROLE_INITIATOR) == FSS_SEND_OK;
    if (ok && pthread_create(&send_worker, NULL, send_thread, &send_arg) == 0)
        thread_started = 1;
    else
        ok = 0;
    int id_corrupt_ok = queue_mismatched_carrier_id(name, UINT64_C(0x5678));
    attach_ok = rx && id_rx &&
        rx->attach(rx->self, id_rx, &sink, FSS_ROLE_TARGET) == FSS_SEND_OK;
    ok = ok && id_corrupt_ok && attach_ok;
    if (thread_started) pthread_join(send_worker, NULL);
    ok = ok && send_arg.result == 3 && id_byte == 0xa5;
    if (tx) destroy(tx); if (rx) destroy(rx);

    reply_record rr = {0}; sender = (fss_endpoint_t){0}; sink = (fss_endpoint_t){0};
    sink.self = &rr; sink.receive = reply_sink;
    make_channel_name(name, sizeof(name));
    tx = rx = NULL;
    ok = ok && setup_pair(create, name, 1, &tx, &rx, &sender, &sink);
    uint8_t guarded[SLOT_DATA_CAP + 2];
    memset(guarded, 0, sizeof(guarded)); guarded[0] = 0xa5; guarded[sizeof(guarded) - 1] = 0x5a;
    fss_carrier_t c = { .id = 1, .data_ptr = guarded + 1, .data_size = SLOT_DATA_CAP };
    ok = ok && sender.send(sender.send_ctx, &c) == FSS_SEND_OK &&
         guarded[0] == 0xa5 && guarded[sizeof(guarded) - 1] == 0x5a &&
         guarded[1] == 0x5a && guarded[SLOT_DATA_CAP] == 0x5a;
    rr.mode = 1;
    memset(guarded + 1, 0, SLOT_DATA_CAP);
    ok = ok && sender.send(sender.send_ctx, &c) == 3 &&
         guarded[0] == 0xa5 && guarded[sizeof(guarded) - 1] == 0x5a;
    rr.mode = 2;
    c.data_ptr = NULL; c.data_size = 0;
    ok = ok && sender.send(sender.send_ctx, &c) == 3;
    rr.mode = 3;
    ok = ok && sender.send(sender.send_ctx, &c) == FSS_SEND_OK;
    if (tx) destroy(tx); if (rx) destroy(rx);

    /* The round-trip bit selects a different physical layout.  An attacher
     * must reject either mismatch before it derives ring/meta-table addresses.
     * It is only ABI-layout compatibility: the carrier has no direction marker
     * available at channel() time. */
    for (int owner_rr = 0; ok && owner_rr <= 1; ++owner_rr) {
        make_channel_name(name, sizeof(name));
        tx = create(NULL); rx = create(NULL);
        fss_channel_t *owner_ch = tx ? tx->channel(tx->self, name, owner_rr, 1, NULL, 0) : NULL;
        fss_channel_t *wrong_ch = rx ? rx->channel(rx->self, name, !owner_rr, 0, NULL, 0) : NULL;
        ok = owner_ch && !wrong_ch;
        if (tx) destroy(tx); if (rx) destroy(rx);
    }

    /* Darwin POSIX SHM objects cannot be resized after creation. */
#if !defined(__APPLE__)
    /* File size is a lower bound, not another layout discriminator.  The
     * header must still match exactly; only trailing backing storage is OK. */
    make_channel_name(name, sizeof(name));
    tx = create(NULL); rx = create(NULL);
    fss_channel_t *oversize_owner = tx ? tx->channel(tx->self, name, 0, 1, NULL, 0) : NULL;
    int enlarged = oversize_owner && extend_backing_file(name);
    fss_channel_t *oversize_attach = enlarged && rx
        ? rx->channel(rx->self, name, 0, 0, NULL, 0) : NULL;
    ok = ok && oversize_owner && enlarged && oversize_attach;
    if (tx) destroy(tx); if (rx) destroy(rx);

    /* A complete matching header does not make a truncated backing object
     * usable: the full expected layout must be present before the full mmap. */
    make_channel_name(name, sizeof(name));
    tx = create(NULL); rx = create(NULL);
    fss_channel_t *undersize_owner = tx ? tx->channel(tx->self, name, 0, 1, NULL, 0) : NULL;
    int truncated = undersize_owner && truncate_backing_file(name);
    fss_channel_t *undersize_attach = truncated && rx
        ? rx->channel(rx->self, name, 0, 0, NULL, 0) : NULL;
    ok = ok && undersize_owner && truncated && !undersize_attach;
    if (tx) destroy(tx); if (rx) destroy(rx);
#endif

    /* First sink wins.  A repeat with the same sink is harmless; a different
     * sink cannot replace a running reader or create a second reader. */
    count_record first = {0}, second = {0};
    fss_endpoint_t first_sink = { .self = &first, .receive = count_sink };
    fss_endpoint_t second_sink = { .self = &second, .receive = count_sink };
    sender = (fss_endpoint_t){0}; tx = rx = NULL;
    make_channel_name(name, sizeof(name));
    fss_channel_t *sink_tx = NULL, *sink_rx = NULL;
    tx = create(NULL); rx = create(NULL);
    sink_tx = tx ? tx->channel(tx->self, name, 0, 1, NULL, 0) : NULL;
    sink_rx = rx ? rx->channel(rx->self, name, 0, 0, NULL, 0) : NULL;
    ok = ok && sink_tx && sink_rx &&
         tx->attach(tx->self, sink_tx, &sender, FSS_ROLE_INITIATOR) == FSS_SEND_OK &&
         rx->attach(rx->self, sink_rx, &first_sink, FSS_ROLE_TARGET) == FSS_SEND_OK &&
         rx->attach(rx->self, sink_rx, &first_sink, FSS_ROLE_TARGET) == FSS_SEND_OK &&
         rx->attach(rx->self, sink_rx, &second_sink, FSS_ROLE_TARGET) == FSS_SEND_UNBOUND;
    uint8_t sink_byte = 0;
    fss_carrier_t sink_carrier = { .id = 7, .data_ptr = &sink_byte, .data_size = 1 };
    ok = ok && sender.send(sender.send_ctx, &sink_carrier) == FSS_SEND_OK;
    for (unsigned i = 0; ok && i < 200 && first.count == 0; ++i) {
        struct timespec delay = { 0, 1000000L };
        nanosleep(&delay, NULL);
    }
    ok = ok && first.count == 1 && second.count == 0;
    if (tx) destroy(tx); if (rx) destroy(rx);
    dlclose(dl);
    return ok ? 0 : 1;
}
