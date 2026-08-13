/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE

#include "uffd_mode.h"

#include "obmm_uffd.h"
#include "logical_op.h"
#include "uffd_state.h"

#include <errno.h>
#include <linux/userfaultfd.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define OBMM_UFFD_PAGE_BYTES 4096UL
#define OBMM_UFFD_HANDLER_STACK_BYTES (128UL * 1024UL)
#define OBMM_UFFD_POLL_MS 20
#define OBMM_UFFD_QUIESCE_TIMEOUT_NS (5ULL * 1000000000ULL)
#define OBMM_UFFD_MAX_SAMPLES 4096

struct obmm_uffd_runtime {
    const struct obmm_uffd_run_config *config;
    struct obmm_uffd uffd;
    uint8_t *shadow;
    size_t shadow_length;
    struct obmm_uffd_page_record *records;
    uint64_t *logical_ordinals;
    uint64_t *fault_samples;
    uint64_t *remote_samples;
    uint64_t *copy_samples;
    uint64_t *wake_samples;
    atomic_uint_fast64_t *remote_by_page;
    atomic_uint_fast64_t *copy_by_page;
    uint64_t sample_capacity;
    atomic_uint_fast64_t sample_count;
    atomic_uint_fast64_t faults;
    atomic_uint_fast64_t remote_reads;
    atomic_uint_fast64_t copy_ok;
    atomic_uint_fast64_t duplicates;
    atomic_uint_fast64_t failures;
    atomic_uint_fast32_t handler_inflight;
    atomic_bool stop;
    atomic_bool fatal;
    pthread_mutex_t ready_lock;
    pthread_cond_t ready_cond;
    bool handler_ready;
    uint64_t generation;
    uint64_t source_generation;
    uint64_t handler_cpu_ns;
};

struct obmm_uffd_worker {
    struct obmm_uffd_runtime *runtime;
    uint64_t operation_base;
    uint64_t count;
    uint64_t stride;
    uint32_t worker_id;
    bool unique_pages;
    bool measurement;
    uint64_t checksum;
    uint64_t cpu_ns;
    uint64_t verify_failures;
    int status;
};

static uint64_t obmm_uffd_now_ns(clockid_t clock_id)
{
    struct timespec now;

    if (clock_gettime(clock_id, &now) != 0) {
        return 0;
    }
    return (uint64_t)now.tv_sec * 1000000000ULL + now.tv_nsec;
}

static uint64_t obmm_uffd_checksum(const volatile uint8_t *bytes,
                                   size_t length)
{
    uint64_t hash = 14695981039346656037ULL;
    size_t index;

    for (index = 0; index < length; index++) {
        hash ^= bytes[index];
        hash *= 1099511628211ULL;
    }
    return hash;
}

static uint8_t obmm_uffd_pattern_byte(uint64_t seed, uint64_t offset)
{
    return (uint8_t)((seed + offset) * 0x9e3779b9U + 0x85ebca77U);
}

static bool obmm_uffd_verify_page(const volatile uint8_t *bytes,
                                  uint64_t page_index, uint64_t seed)
{
    uint64_t base = page_index * OBMM_UFFD_PAGE_BYTES;
    size_t index;

    for (index = 0; index < OBMM_UFFD_PAGE_BYTES; index++) {
        if (bytes[index] != obmm_uffd_pattern_byte(seed, base + index)) {
            return false;
        }
    }
    return true;
}

static int obmm_uffd_pin_cpu(int cpu)
{
    cpu_set_t affinity;
    int ret;

    CPU_ZERO(&affinity);
    CPU_SET(cpu, &affinity);
    ret = pthread_setaffinity_np(pthread_self(), sizeof(affinity),
                                 &affinity);
    return ret ? -ret : 0;
}

static bool obmm_uffd_trace_page(const struct obmm_uffd_runtime *runtime,
                                 uint64_t page)
{
    uint64_t draw;

    if (!runtime->config->trace_sample_ppm) {
        return false;
    }
    draw = obmm_logical_splitmix64(
        runtime->config->seed ^ runtime->logical_ordinals[page] ^
        0x756666645f747631ULL) % 1000000;
    return draw < runtime->config->trace_sample_ppm;
}

static void obmm_uffd_reset_phase(struct obmm_uffd_runtime *runtime)
{
    uint32_t page;

    runtime->generation++;
    if (!runtime->generation) {
        runtime->generation++;
    }
    for (page = 0; page < runtime->config->pages; page++) {
        obmm_uffd_page_reset(&runtime->records[page],
                             runtime->generation);
    }
}

static uint64_t obmm_uffd_operation_key(
    const struct obmm_uffd_runtime *runtime, uint32_t page)
{
    return obmm_logical_splitmix64(
        runtime->config->seed ^ runtime->generation ^
        runtime->logical_ordinals[page]);
}

