#ifndef MEM_SERVICE_DAEMON_H
#define MEM_SERVICE_DAEMON_H

#include <stdbool.h>
#include <stdint.h>

struct mem_service_remote_transport_probe_result {
    bool payload_block_round_trip;
    bool payload_checksum_validation;
    bool payload_corruption_fail_closed;
    uint64_t payload_len;
    uint64_t payload_checksum;
};

struct mem_service_daemon_limits {
    uint64_t max_records;
    uint64_t max_payload_bytes;
    uint64_t max_audit_events;
    uint64_t max_checkpoint_records;
    uint64_t max_retained_records;
    uint64_t max_retained_record_age_ms;
    uint32_t max_retained_record_kind;
    bool max_retained_record_tenant_enabled;
    uint32_t max_retained_record_tenant;
};

struct mem_service_provider_registry;

struct mem_service_daemon_runtime {
    const struct mem_service_daemon_limits *limits;
    const struct mem_service_provider_registry *providers;
};

int mem_service_run_unix_daemon(const char *listen_spec);
int mem_service_run_unix_daemon_with_store(const char *listen_spec, const char *store_path);
int mem_service_run_unix_daemon_with_store_and_metrics(const char *listen_spec,
                                                       const char *store_path,
                                                       const char *metrics_listen_spec);
int mem_service_run_unix_daemon_with_store_metrics_and_catalog(
    const char *listen_spec,
    const char *store_path,
    const char *metrics_listen_spec,
    const char *storage_root);
int mem_service_run_unix_daemon_with_store_metrics_catalog_and_limits(
    const char *listen_spec,
    const char *store_path,
    const char *metrics_listen_spec,
    const char *storage_root,
    const struct mem_service_daemon_limits *limits);
int mem_service_run_unix_daemon_with_runtime(
    const char *listen_spec,
    const char *store_path,
    const char *metrics_listen_spec,
    const char *storage_root,
    const struct mem_service_daemon_runtime *runtime);
int mem_service_run_wire_fixture_check(void);
int mem_service_run_store_fixture_check(void);
int mem_service_run_journal_fixture_check(void);
int mem_service_run_journal_torn_recovery_fixture_check(void);
int mem_service_run_journal_compaction_fixture_check(void);
int mem_service_run_durable_catalog_fixture_check(void);
int mem_service_run_chunked_block_fixture_check(void);
int mem_service_run_transport_block_fixture_check(void);
int mem_service_run_network_transport_block_fixture_check(void);
int mem_service_run_tcp_payload_fixture_source(const char *listen_spec,
                                               uint64_t payload_len);
int mem_service_run_runtime_quota_fixture_check(void);
int mem_service_run_retention_fixture_check(void);
int mem_service_run_checkpoint_retention_fixture_check(void);
int mem_service_run_payload_gc_fixture_check(void);
int mem_service_run_record_retention_fixture_check(void);
int mem_service_probe_transport_tcp_payload_block(
    const char *storage_root,
    const char *payload_source,
    struct mem_service_remote_transport_probe_result *result);
int mem_service_run_serving_fail_closed_fixture_check(void);
int mem_service_run_pretraining_fail_closed_fixture_check(void);
int mem_service_run_typed_payload_fixture_check(void);
int mem_service_run_restore_policy_fixture_check(void);
int mem_service_run_upgrade_rollback_runtime_fixture_check(void);
int mem_service_run_compat_runtime_fixture_check(void);
int mem_service_run_compat_old_server_runtime_fixture_check(void);

#endif
