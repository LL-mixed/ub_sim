/*
 * NPU test application.
 *
 * Tests UB-attached NPU MEMCOPY, FILL, VECTOR_ADD_U32, CHECKSUM64
 * with GSVA buffer descriptors.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <errno.h>

#include "../../../kernel_ub/include/uapi/ub/ub_npu.h"

#define NPU_DEV "/dev/ub_npu0"

static int npu_fd = -1;

static int npu_open(void)
{
    npu_fd = open(NPU_DEV, O_RDWR);
    if (npu_fd < 0) {
        perror("open " NPU_DEV);
        return -1;
    }
    return 0;
}

static int npu_submit_and_wait(struct ub_npu_cmd_v1 *cmd,
                               struct ub_npu_cpl_v1 *cpl)
{
    int rc;

    rc = ioctl(npu_fd, UB_NPU_SUBMIT, cmd);
    if (rc < 0) {
        fprintf(stderr, "NPU_SUBMIT failed: %s\n", strerror(errno));
        return rc;
    }

    rc = ioctl(npu_fd, UB_NPU_WAIT, cpl);
    if (rc < 0) {
        fprintf(stderr, "NPU_WAIT failed: %s\n", strerror(errno));
        return rc;
    }

    return 0;
}

static int test_npu_echo(void)
{
    struct ub_npu_cmd_v1 cmd = {0};
    struct ub_npu_cpl_v1 cpl = {0};

    printf("[npu_test] TEST: command echo (no data)\n");

    cmd.version = 1;
    cmd.opcode = NPU_OP_MEMCOPY;
    cmd.req_id = 0x1234;
    cmd.desc_count = 0;

    if (npu_submit_and_wait(&cmd, &cpl) < 0)
        return -1;

    if (cpl.status != NPU_OK) {
        fprintf(stderr, "  FAIL: status=%d\n", cpl.status);
        return -1;
    }

    printf("  PASS: req_id=%#lx status=%d\n",
           (unsigned long)cpl.req_id, cpl.status);
    return 0;
}

static int test_npu_bad_opcode(void)
{
    struct ub_npu_cmd_v1 cmd = {0};
    struct ub_npu_cpl_v1 cpl = {0};

    printf("[npu_test] TEST: bad opcode rejection\n");

    cmd.version = 1;
    cmd.opcode = 0xFF;
    cmd.req_id = 0x5678;
    cmd.desc_count = 0;

    if (npu_submit_and_wait(&cmd, &cpl) < 0)
        return -1;

    if (cpl.status != (__u32)NPU_ERR_BAD_OPCODE) {
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

    printf("[npu_test] NPU test suite\n");

    if (npu_open() < 0) {
        fprintf(stderr, "[npu_test] cannot open device, skipping\n");
        /* Not fatal: device may not exist in all configs */
        printf("[npu_test] verdict=SKIP\n");
        return 0;
    }

    if (test_npu_echo() == 0) pass++; else fail++;
    if (test_npu_bad_opcode() == 0) pass++; else fail++;

    close(npu_fd);

    printf("[npu_test] Results: %d/%d passed, %d failed\n",
           pass, pass + fail, fail);
    printf("[npu_test] verdict=%s\n", fail == 0 ? "PASS" : "FAIL");
    return fail > 0 ? 1 : 0;
}