static void obmm_uffd_fail_stop(struct obmm_uffd_runtime *runtime,
                                void *fault_page)
{
    atomic_store_explicit(&runtime->fatal, true, memory_order_release);
    atomic_fetch_add_explicit(&runtime->failures, 1,
                              memory_order_relaxed);
    if (fault_page && runtime->uffd.poison_supported) {
        uint64_t page = ((uintptr_t)fault_page -
                         (uintptr_t)runtime->shadow) /
            OBMM_UFFD_PAGE_BYTES;

        if (obmm_uffd_poison(&runtime->uffd, fault_page,
                             OBMM_UFFD_PAGE_BYTES) == 0) {
            fprintf(stderr, "obmm_uffd_resolve operation_key=%016llx "
                    "ioctl=poison status=remote-failure\n",
                    (unsigned long long)obmm_uffd_operation_key(
                        runtime, page));
            fflush(NULL);
            pthread_exit(NULL);
        }
    }
    fprintf(stderr, "OBMM_UFFD_FAIL_STOP failures=%llu\n",
            (unsigned long long)atomic_load_explicit(
                &runtime->failures, memory_order_relaxed));
    _exit(125);
}

static void obmm_uffd_record_sample(struct obmm_uffd_runtime *runtime,
                                    uint64_t fault_ns, uint64_t remote_ns,
                                    uint64_t copy_ns, uint64_t wake_ns)
{
    uint64_t index = atomic_fetch_add_explicit(
        &runtime->sample_count, 1, memory_order_relaxed);

    if (index >= runtime->sample_capacity) {
        atomic_store_explicit(&runtime->sample_count,
                              runtime->sample_capacity,
                              memory_order_relaxed);
        return;
    }
    runtime->fault_samples[index] = fault_ns;
    runtime->remote_samples[index] = remote_ns;
    runtime->copy_samples[index] = copy_ns;
    runtime->wake_samples[index] = wake_ns;
}

static void obmm_uffd_handle_fault(struct obmm_uffd_runtime *runtime,
                                   const struct obmm_uffd_fault *fault)
{
    const struct obmm_uffd_run_config *config = runtime->config;
    struct obmm_uffd_page_record *record;
    struct obmm_async_future future;
    struct obmm_async_result result = { 0 };
    enum obmm_uffd_fault_claim claim;
    uint64_t fault_page;
    uint64_t page_index;
    uint64_t fault_start;
    uint64_t remote_start;
    uint64_t remote_end;
    uint64_t copy_start;
    uint64_t copy_end;
    uint64_t deadline_ns = 0;
    uint64_t checksum;
    int ret;

    fault_page = fault->address & ~(OBMM_UFFD_PAGE_BYTES - 1);
    if (fault->flags & UFFD_PAGEFAULT_FLAG_WP) {
        obmm_uffd_fail_stop(runtime, NULL);
    }
#ifdef UFFD_PAGEFAULT_FLAG_MINOR
    if (fault->flags & UFFD_PAGEFAULT_FLAG_MINOR) {
        obmm_uffd_fail_stop(runtime, NULL);
    }
#endif
    if (fault_page < (uintptr_t)runtime->shadow ||
        fault_page >= (uintptr_t)runtime->shadow + runtime->shadow_length) {
        obmm_uffd_fail_stop(runtime, NULL);
    }
    page_index = (fault_page - (uintptr_t)runtime->shadow) /
        OBMM_UFFD_PAGE_BYTES;
    record = &runtime->records[page_index];
    claim = obmm_uffd_page_claim(record, runtime->generation);
    atomic_fetch_add_explicit(&runtime->faults, 1,
                              memory_order_relaxed);
    if (claim == OBMM_UFFD_FAULT_DUPLICATE) {
        atomic_fetch_add_explicit(&runtime->duplicates, 1,
                                  memory_order_relaxed);
        return;
    }
    if (claim != OBMM_UFFD_FAULT_OWNER ||
        !obmm_uffd_page_remote_begin(record, runtime->generation) ||
        config->remote_map->generation != runtime->source_generation) {
        obmm_uffd_fail_stop(runtime, (void *)(uintptr_t)fault_page);
    }

    fault_start = obmm_uffd_now_ns(CLOCK_MONOTONIC);
    if (obmm_uffd_trace_page(runtime, page_index)) {
        printf("obmm_uffd_fault operation_key=%016llx page=%llu "
               "guest_ns=%llu\n",
               (unsigned long long)obmm_uffd_operation_key(
                   runtime, page_index),
               (unsigned long long)page_index,
               (unsigned long long)fault_start);
    }
    remote_start = obmm_uffd_now_ns(CLOCK_MONOTONIC);
    if (config->deadline_us) {
        deadline_ns = remote_start +
            (uint64_t)config->deadline_us * 1000;
    }
    ret = obmm_load_submit(
        config->remote_runtime, config->remote_map,
        page_index * OBMM_UFFD_PAGE_BYTES, config->staging_buffer, 0,
        OBMM_UFFD_PAGE_BYTES, deadline_ns,
        runtime->logical_ordinals[page_index], &future);
    if (!ret) {
        ret = obmm_await(config->remote_runtime, &future, &result);
    }
    remote_end = obmm_uffd_now_ns(CLOCK_MONOTONIC);
    if (ret || result.bytes_done != OBMM_UFFD_PAGE_BYTES) {
        obmm_uffd_page_fail(record, runtime->generation,
                            runtime->uffd.poison_supported);
        obmm_uffd_fail_stop(runtime, (void *)(uintptr_t)fault_page);
    }
    checksum = obmm_uffd_checksum(config->staging_buffer->data,
                                  OBMM_UFFD_PAGE_BYTES);
    if (checksum != result.checksum64 ||
        (config->verify && !obmm_uffd_verify_page(
             config->staging_buffer->data, page_index, config->seed)) ||
        !obmm_uffd_page_remote_done(record, runtime->generation,
                                    checksum) ||
        !obmm_uffd_page_copy_begin(record, runtime->generation)) {
        obmm_uffd_page_fail(record, runtime->generation,
                            runtime->uffd.poison_supported);
        obmm_uffd_fail_stop(runtime, (void *)(uintptr_t)fault_page);
    }
    atomic_fetch_add_explicit(&runtime->remote_reads, 1,
                              memory_order_relaxed);
    if (obmm_uffd_trace_page(runtime, page_index)) {
        printf("obmm_uffd_remote_done operation_key=%016llx "
               "status=success guest_ns=%llu\n",
               (unsigned long long)obmm_uffd_operation_key(
                   runtime, page_index),
               (unsigned long long)remote_end);
    }

    copy_start = obmm_uffd_now_ns(CLOCK_MONOTONIC);
    ret = obmm_uffd_copy(&runtime->uffd,
                         (void *)(uintptr_t)fault_page,
                         config->staging_buffer->data,
                         OBMM_UFFD_PAGE_BYTES);
    copy_end = obmm_uffd_now_ns(CLOCK_MONOTONIC);
    if (ret == -EEXIST) {
        uint64_t existing = obmm_uffd_checksum(
            (const volatile uint8_t *)(uintptr_t)fault_page,
            OBMM_UFFD_PAGE_BYTES);

        if (!obmm_uffd_page_resolve(record, runtime->generation, true,
                                    existing)) {
            obmm_uffd_fail_stop(runtime,
                                (void *)(uintptr_t)fault_page);
        }
        atomic_fetch_add_explicit(&runtime->duplicates, 1,
                                  memory_order_relaxed);
    } else if (ret || !obmm_uffd_page_resolve(
                   record, runtime->generation, false, checksum)) {
        obmm_uffd_fail_stop(runtime, (void *)(uintptr_t)fault_page);
    }
    atomic_fetch_add_explicit(&runtime->copy_ok, 1,
                              memory_order_relaxed);
    atomic_store_explicit(&runtime->remote_by_page[page_index],
                          remote_end - remote_start,
                          memory_order_release);
    atomic_store_explicit(&runtime->copy_by_page[page_index],
                          copy_end - copy_start,
                          memory_order_release);
    if (obmm_uffd_trace_page(runtime, page_index)) {
        printf("obmm_uffd_resolve operation_key=%016llx ioctl=copy "
               "status=success guest_ns=%llu\n",
               (unsigned long long)obmm_uffd_operation_key(
                   runtime, page_index),
               (unsigned long long)copy_end);
    }
}

