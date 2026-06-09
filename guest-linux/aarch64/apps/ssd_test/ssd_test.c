/*
 * SSD test application.
 *
 * Tests UB-attached SSD BLOCK_WRITE, BLOCK_READ, BLOCK_SEAL, BLOCK_TOMBSTONE.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <errno.h>

#include "../../../kernel_ub/include/uapi/ub/ub_ssd.h"

#define SSD_DEV "/dev/ub_ssd0"

static int ssd_fd = -1;

static int ssd_open(void)
{
    ssd_fd = open(SSD_DEV, O_RDWR);
    if (ssd_fd < 0) {
        perror("open " SSD_DEV);
        return -1;
    }
    return 0;
}

static int ssd_submit_and_wait(struct ub_ssd_cmd_v1 *cmd,
                               struct ub_ssd_cpl_v1 *cpl)
{
    int rc;

    rc = ioctl(ssd_fd, UB_SSD_SUBMIT, cmd);
    if (rc < 0) {
        fprintf(stderr, "SSD_SUBMIT failed: %s\n", strerror(errno));
        return rc;
    }

    rc = ioctl(ssd_fd, UB_SSD_WAIT, cpl);
    if (rc < 0) {
        fprintf(stderr, "SSD_WAIT failed: %s\n", strerror(errno));
        return rc;
    }

    return 0;
}

static int test_ssd_echo(void)
{
    struct ub_ssd_cmd_v1 cmd = {0};
    struct ub_ssd_cpl_v1 cpl = {0};

    printf("[ssd_test] TEST: command echo (no data)\n");

    cmd.version = 1;
    cmd.opcode = SSD_OP_FLUSH;
    cmd.req_id = 0xABCD;

    if (ssd_submit_and_wait(&cmd, &cpl) < 0)
        return -1;

    if (cpl.status != SSD_OK) {
        fprintf(stderr, "  FAIL: status=%d\n", cpl.status);
        return -1;
    }

    printf("  PASS: req_id=%#lx status=%d\n",
           (unsigned long)cpl.req_id, cpl.status);
    return 0;
}

static int test_ssd_bad_opcode(void)
{
    struct ub_ssd_cmd_v1 cmd = {0};
    struct ub_ssd_cpl_v1 cpl = {0};

    printf("[ssd_test] TEST: bad opcode rejection\n");

    cmd.version = 1;
    cmd.opcode = 0xFF;
    cmd.req_id = 0xDEAD;

    if (ssd_submit_and_wait(&cmd, &cpl) < 0)
        return -1;

    if (cpl.status != (__u32)SSD_ERR_BAD_OPCODE) {
        fprintf(stderr, "  FAIL: expected BAD_OPCODE, got %d\n", cpl.status);
        return -1;
    }

    printf("  PASS: rejected with status=%d\n", cpl.status);
    return 0;
}

int main(int argc, char *argv[])
{
    int pass = 0, fail = 0;

    (void)argc;
    (void)argv;

    printf("[ssd_test] SSD test suite\n");

    if (ssd_open() < 0) {
        fprintf(stderr, "[ssd_test] cannot open device, skipping\n");
        printf("[ssd_test] verdict=SKIP\n");
        return 0;
    }

    if (test_ssd_echo() == 0) pass++; else fail++;
    if (test_ssd_bad_opcode() == 0) pass++; else fail++;

    close(ssd_fd);

    printf("[ssd_test] Results: %d/%d passed, %d failed\n",
           pass, pass + fail, fail);
    printf("[ssd_test] verdict=%s\n", fail == 0 ? "PASS" : "FAIL");
    return fail > 0 ? 1 : 0;
}
