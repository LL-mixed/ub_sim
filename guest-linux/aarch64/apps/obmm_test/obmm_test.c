/* SPDX-License-Identifier: GPL-2.0 */
/*
 * linqu_obmm_test: run the upstream obmm-test framework (gitcode
 * obmm-ecology/obmm-test) inside the guest.
 *
 * Every node brings up the ipourma datapath and starts obmm-test-conductor.
 * The first node in LINQU_UB_ALL_IPS additionally waits for all peer
 * conductors, writes the orchestrator config, runs obmm-test-orchestrator in
 * the foreground, and propagates its exit status.
 *
 * Environment (exported by run_app from kernel cmdline):
 *   LINQU_UB_LOCAL_IP / LINQU_UB_ALL_IPS  node addressing
 *   OBMM_TEST_PORT                        conductor port (default 9981)
 *   OBMM_TEST_INCLUDE / OBMM_TEST_EXCLUDE / OBMM_TEST_GROUP_INCLUDE /
 *   OBMM_TEST_GROUP_EXCLUDE               test filters (optional)
 *   OBMM_TEST_TIMEOUT_SCALE               orchestrator timeout scale (optional)
 *
 * Serial markers grepped by scripts/run_ub_dual_node_obmm_test.sh:
 *   [obmm_test] conductor ready
 *   [obmm_test] orchestrator start peers=<n> config=...
 *   [obmm_test] orchestrator done rc=<n>
 *   [obmm_test] pass / [obmm_test] fail
 */

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "obmm_common.h"

#define OBMM_TEST_TAG "[obmm_test] "
#define OBMM_TEST_DEFAULT_PORT 9981
#define OBMM_TEST_PEER_WAIT_MS 120000
#define OBMM_TEST_CONDUCTOR_BIN "/bin/obmm-test-conductor"
#define OBMM_TEST_ORCHESTRATOR_BIN "/bin/obmm-test-orchestrator"
#define OBMM_TEST_CONFIG_PATH "/tmp/obmm_test.ini"

static char g_peer_ips[OBMM_POOL_HELPERS_MAX_NODES][16];
static int g_peer_count;

static const char *env_value(const char *name)
{
    const char *value = getenv(name);
    if (value && value[0] != '\0')
        return value;
    return NULL;
}

static int split_ips(const char *csv)
{
    char buf[256];
    char *saveptr = NULL;
    char *tok;
    int count = 0;

    snprintf(buf, sizeof(buf), "%s", csv);
    tok = strtok_r(buf, ",", &saveptr);
    while (tok != NULL && count < OBMM_POOL_HELPERS_MAX_NODES) {
        snprintf(g_peer_ips[count], sizeof(g_peer_ips[count]), "%s", tok);
        count++;
        tok = strtok_r(NULL, ",", &saveptr);
    }
    return count;
}

static void bring_up_loopback(void)
{
    struct ifreq ifr;
    int fd;

    fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0)
        return;
    memset(&ifr, 0, sizeof(ifr));
    snprintf(ifr.ifr_name, sizeof(ifr.ifr_name), "lo");
    if (ioctl(fd, SIOCGIFFLAGS, &ifr) == 0) {
        ifr.ifr_flags |= IFF_UP | IFF_RUNNING;
        ioctl(fd, SIOCSIFFLAGS, &ifr);
    }
    close(fd);
}

static int check_listening(int port)
{
    /* Non-invasive readiness check: a bare connect()+close() makes the
     * conductor's accept() read EOF and exit, so inspect the listen table
     * instead of touching the socket. */
    FILE *fp = fopen("/proc/net/tcp", "r");
    char line[256];
    char want_any[16];
    char want_lo[16];
    int found = 0;

    if (!fp)
        return 0;
    snprintf(want_any, sizeof(want_any), "00000000:%04X", (unsigned)port);
    snprintf(want_lo, sizeof(want_lo), "0100007F:%04X", (unsigned)port);
    while (fgets(line, sizeof(line), fp) != NULL) {
        if ((strstr(line, want_any) || strstr(line, want_lo)) &&
            strstr(line, " 0A ")) {
            found = 1;
            break;
        }
    }
    fclose(fp);
    return found;
}