static void *obmm_uffd_handler_main(void *opaque)
{
    struct obmm_uffd_runtime *runtime = opaque;
    uint64_t cpu_start;

    if (obmm_uffd_pin_cpu(runtime->config->handler_cpu) != 0) {
        atomic_store_explicit(&runtime->fatal, true, memory_order_release);
    }
    cpu_start = obmm_uffd_now_ns(CLOCK_THREAD_CPUTIME_ID);
    pthread_mutex_lock(&runtime->ready_lock);
    runtime->handler_ready = true;
    pthread_cond_signal(&runtime->ready_cond);
    pthread_mutex_unlock(&runtime->ready_lock);

    while (!atomic_load_explicit(&runtime->stop, memory_order_acquire) &&
           !atomic_load_explicit(&runtime->fatal, memory_order_acquire)) {
        struct pollfd poll_fd = {
            .fd = runtime->uffd.fd,
            .events = POLLIN,
        };
        int ready = poll(&poll_fd, 1, OBMM_UFFD_POLL_MS);

        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            obmm_uffd_fail_stop(runtime, NULL);
        }
        if (!ready) {
            continue;
        }
        if (poll_fd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
            if (atomic_load_explicit(&runtime->stop,
                                     memory_order_acquire)) {
                break;
            }
            obmm_uffd_fail_stop(runtime, NULL);
        }
        if (poll_fd.revents & POLLIN) {
            struct obmm_uffd_fault fault;
            int ret;

            atomic_fetch_add_explicit(&runtime->handler_inflight, 1,
                                      memory_order_acq_rel);
            ret = obmm_uffd_read_fault(&runtime->uffd, &fault);
            if (ret == -EAGAIN) {
                atomic_fetch_sub_explicit(&runtime->handler_inflight, 1,
                                          memory_order_release);
                continue;
            }
            if (ret) {
                obmm_uffd_fail_stop(runtime, NULL);
            }
            obmm_uffd_handle_fault(runtime, &fault);
            atomic_fetch_sub_explicit(&runtime->handler_inflight, 1,
                                      memory_order_release);
        }
    }
    runtime->handler_cpu_ns =
        obmm_uffd_now_ns(CLOCK_THREAD_CPUTIME_ID) - cpu_start;
    return NULL;
}

