#ifndef MEM_SERVICE_DAEMON_H
#define MEM_SERVICE_DAEMON_H

int mem_service_run_unix_daemon(const char *listen_spec);
int mem_service_run_unix_daemon_with_store(const char *listen_spec, const char *store_path);
int mem_service_run_unix_daemon_with_store_and_metrics(const char *listen_spec,
                                                       const char *store_path,
                                                       const char *metrics_listen_spec);
int mem_service_run_wire_fixture_check(void);
int mem_service_run_store_fixture_check(void);
int mem_service_run_journal_fixture_check(void);

#endif