static int wait_for_peers(int port, const char *local_ip)
{
    /* The peer conductor's listen state is not visible from this node and a
     * TCP probe would kill it (see check_listening), so wait for the local
     * conductor and then give peers a fixed grace period. */
    const char *grace_env = env_value("OBMM_TEST_PEER_GRACE_SECS");
    long grace_ms = grace_env ? (atol(grace_env) * 1000) : (OBMM_TEST_PEER_WAIT_MS / 6);
    long listen_deadline = obmm_now_ms() + OBMM_TEST_PEER_WAIT_MS;
    long grace_deadline;
    int i;

    (void)local_ip;
    while (!check_listening(port)) {
        if (obmm_now_ms() >= listen_deadline) {
            fprintf(stderr, OBMM_TEST_TAG "FAIL: local conductor not listening port=%d\n", port);
            return -1;
        }
        usleep(200000);
    }
    grace_deadline = obmm_now_ms() + grace_ms;
    while (obmm_now_ms() < grace_deadline)
        usleep(200000);
    for (i = 0; i < g_peer_count; i++) {
        if (strcmp(g_peer_ips[i], local_ip) != 0)
            printf(OBMM_TEST_TAG "peer grace done ip=%s port=%d\n", g_peer_ips[i], port);
    }
    fflush(stdout);
    return 0;
}

static pid_t start_conductor(int port)
{
    char port_arg[16];
    pid_t pid;

    pid = fork();
    if (pid < 0) {
        fprintf(stderr, OBMM_TEST_TAG "FAIL: fork conductor: %s\n", strerror(errno));
        return -1;
    }
    if (pid == 0) {
        signal(SIGINT, SIG_DFL);
        signal(SIGTERM, SIG_DFL);
        snprintf(port_arg, sizeof(port_arg), "%d", port);
        execl(OBMM_TEST_CONDUCTOR_BIN, OBMM_TEST_CONDUCTOR_BIN,
              "--port", port_arg, (char *)NULL);
        fprintf(stderr, OBMM_TEST_TAG "FAIL: exec %s: %s\n",
                OBMM_TEST_CONDUCTOR_BIN, strerror(errno));
        _exit(127);
    }
    return pid;
}

static int write_config(int port, const char *local_ip)
{
    FILE *fp = fopen(OBMM_TEST_CONFIG_PATH, "w");
    int i;

    if (!fp) {
        fprintf(stderr, OBMM_TEST_TAG "FAIL: write %s: %s\n",
                OBMM_TEST_CONFIG_PATH, strerror(errno));
        return -1;
    }
    fprintf(fp, "[hosts]\n");
    for (i = 0; i < g_peer_count; i++) {
        /* The orchestrator reaches its own conductor over loopback. */
        if (strcmp(g_peer_ips[i], local_ip) == 0)
            fprintf(fp, "host%d = 127.0.0.1:%d\n", i, port);
        else
            fprintf(fp, "host%d = %s:%d\n", i, g_peer_ips[i], port);
    }
    fprintf(fp, "\n[tests]\n");
    if (env_value("OBMM_TEST_GROUP_EXCLUDE"))
        fprintf(fp, "group_exclude = %s\n", env_value("OBMM_TEST_GROUP_EXCLUDE"));
    if (env_value("OBMM_TEST_GROUP_INCLUDE"))
        fprintf(fp, "group_include = %s\n", env_value("OBMM_TEST_GROUP_INCLUDE"));
    if (env_value("OBMM_TEST_INCLUDE"))
        fprintf(fp, "include = %s\n", env_value("OBMM_TEST_INCLUDE"));
    if (env_value("OBMM_TEST_EXCLUDE"))
        fprintf(fp, "exclude = %s\n", env_value("OBMM_TEST_EXCLUDE"));
    fprintf(fp, "\n[timeout]\n");
    fprintf(fp, "scale = %s\n",
            env_value("OBMM_TEST_TIMEOUT_SCALE") ? env_value("OBMM_TEST_TIMEOUT_SCALE") : "5.0");
    fclose(fp);
    (void)local_ip;
    return 0;
}