static void *obmm_uffd_worker_main(void *opaque)
{
    struct obmm_uffd_worker *worker = opaque;
    struct obmm_uffd_runtime *runtime = worker->runtime;
    int online_cpus = sysconf(_SC_NPROCESSORS_ONLN);
    uint64_t cpu_start;
    uint64_t index;
    int worker_cpu;

    worker_cpu = (runtime->config->handler_cpu + 1 +
                  worker->worker_id % (online_cpus - 1)) % online_cpus;
    worker->status = obmm_uffd_pin_cpu(worker_cpu);
    if (worker->status) {
        return NULL;
    }
    cpu_start = obmm_uffd_now_ns(CLOCK_THREAD_CPUTIME_ID);
    for (index = 0; index < worker->count; index++) {
        uint64_t started_ns = obmm_uffd_now_ns(CLOCK_MONOTONIC);
        uint64_t ordinal = worker->operation_base +
            index * worker->stride;
        uint64_t local_ordinal = ordinal /
            runtime->config->worker_threads;
        uint32_t page = obmm_logical_worker_page(
            runtime->config->seed, worker->worker_id,
            local_ordinal, runtime->config->worker_threads,
            runtime->config->pages,
            runtime->config->random_pattern);
        const volatile uint8_t *address = runtime->shadow +
            (uint64_t)page * OBMM_UFFD_PAGE_BYTES;
        uint64_t woken_ns;
        uint64_t fault_ns;
        uint64_t remote_ns;
        uint64_t copy_ns;
        uint64_t checksum;
        volatile uint8_t first_byte;

        first_byte = address[0];
        (void)first_byte;
        woken_ns = obmm_uffd_now_ns(CLOCK_MONOTONIC);
        fault_ns = woken_ns - started_ns;
        remote_ns = atomic_load_explicit(&runtime->remote_by_page[page],
                                         memory_order_acquire);
        copy_ns = atomic_load_explicit(&runtime->copy_by_page[page],
                                       memory_order_acquire);
        checksum = obmm_uffd_checksum(address, OBMM_UFFD_PAGE_BYTES);

        if (runtime->config->verify &&
            !obmm_uffd_verify_page(address, page,
                                   runtime->config->seed)) {
            worker->verify_failures++;
        }
        if (worker->measurement) {
            if (runtime->config->test_case ==
                OBMM_UFFD_CASE_MISSING_REMOTE) {
                uint64_t wake_ns = fault_ns > remote_ns + copy_ns ?
                    fault_ns - remote_ns - copy_ns : 0;

                obmm_uffd_record_sample(runtime, fault_ns, remote_ns,
                                        copy_ns, wake_ns);
            }
            worker->checksum ^= checksum + ordinal;
            if (runtime->config->trace) {
                runtime->config->trace(
                    runtime->config->trace_opaque, ordinal,
                    (uint64_t)page * OBMM_UFFD_PAGE_BYTES, 0,
                    fault_ns);
            }
        }
    }
    worker->cpu_ns = obmm_uffd_now_ns(CLOCK_THREAD_CPUTIME_ID) - cpu_start;
    return NULL;
}

static int obmm_uffd_run_workers(struct obmm_uffd_runtime *runtime,
                                 uint64_t operation_base,
                                 uint64_t operation_count,
                                 bool unique_pages, bool measurement,
                                 uint64_t *checksum, uint64_t *cpu_ns)
{
    const uint32_t worker_count = runtime->config->worker_threads;
    struct obmm_uffd_worker *workers;
    pthread_t *threads;
    uint64_t operation_end = operation_base + operation_count;
    uint32_t created = 0;
    uint32_t index;
    int ret = 0;

    workers = calloc(worker_count, sizeof(*workers));
    threads = calloc(worker_count, sizeof(*threads));
    if (!workers || !threads) {
        free(threads);
        free(workers);
        return -ENOMEM;
    }
    for (index = 0; index < worker_count; index++) {
        uint64_t first = operation_base + index;
        uint64_t count = first < operation_end ?
            (operation_end - first + worker_count - 1) / worker_count : 0;
        uint64_t local_index;

        workers[index] = (struct obmm_uffd_worker) {
            .runtime = runtime,
            .operation_base = first,
            .count = count,
            .stride = worker_count,
            .worker_id = index,
            .unique_pages = unique_pages,
            .measurement = measurement,
        };
        if (unique_pages) {
            for (local_index = 0; local_index < count; local_index++) {
                uint64_t ordinal = first + local_index * worker_count;
                uint64_t local_ordinal = ordinal / worker_count;
                uint64_t page = obmm_logical_worker_page(
                    runtime->config->seed, index, local_ordinal,
                    worker_count, runtime->config->pages,
                    runtime->config->random_pattern);

                runtime->logical_ordinals[page] =
                    obmm_logical_remote_ordinal(
                        index, local_ordinal, worker_count, 0);
            }
        }
        if (!count) {
            continue;
        }
        ret = pthread_create(&threads[index], NULL,
                             obmm_uffd_worker_main, &workers[index]);
        if (ret) {
            ret = -ret;
            break;
        }
        created = index + 1;
    }
    for (index = 0; index < created; index++) {
        if (workers[index].count) {
            pthread_join(threads[index], NULL);
        }
    }
    if (!ret) {
        for (index = 0; index < worker_count; index++) {
            if (workers[index].status || workers[index].verify_failures) {
                ret = workers[index].status ? workers[index].status : -EIO;
                break;
            }
            *checksum ^= workers[index].checksum;
            *cpu_ns += workers[index].cpu_ns;
        }
    }
    free(threads);
    free(workers);
    return ret;
}

static int obmm_uffd_compare_u64(const void *left, const void *right)
{
    uint64_t a = *(const uint64_t *)left;
    uint64_t b = *(const uint64_t *)right;

    return (a > b) - (a < b);
}

static uint64_t obmm_uffd_percentile(uint64_t *values, uint64_t count,
                                     uint32_t percentile)
{
    if (!count) {
        return 0;
    }
    qsort(values, count, sizeof(*values), obmm_uffd_compare_u64);
    return values[((count - 1) * percentile) / 100];
}

static int obmm_uffd_prefault_lock(void *address, size_t length)
{
    volatile uint8_t *bytes = address;
    size_t offset;

    for (offset = 0; offset < length; offset += OBMM_UFFD_PAGE_BYTES) {
        bytes[offset] = bytes[offset];
    }
    return mlock(address, length) == 0 ? 0 : -errno;
}

static int obmm_uffd_wait_quiescent(struct obmm_uffd_runtime *runtime)
{
    uint64_t deadline = obmm_uffd_now_ns(CLOCK_MONOTONIC) +
        OBMM_UFFD_QUIESCE_TIMEOUT_NS;

    for (;;) {
        struct pollfd poll_fd = {
            .fd = runtime->uffd.fd,
            .events = POLLIN,
        };
        int ready;

        if (atomic_load_explicit(&runtime->fatal,
                                 memory_order_acquire)) {
            return -EIO;
        }
        ready = poll(&poll_fd, 1, 0);
        if (ready < 0 && errno != EINTR) {
            return -errno;
        }
        if (!ready && !atomic_load_explicit(
                &runtime->handler_inflight, memory_order_acquire)) {
            return 0;
        }
        if (ready > 0 && poll_fd.revents &
            (POLLERR | POLLHUP | POLLNVAL)) {
            return -EIO;
        }
        if (obmm_uffd_now_ns(CLOCK_MONOTONIC) >= deadline) {
            return -ETIMEDOUT;
        }
        sched_yield();
    }
}

const char *obmm_uffd_case_name(enum obmm_uffd_case test_case)
{
    return test_case == OBMM_UFFD_CASE_PRESENT_HIT ?
        "present-hit" : "missing-remote";
}

int obmm_uffd_run(const struct obmm_uffd_run_config *config,
                  struct obmm_uffd_metrics *metrics)
{
    struct obmm_uffd_runtime *runtime = MAP_FAILED;
    pthread_attr_t handler_attr;
    pthread_t handler;
    uint8_t *handler_mapping = MAP_FAILED;
    void *handler_stack;
    size_t handler_mapping_bytes = OBMM_UFFD_HANDLER_STACK_BYTES +
        OBMM_UFFD_PAGE_BYTES * 2;
    uint64_t operation = 0;
    uint64_t checksum = 0;
    uint64_t worker_cpu_ns = 0;
    uint64_t warm_faults = 0;
    uint64_t warm_reads = 0;
    uint64_t warm_copies = 0;
    uint64_t warm_duplicates = 0;
    uint64_t warm_samples = 0;
    int online_cpus = sysconf(_SC_NPROCESSORS_ONLN);
    bool attr_initialized = false;
    bool sync_initialized = false;
    bool handler_started = false;
    bool poison_supported = false;
    int ret;

    if (!config || !metrics || !config->remote_runtime ||
        !config->remote_map || !config->staging_buffer ||
        !config->source_base || !config->source_length || !config->pages ||
        !config->worker_threads || !config->iterations ||
        !config->remote_map->id || !config->remote_map->generation ||
        config->pages > config->source_length / OBMM_UFFD_PAGE_BYTES ||
        config->pages % config->worker_threads ||
        config->source_length > config->remote_map->length ||
        config->staging_buffer->length < OBMM_UFFD_PAGE_BYTES ||
        (uintptr_t)config->source_base % OBMM_UFFD_PAGE_BYTES ||
        (uintptr_t)config->source_base + config->source_length <
            (uintptr_t)config->source_base ||
        config->source_length % OBMM_UFFD_PAGE_BYTES) {
        return -EINVAL;
    }
    memset(metrics, 0, sizeof(*metrics));
    if (sysconf(_SC_PAGESIZE) != OBMM_UFFD_PAGE_BYTES || online_cpus < 2 ||
        config->handler_cpu < 0 || config->handler_cpu >= online_cpus) {
        return -EOPNOTSUPP;
    }