static int run_orchestrator(int port, const char *local_ip)
{
    pid_t pid;
    int status = 0;

    if (wait_for_peers(port, local_ip) != 0)
        return 1;
    if (write_config(port, local_ip) != 0)
        return 1;
    printf(OBMM_TEST_TAG "orchestrator start peers=%d config=%s\n",
           g_peer_count, OBMM_TEST_CONFIG_PATH);
    fflush(stdout);

    pid = fork();
    if (pid < 0)
        return 1;
    if (pid == 0) {
        signal(SIGINT, SIG_DFL);
        signal(SIGTERM, SIG_DFL);
        execl(OBMM_TEST_ORCHESTRATOR_BIN, OBMM_TEST_ORCHESTRATOR_BIN,
              "--config", OBMM_TEST_CONFIG_PATH,
              "--verbose",
              "--results", "/tmp/obmm-test-results.csv",
              (char *)NULL);
        fprintf(stderr, OBMM_TEST_TAG "FAIL: exec %s: %s\n",
                OBMM_TEST_ORCHESTRATOR_BIN, strerror(errno));
        _exit(127);
    }
    if (waitpid(pid, &status, 0) < 0) {
        fprintf(stderr, OBMM_TEST_TAG "FAIL: waitpid orchestrator: %s\n", strerror(errno));
        return 1;
    }
    if (WIFEXITED(status)) {
        printf(OBMM_TEST_TAG "orchestrator done rc=%d\n", WEXITSTATUS(status));
        fflush(stdout);
        return WEXITSTATUS(status);
    }
    printf(OBMM_TEST_TAG "orchestrator done signal=%d\n", WTERMSIG(status));
    fflush(stdout);
    return 1;
}

int main(void)
{
    const char *local_ip = env_value("LINQU_UB_LOCAL_IP");
    const char *all_ips = env_value("LINQU_UB_ALL_IPS");
    const char *port_env = env_value("OBMM_TEST_PORT");
    char ifname[64];
    unsigned int ifindex = 0;
    struct in_addr peer_addr;
    int port = OBMM_TEST_DEFAULT_PORT;
    pid_t conductor_pid;
    int is_orchestrator;
    int rc = 0;
    int status = 0;
    int i;

    if (!local_ip)
        local_ip = "10.0.0.1";
    if (!all_ips)
        all_ips = "10.0.0.1,10.0.0.2";
    if (port_env)
        port = atoi(port_env);
    if (port <= 0 || port > 65535)
        port = OBMM_TEST_DEFAULT_PORT;

    g_peer_count = split_ips(all_ips);
    if (g_peer_count < 2) {
        fprintf(stderr, OBMM_TEST_TAG "FAIL: need at least two nodes in LINQU_UB_ALL_IPS\n");
        return 1;
    }
    is_orchestrator = (strcmp(local_ip, g_peer_ips[0]) == 0);
    bring_up_loopback();

    if (!obmm_wait_iface(ifname, sizeof(ifname), &ifindex)) {
        fprintf(stderr, OBMM_TEST_TAG "FAIL: ipourma interface did not appear\n");
        return 1;
    }
    if (!obmm_set_ipv4(ifname, local_ip)) {
        fprintf(stderr, OBMM_TEST_TAG "FAIL: set ipv4 %s on %s\n", local_ip, ifname);
        return 1;
    }
    for (i = 0; i < g_peer_count; i++) {
        if (strcmp(g_peer_ips[i], local_ip) == 0)
            continue;
        if (inet_pton(AF_INET, g_peer_ips[i], &peer_addr) == 1)
            obmm_install_arp(ifname, &peer_addr);
    }
    printf(OBMM_TEST_TAG "datapath ready iface=%s local_ip=%s peers=%d\n",
           ifname, local_ip, g_peer_count);
    fflush(stdout);

    conductor_pid = start_conductor(port);
    if (conductor_pid < 0)
        return 1;
    printf(OBMM_TEST_TAG "conductor ready port=%d pid=%d\n", port, conductor_pid);
    fflush(stdout);

    if (is_orchestrator) {
        rc = run_orchestrator(port, local_ip);
        kill(conductor_pid, SIGTERM);
        waitpid(conductor_pid, &status, 0);
    } else {
        if (waitpid(conductor_pid, &status, 0) < 0)
            rc = 1;
        printf(OBMM_TEST_TAG "conductor finished\n");
    }

    if (rc == 0) {
        printf(OBMM_TEST_TAG "pass\n");
    } else {
        printf(OBMM_TEST_TAG "fail rc=%d\n", rc);
    }
    fflush(stdout);
    return rc;
}