    runtime = mmap(NULL, sizeof(*runtime), PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (runtime == MAP_FAILED) {
        return -errno;
    }
    memset(runtime, 0, sizeof(*runtime));
    runtime->uffd.fd = -1;
    runtime->config = config;
    runtime->source_generation = config->remote_map->generation;
    runtime->shadow_length =
        (size_t)config->pages * OBMM_UFFD_PAGE_BYTES;
    runtime->sample_capacity = config->iterations + config->pages;
    if (runtime->sample_capacity > OBMM_UFFD_MAX_SAMPLES) {
        runtime->sample_capacity = OBMM_UFFD_MAX_SAMPLES;
    }
    runtime->shadow = mmap(NULL, runtime->shadow_length,
                           PROT_READ | PROT_WRITE,
                           MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    runtime->records = calloc(config->pages, sizeof(*runtime->records));
    runtime->logical_ordinals = calloc(
        config->pages, sizeof(*runtime->logical_ordinals));
    runtime->fault_samples = calloc(runtime->sample_capacity,
                                    sizeof(*runtime->fault_samples));
    runtime->remote_samples = calloc(runtime->sample_capacity,
                                     sizeof(*runtime->remote_samples));
    runtime->copy_samples = calloc(runtime->sample_capacity,
                                   sizeof(*runtime->copy_samples));
    runtime->wake_samples = calloc(runtime->sample_capacity,
                                   sizeof(*runtime->wake_samples));
    runtime->remote_by_page = calloc(config->pages,
                                     sizeof(*runtime->remote_by_page));
    runtime->copy_by_page = calloc(config->pages,
                                   sizeof(*runtime->copy_by_page));
    if (runtime->shadow == MAP_FAILED || !runtime->records ||
        !runtime->logical_ordinals || !runtime->fault_samples ||
        !runtime->remote_samples || !runtime->copy_samples ||
        !runtime->wake_samples || !runtime->remote_by_page ||
        !runtime->copy_by_page) {
        ret = -ENOMEM;
        goto cleanup;
    }
    if ((uintptr_t)runtime->shadow <
            (uintptr_t)config->source_base + config->source_length &&
        (uintptr_t)config->source_base <
            (uintptr_t)runtime->shadow + runtime->shadow_length) {
        ret = -EADDRINUSE;
        goto cleanup;
    }
    ret = pthread_mutex_init(&runtime->ready_lock, NULL);
    if (ret) {
        ret = -ret;
        goto cleanup;
    }
    ret = pthread_cond_init(&runtime->ready_cond, NULL);
    if (ret) {
        pthread_mutex_destroy(&runtime->ready_lock);
        ret = -ret;
        goto cleanup;
    }
    sync_initialized = true;
    if (obmm_uffd_prefault_lock(runtime, sizeof(*runtime)) ||
        obmm_uffd_prefault_lock(runtime->records,
                                config->pages * sizeof(*runtime->records)) ||
        obmm_uffd_prefault_lock(
            runtime->logical_ordinals,
            config->pages * sizeof(*runtime->logical_ordinals)) ||
        obmm_uffd_prefault_lock(runtime->fault_samples,
                                runtime->sample_capacity *
                                sizeof(*runtime->fault_samples)) ||
        obmm_uffd_prefault_lock(runtime->remote_samples,
                                runtime->sample_capacity *
                                sizeof(*runtime->remote_samples)) ||
        obmm_uffd_prefault_lock(runtime->copy_samples,
                                runtime->sample_capacity *
                                sizeof(*runtime->copy_samples)) ||
        obmm_uffd_prefault_lock(runtime->wake_samples,
                                runtime->sample_capacity *
                                sizeof(*runtime->wake_samples)) ||
        obmm_uffd_prefault_lock(runtime->remote_by_page,
                                config->pages *
                                sizeof(*runtime->remote_by_page)) ||
        obmm_uffd_prefault_lock(runtime->copy_by_page,
                                config->pages *
                                sizeof(*runtime->copy_by_page)) ||
        obmm_uffd_prefault_lock(config->staging_buffer->data,
                                OBMM_UFFD_PAGE_BYTES)) {
        ret = -EOPNOTSUPP;
        goto cleanup;
    }
    ret = obmm_uffd_open(&runtime->uffd);
    if (ret) {
        goto cleanup;
    }
    ret = obmm_uffd_register_missing(&runtime->uffd, runtime->shadow,
                                     runtime->shadow_length);
    if (ret) {
        goto cleanup;
    }
    poison_supported = runtime->uffd.poison_supported;
    handler_mapping = mmap(NULL, handler_mapping_bytes, PROT_NONE,
                           MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (handler_mapping == MAP_FAILED) {
        ret = -errno;
        goto cleanup;
    }
    handler_stack = handler_mapping + OBMM_UFFD_PAGE_BYTES;
    if (mprotect(handler_stack, OBMM_UFFD_HANDLER_STACK_BYTES,
                 PROT_READ | PROT_WRITE) != 0 ||
        obmm_uffd_prefault_lock(handler_stack,
                                OBMM_UFFD_HANDLER_STACK_BYTES)) {
        ret = -EOPNOTSUPP;
        goto cleanup;
    }
    ret = pthread_attr_init(&handler_attr);
    if (ret) {
        ret = -ret;
        goto cleanup;
    }
    attr_initialized = true;
    ret = pthread_attr_setstack(&handler_attr, handler_stack,
                                OBMM_UFFD_HANDLER_STACK_BYTES);
    if (ret) {
        ret = -ret;
        goto cleanup;
    }
    obmm_uffd_reset_phase(runtime);
    ret = pthread_create(&handler, &handler_attr,
                         obmm_uffd_handler_main, runtime);
    if (ret) {
        ret = -ret;
        goto cleanup;
    }
    handler_started = true;
    pthread_mutex_lock(&runtime->ready_lock);
    while (!runtime->handler_ready) {
        pthread_cond_wait(&runtime->ready_cond, &runtime->ready_lock);
    }
    pthread_mutex_unlock(&runtime->ready_lock);
    if (atomic_load_explicit(&runtime->fatal, memory_order_acquire)) {
        ret = -EIO;
        goto cleanup;
    }

    if (config->test_case == OBMM_UFFD_CASE_PRESENT_HIT) {
        ret = obmm_uffd_run_workers(runtime, 0, config->pages,
                                    true, false, &checksum,
                                    &worker_cpu_ns);
        if (ret) {
            goto cleanup;
        }
        ret = obmm_uffd_wait_quiescent(runtime);
        if (ret) {
            goto cleanup;
        }
        warm_faults = atomic_load(&runtime->faults);
        warm_reads = atomic_load(&runtime->remote_reads);
        warm_copies = atomic_load(&runtime->copy_ok);
        warm_duplicates = atomic_load(&runtime->duplicates);
        warm_samples = atomic_load(&runtime->sample_count);
        worker_cpu_ns = 0;
        ret = obmm_uffd_run_workers(runtime, 0, config->iterations,
                                    false, true, &checksum,
                                    &worker_cpu_ns);
    } else {
        while (operation < config->iterations) {
            uint64_t count = config->iterations - operation;

            if (count > config->pages) {
                count = config->pages;
            }
            if (operation) {
                ret = obmm_uffd_wait_quiescent(runtime);
                if (ret) {
                    goto cleanup;
                }
                if (madvise(runtime->shadow, runtime->shadow_length,
                            MADV_DONTNEED) != 0) {
                    ret = -errno;
                    goto cleanup;
                }
                obmm_uffd_reset_phase(runtime);
            }
            ret = obmm_uffd_run_workers(runtime, operation, count,
                                        true, true, &checksum,
                                        &worker_cpu_ns);
            if (ret) {
                break;
            }
            operation += count;
        }
    }
    if (ret) {
        goto cleanup;
    }

    ret = obmm_uffd_wait_quiescent(runtime);
    if (ret) {
        goto cleanup;
    }

    atomic_store_explicit(&runtime->stop, true, memory_order_release);
    ret = obmm_uffd_unregister(&runtime->uffd);
    if (ret) {
        goto cleanup;
    }
    obmm_uffd_close(&runtime->uffd);
    pthread_join(handler, NULL);
    handler_started = false;
    metrics->pages = config->pages;
    metrics->faults = atomic_load(&runtime->faults) - warm_faults;
    metrics->remote_reads =
        atomic_load(&runtime->remote_reads) - warm_reads;
    metrics->copy_ok = atomic_load(&runtime->copy_ok) - warm_copies;
    metrics->duplicates =
        atomic_load(&runtime->duplicates) - warm_duplicates;
    metrics->failures = atomic_load(&runtime->failures);
    metrics->checksum = checksum;
    metrics->handler_cpu_ns = runtime->handler_cpu_ns;
    metrics->worker_cpu_ns = worker_cpu_ns;
    metrics->poison_supported = poison_supported;
    metrics->fault_ns_p50 = obmm_uffd_percentile(
        runtime->fault_samples + warm_samples,
        atomic_load(&runtime->sample_count) - warm_samples, 50);
    metrics->fault_ns_p95 = obmm_uffd_percentile(
        runtime->fault_samples + warm_samples,
        atomic_load(&runtime->sample_count) - warm_samples, 95);
    metrics->fault_ns_p99 = obmm_uffd_percentile(
        runtime->fault_samples + warm_samples,
        atomic_load(&runtime->sample_count) - warm_samples, 99);
    metrics->fault_ns_max = obmm_uffd_percentile(
        runtime->fault_samples + warm_samples,
        atomic_load(&runtime->sample_count) - warm_samples, 100);
    metrics->remote_ns_p50 = obmm_uffd_percentile(
        runtime->remote_samples + warm_samples,
        atomic_load(&runtime->sample_count) - warm_samples, 50);
    metrics->remote_ns_p95 = obmm_uffd_percentile(
        runtime->remote_samples + warm_samples,
        atomic_load(&runtime->sample_count) - warm_samples, 95);
    metrics->remote_ns_p99 = obmm_uffd_percentile(
        runtime->remote_samples + warm_samples,
        atomic_load(&runtime->sample_count) - warm_samples, 99);
    metrics->remote_ns_max = obmm_uffd_percentile(
        runtime->remote_samples + warm_samples,
        atomic_load(&runtime->sample_count) - warm_samples, 100);
    metrics->copy_ns_p50 = obmm_uffd_percentile(
        runtime->copy_samples + warm_samples,
        atomic_load(&runtime->sample_count) - warm_samples, 50);
    metrics->copy_ns_p95 = obmm_uffd_percentile(
        runtime->copy_samples + warm_samples,
        atomic_load(&runtime->sample_count) - warm_samples, 95);
    metrics->copy_ns_p99 = obmm_uffd_percentile(
        runtime->copy_samples + warm_samples,
        atomic_load(&runtime->sample_count) - warm_samples, 99);
    metrics->copy_ns_max = obmm_uffd_percentile(
        runtime->copy_samples + warm_samples,
        atomic_load(&runtime->sample_count) - warm_samples, 100);
    metrics->wake_ns_p50 = obmm_uffd_percentile(
        runtime->wake_samples + warm_samples,
        atomic_load(&runtime->sample_count) - warm_samples, 50);
    metrics->wake_ns_p95 = obmm_uffd_percentile(
        runtime->wake_samples + warm_samples,
        atomic_load(&runtime->sample_count) - warm_samples, 95);
    metrics->wake_ns_p99 = obmm_uffd_percentile(
        runtime->wake_samples + warm_samples,
        atomic_load(&runtime->sample_count) - warm_samples, 99);
    metrics->wake_ns_max = obmm_uffd_percentile(
        runtime->wake_samples + warm_samples,
        atomic_load(&runtime->sample_count) - warm_samples, 100);
    ret = 0;

cleanup:
    if (handler_started) {
        atomic_store_explicit(&runtime->stop, true, memory_order_release);
        obmm_uffd_unregister(&runtime->uffd);
        obmm_uffd_close(&runtime->uffd);
        pthread_join(handler, NULL);
    } else if (runtime != MAP_FAILED) {
        obmm_uffd_close(&runtime->uffd);
    }
    if (attr_initialized) {
        pthread_attr_destroy(&handler_attr);
    }
    if (handler_mapping != MAP_FAILED) {
        munlock(handler_mapping + OBMM_UFFD_PAGE_BYTES,
                OBMM_UFFD_HANDLER_STACK_BYTES);
        munmap(handler_mapping, handler_mapping_bytes);
    }
    if (runtime != MAP_FAILED) {
        if (runtime->shadow && runtime->shadow != MAP_FAILED) {
            munmap(runtime->shadow, runtime->shadow_length);
        }
        munlock((void *)config->staging_buffer->data,
                OBMM_UFFD_PAGE_BYTES);
        if (runtime->records) {
            munlock(runtime->records,
                    config->pages * sizeof(*runtime->records));
        }
        if (runtime->logical_ordinals) {
            munlock(runtime->logical_ordinals,
                    config->pages * sizeof(*runtime->logical_ordinals));
        }
        if (runtime->fault_samples) {
            munlock(runtime->fault_samples,
                    runtime->sample_capacity *
                    sizeof(*runtime->fault_samples));
        }
        if (runtime->remote_samples) {
            munlock(runtime->remote_samples,
                    runtime->sample_capacity *
                    sizeof(*runtime->remote_samples));
        }
        if (runtime->copy_samples) {
            munlock(runtime->copy_samples,
                    runtime->sample_capacity *
                    sizeof(*runtime->copy_samples));
        }
        if (runtime->wake_samples) {
            munlock(runtime->wake_samples,
                    runtime->sample_capacity *
                    sizeof(*runtime->wake_samples));
        }
        if (runtime->remote_by_page) {
            munlock(runtime->remote_by_page,
                    config->pages * sizeof(*runtime->remote_by_page));
        }
        if (runtime->copy_by_page) {
            munlock(runtime->copy_by_page,
                    config->pages * sizeof(*runtime->copy_by_page));
        }
        free(runtime->copy_by_page);
        free(runtime->remote_by_page);
        free(runtime->wake_samples);
        free(runtime->copy_samples);
        free(runtime->remote_samples);
        free(runtime->fault_samples);
        free(runtime->logical_ordinals);
        free(runtime->records);
        if (sync_initialized) {
            pthread_cond_destroy(&runtime->ready_cond);
            pthread_mutex_destroy(&runtime->ready_lock);
        }
        munlock(runtime, sizeof(*runtime));
        munmap(runtime, sizeof(*runtime));
    }
    return ret;
}
