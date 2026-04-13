# FM 动态 Entity 注入实现计划

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** 在双节点 UB 模拟互联中实现完整的动态实体注入机制，支持静态多实体枚举和运行期动态增删。

**架构:**
1. M0: Per-entity 配置空间切片 + 能力字段修复
2. M1: INI plan 加载 + FM diff apply + Pool 消息注入
3. M2: 统一日志 + 快速诊断

**Tech Stack:** QEMU 8.2.0, C, GLib, INI 配置解析, Guest Linux Kernel

---

## Task 1: 添加 Per-Entity CFG 空间结构

**Files:**
- Modify: `simulator/vendor/qemu_8.2.0_ub/include/hw/ub/ub_ubc.h`

**Step 1: 添加 Per-Entity CFG 空间结构定义**

在 `BusControllerDev` 结构体中添加 per-entity 配置空间支持：

```c
/* Per-Entity Configuration Space */
typedef struct UBEntityCfgSpace {
    uint8_t  *cfg_base;      /* 该实体的配置空间基址 */
    uint32_t cfg_size;       /* 配置空间大小 */
    uint32_t eid;            /* 实体 EID */
    uint32_t cna;            /* 实体 CNA */
    uint16_t upi;            /* 实体 UPI */
    bool     initialized;    /* 是否已初始化 */
} UBEntityCfgSpace;
```

在 `BusControllerDev` 结构体中添加字段：

```c
UBEntityCfgSpace entity_cfg_spaces[UB_MAX_ENTITIES]; /* per-entity cfg spaces */
```

**Step 2: Commit**

```bash
git add simulator/vendor/qemu_8.2.0_ub/include/hw/ub/ub_ubc.h
git commit -m "feat(ub): add per-entity cfg space structure"
```

---

## Task 2: 实现 Per-Entity CFG 空间初始化

**Files:**
- Modify: `simulator/vendor/qemu_8.2.0_ub/hw/ub/ub_ubc.c`

**Step 1: 实现 per-entity cfg 空间初始化函数**

在 `ub_entity_table_init` 之后添加新函数：

```c
void ub_entity_cfg_spaces_init(BusControllerDev *ubc_dev)
{
    uint32_t i;
    UBDevice *ub_dev = UB_DEVICE(ubc_dev);
    uint32_t base_cfg_size = ub_config_size();

    for (i = 0; i < ubc_dev->entity_count && i < UB_MAX_ENTITIES; i++) {
        UBEntityCfgSpace *space = &ubc_dev->entity_cfg_spaces[i];
        UBEntityDesc *e = &ubc_dev->entities[i];

        /* 分配独立配置空间 */
        space->cfg_base = g_malloc0(base_cfg_size);
        if (!space->cfg_base) {
            qemu_log("entity_cfg_spaces_init: failed to alloc for entity %u\n", i);
            continue;
        }

        /* 复制基础配置 */
        memcpy(space->cfg_base, ub_dev->config, base_cfg_size);

        /* 设置实体特定字段 */
        space->cfg_size = base_cfg_size;
        space->eid = e->eid[0];
        space->cna = e->cna;
        space->upi = e->upi;
        space->initialized = true;

        /* 修改该实体配置空间中的 EID/CNA/UPICNA */
        uint64_t emulated_offset;

        /* 修改 EID */
        emulated_offset = ub_cfg_offset_to_emulated_offset(UB_CFG0_EID_0_OFFSET, true);
        uint32_t *eid_ptr = (uint32_t *)(space->cfg_base + emulated_offset);
        *eid_ptr = cpu_to_le32(e->eid[0]);

        /* 修改 UPICNA */
        emulated_offset = ub_cfg_offset_to_emulated_offset(UB_CFG0_UPI_OFFSET, true);
        uint32_t *upi_cna_ptr = (uint32_t *)(space->cfg_base + emulated_offset);
        *upi_cna_ptr = cpu_to_le32((e->upi & 0x7FFF) | ((e->cna & 0xFF) << 16));

        /* 修改 FM CNA */
        emulated_offset = ub_cfg_offset_to_emulated_offset(UB_CFG0_FM_CNA_OFFSET, true);
        uint32_t *cna_ptr = (uint32_t *)(space->cfg_base + emulated_offset);
        *cna_ptr = cpu_to_le32(e->cna);

        qemu_log("entity_cfg_spaces_init: [%u] eid=%#x cna=%#x upi=%u cfg_size=%u\n",
                 i, space->eid, space->cna, space->upi, space->cfg_size);
    }

    qemu_log("entity_cfg_spaces_init: %u entity cfg spaces initialized\n",
             ubc_dev->entity_count);
}
```

**Step 2: 在 realize 中调用初始化**

修改 `ub_bus_controller_dev_realize` 函数，在 `ub_entity_table_init` 之后添加：

```c
ub_entity_table_init(ubc->ubc_dev);
ub_entity_cfg_spaces_init(ubc->ubc_dev);  /* 新增 */
```

**Step 3: 添加清理函数**

```c
void ub_entity_cfg_spaces_cleanup(BusControllerDev *ubc_dev)
{
    uint32_t i;

    for (i = 0; i < UB_MAX_ENTITIES; i++) {
        UBEntityCfgSpace *space = &ubc_dev->entity_cfg_spaces[i];
        if (space->initialized && space->cfg_base) {
            g_free(space->cfg_base);
            space->cfg_base = NULL;
            space->initialized = false;
        }
    }
}
```

**Step 4: Commit**

```bash
git add simulator/vendor/qemu_8.2.0_ub/hw/ub/ub_ubc.c
git commit -m "feat(ub): implement per-entity cfg space initialization"
```

---

## Task 3: 修改 ub_cfg_rw 支持 Per-Entity CFG 读取

**Files:**
- Modify: `simulator/vendor/qemu_8.2.0_ub/hw/ub/ub_config.c`

**Step 1: 修改 ub_cfg_rw 函数使用 per-entity cfg 空间**

将现有的临时放行逻辑替换为真正的 per-entity cfg 视图：

```c
/* Support multiple entities: select per-entity cfg view */
UBEntityCfgSpace *entity_cfg = NULL;
BusControllerDev *ubc_dev = s->ubc_dev;

if (entity_idx > 0) {
    if (entity_idx >= ubc_dev->entity_count || entity_idx >= UB_MAX_ENTITIES) {
        qemu_log("ub_cfg_rw: invalid entity_idx=%u (entity_count=%u)\n",
                 entity_idx, ubc_dev->entity_count);
        rsp_pkt.header.msgetah.rsp_status = UB_MSG_RSP_REG_ATTR_MISMATCH;
        goto fill_rq_cq;
    }

    entity_cfg = &ubc_dev->entity_cfg_spaces[entity_idx];
    if (!entity_cfg->initialized) {
        qemu_log("ub_cfg_rw: entity_idx=%u cfg space not initialized\n", entity_idx);
        rsp_pkt.header.msgetah.rsp_status = UB_MSG_RSP_REG_ATTR_MISMATCH;
        goto fill_rq_cq;
    }

    qemu_log("ub_cfg_rw: using per-entity cfg space for entity_idx=%u\n", entity_idx);
}
```

**Step 2: 修改配置空间读取逻辑**

在使用 `ub_dev->config` 的地方，根据 entity_idx 选择正确的 cfg_base：

```c
/* 选择配置空间 */
uint8_t *cfg_base = entity_cfg ? entity_cfg->cfg_base : ub_dev->config;

switch (header->msgetah.sub_msg_code) {
case UB_CFG0_READ:
case UB_CFG1_READ:
    if (cfg_offset < ub_config_size()) {
        uint32_t *cfg_ptr = (uint32_t *)(cfg_base + (cfg_offset / DWORD_SIZE * sizeof(uint32_t)));
        rsp_pkt.pld.rsp.read_data = *cfg_ptr;
    } else {
        rsp_pkt.header.msgetah.rsp_status = UB_MSG_RSP_INVALID_ADDR;
        goto fill_rq_cq;
    }
    /* ... rest of the logic ... */
```

**Step 3: Commit**

```bash
git add simulator/vendor/qemu_8.2.0_ub/hw/ub/ub_config.c
git commit -m "feat(ub): support per-entity cfg view in ub_cfg_rw"
```

---

## Task 4: 修复 UBC 能力字段

**Files:**
- Modify: `simulator/vendor/qemu_8.2.0_ub/hw/ub/ub_ubc.c`

**Step 1: 修复 total_num_of_ue 硬编码**

查找并修改 `ub_bus_controller_space_cfg0_init` 中的硬编码：

```c
/* 修改前 */
cfg0_basic->total_num_of_ue = 1;

/* 修改后 */
cfg0_basic->total_num_of_ue = ub_dev->ubc->ubc_dev->entity_count;
```

**Step 2: 修复 ue_cnt 硬编码**

在 `ubc_handle_post_mb` 的 `UBASE_OPC_QUERY_UE_RES` case 中：

```c
/* 修改前 */
ue.ue_cnt = cpu_to_le16(1);

/* 修改后 */
BusControllerDev *ubc_dev = s->ubc_dev;
ue.ue_cnt = cpu_to_le16(ubc_dev ? ubc_dev->entity_count : 1);
```

**Step 3: Commit**

```bash
git add simulator/vendor/qemu_8.2.0_ub/hw/ub/ub_ubc.c
git commit -m "fix(ub): use entity_count for all UE capability fields"
```

---

## Task 5: 统一 UB_OBTAIN_ENTITY_INFO 报文字段

**Files:**
- Modify: `simulator/vendor/qemu_8.2.0_ub/hw/ub/hisi/ubc_msgq.c`

**Step 1: 统一 entity_nums 和 map 字段**

修改 `ub_obtain_entity_info` 函数：

```c
static void ub_obtain_entity_info(BusControllerState *s, HiMsgSqe *sqe, MsgPktHeader *header)
{
    EntityInfoMsgPkt *rsp_pkt = NULL;
    uint32_t rsp_pkt_size;
    BusControllerDev *ubc_dev = s->ubc_dev;
    uint32_t entity_count = ubc_dev ? ubc_dev->entity_count : 1;

    rsp_pkt_size = sizeof(EntityInfoMsgPkt) + sizeof(struct UeMap);
    rsp_pkt = g_malloc0(rsp_pkt_size);
    memcpy(&rsp_pkt->header, header, sizeof(rsp_pkt->header));

    /* 统一字段语义 */
    rsp_pkt->pld.rsp.entity_nums = entity_count;
    rsp_pkt->pld.rsp.mue_nums = 1;  /* 始终为 1 个 MUE (entity_idx=0) */

    /* 设置 entity map: 连续范围 [0, entity_count-1] */
    rsp_pkt->pld.rsp.map[0].start_entity_idx = 0;
    rsp_pkt->pld.rsp.map[0].end_entity_idx = entity_count - 1;

    rsp_pkt->header.msgetah.rsp_status = UB_MSG_RSP_SUCCESS;

    /* 确保 plen 与实际 payload 一致 */
    rsp_pkt->header.msgetah.plen = ENTITY_INFO_BASE_PLD_SIZE + sizeof(struct UeMap);

    ub_obtain_entity_info_ms_fill_cq_rq(s, sqe, header, rsp_pkt);
    g_free(rsp_pkt);

    qemu_log("ub_obtain_entity_info: entity_count=%u, entity_nums=%u, mue_nums=%u, map=0..%u, plen=%u\n",
             entity_count, rsp_pkt->pld.rsp.entity_nums, rsp_pkt->pld.rsp.mue_nums,
             rsp_pkt->pld.rsp.map[0].end_entity_idx, rsp_pkt->header.msgetah.plen);
}
```

**Step 2: Commit**

```bash
git add simulator/vendor/qemu_8.2.0_ub/hw/ub/hisi/ubc_msgq.c
git commit -m "fix(ub): unify UB_OBTAIN_ENTITY_INFO response fields"
```

---

## Task 6: 添加 Pool 消息结构定义

**Files:**
- Create: `simulator/vendor/qemu_8.2.0_ub/include/hw/ub/ub_pool_msg.h`

**Step 1: 创建 Pool 消息头文件**

```c
#ifndef UB_POOL_MSG_H
#define UB_POOL_MSG_H

#include "hw/ub/ub_msg.h"
#include "hw/ub/ub_ubc.h"

/* Pool 消息码 */
#define UB_MSG_CODE_POOL  6

/* Pool 子消息码 */
#define UB_DEV_REG         0
#define UB_DEV_RLS         1
#define UB_BI_CREATE       2
#define UB_BI_DESTROY      3
#define UB_CFG_CPL_NOTIFY  4

/* entity_base_info 结构 (匹配 guest pool.h) */
typedef struct UBPoolEntityBaseInfo {
    /* DW0 */
    uint32_t entity_idx : 16;
    uint32_t upi        : 15;
    uint32_t rsvd0      : 1;
    /* DW1~DW4 */
    uint32_t eid[4];
    /* DW5~DW8 */
    uint32_t guid[UB_ENTITY_GUID_DW_NUM];
    /* DW9 */
    uint32_t cna        : 24;
    uint32_t rsvd2      : 8;
    /* DW10~DW13 */
    uint32_t ueid[4];
} UBPoolEntityBaseInfo;

/* entity_rs_info 结构 */
typedef struct UBPoolEntityRsInfo {
    uint32_t ss;    /* segment size */
    uint32_t sa_l;  /* start address low */
    uint32_t sa_h;  /* start address high */
} UBPoolEntityRsInfo;

/* entity_reg_msg_pld 结构 */
typedef struct UBPoolEntityRegMsg {
    UBPoolEntityBaseInfo base;
    UBPoolEntityRsInfo ers[UB_ENTITY_MAX_RES_NUM];
} UBPoolEntityRegMsg;

/* entity_rls_msg_pld 结构 */
typedef struct UBPoolEntityRlsMsg {
    uint32_t eid[4];
    uint32_t reason : 8;
    uint32_t rsvd1  : 24;
} UBPoolEntityRlsMsg;

#define UB_POOL_ENTITY_BASE_SIZE  56
#define UB_POOL_ENTITY_RS_SIZE    36
#define UB_POOL_ENTITY_REG_SIZE   (UB_POOL_ENTITY_BASE_SIZE + UB_POOL_ENTITY_RS_SIZE * UB_ENTITY_MAX_RES_NUM)
#define UB_POOL_ENTITY_RLS_SIZE   20

#endif /* UB_POOL_MSG_H */
```

**Step 2: Commit**

```bash
git add simulator/vendor/qemu_8.2.0_ub/include/hw/ub/ub_pool_msg.h
git commit -m "feat(ub): add pool message structure definitions"
```

---

## Task 7: 实现 UB_DEV_REG 消息注入

**Files:**
- Modify: `simulator/vendor/qemu_8.2.0_ub/hw/ub/ub_ubc.c`

**Step 1: 实现 ub_inject_entity_reg 函数**

```c
int ub_inject_entity_reg(BusControllerState *s, const UBEntityDesc *e, Error **errp)
{
    BusControllerDev *ubc_dev = s->ubc_dev;
    UBPoolEntityRegMsg *reg_msg;
    uint8_t *msg_buf;
    uint32_t msg_size;
    uint32_t pi;
    HiMsgCqe cqe;

    if (!s->msgq.rq_inited || !s->msgq.cq_inited) {
        error_setg(errp, "msgq not initialized");
        return -1;
    }

    if (!ubc_dev || !ubc_dev->parent.cluster) {
        error_setg(errp, "not in cluster mode");
        return -1;
    }

    msg_size = MSG_PKT_HEADER_SIZE + UB_POOL_ENTITY_REG_SIZE;
    msg_buf = g_malloc0(msg_size);

    /* 构造消息头 */
    MsgPktHeader *header = (MsgPktHeader *)msg_buf;
    header->msgetah.msg_code = UB_MSG_CODE_POOL;
    header->msgetah.sub_msg_code = UB_DEV_REG;
    header->msgetah.code = UB_MSG_RESPONSE;
    header->msgetah.rsp_status = UB_MSG_RSP_SUCCESS;
    header->msgetah.plen = UB_POOL_ENTITY_REG_SIZE;

    /* 构造 entity_base_info */
    reg_msg = (UBPoolEntityRegMsg *)(msg_buf + MSG_PKT_HEADER_SIZE);
    reg_msg->base.entity_idx = e->entity_idx;
    reg_msg->base.upi = e->upi;
    reg_msg->base.cna = e->cna;
    memcpy(reg_msg->base.eid, e->eid, sizeof(e->eid));
    memcpy(reg_msg->base.ueid, e->ueid, sizeof(e->ueid));
    memcpy(reg_msg->base.guid, e->guid, sizeof(e->guid));

    /* 构造 entity_rs_info */
    for (uint32_t i = 0; i < UB_ENTITY_MAX_RES_NUM; i++) {
        reg_msg->ers[i].ss = e->ers[i].ss;
        reg_msg->ers[i].sa_l = e->ers[i].sa_l;
        reg_msg->ers[i].sa_h = e->ers[i].sa_h;
    }

    /* 注入到 RQ */
    pi = fill_rq(s, msg_buf, msg_size);
    g_free(msg_buf);

    if (pi == UINT32_MAX) {
        error_setg(errp, "fill_rq failed");
        return -1;
    }

    /* 填充 CQE */
    memset(&cqe, 0, sizeof(cqe));
    cqe.status = CQE_SUCCESS;
    cqe.rq_pi = pi;
    cqe.p_len = msg_size;

    if (fill_cq(s, &cqe) == UINT32_MAX) {
        error_setg(errp, "fill_cq failed");
        return -1;
    }

    qemu_log("entity_reg inject: entity_idx=%u eid=%#x ueid=%#x device_id=%#x cna=%#x\n",
             e->entity_idx, e->eid[0], e->ueid[0], e->device_id, e->cna);

    return 0;
}
```

**Step 2: Commit**

```bash
git add simulator/vendor/qemu_8.2.0_ub/hw/ub/ub_ubc.c
git commit -m "feat(ub): implement UB_DEV_REG message injection"
```

---

## Task 8: 实现 UB_DEV_RLS 消息注入

**Files:**
- Modify: `simulator/vendor/qemu_8.2.0_ub/hw/ub/ub_ubc.c`

**Step 1: 实现 ub_inject_entity_rls 函数**

```c
int ub_inject_entity_rls(BusControllerState *s, uint32_t eid, uint8_t reason, Error **errp)
{
    BusControllerDev *ubc_dev = s->ubc_dev;
    UBPoolEntityRlsMsg *rls_msg;
    uint8_t *msg_buf;
    uint32_t msg_size;
    uint32_t pi;
    HiMsgCqe cqe;

    if (!s->msgq.rq_inited || !s->msgq.cq_inited) {
        error_setg(errp, "msgq not initialized");
        return -1;
    }

    if (!ubc_dev || !ubc_dev->parent.cluster) {
        error_setg(errp, "not in cluster mode");
        return -1;
    }

    msg_size = MSG_PKT_HEADER_SIZE + UB_POOL_ENTITY_RLS_SIZE;
    msg_buf = g_malloc0(msg_size);

    /* 构造消息头 */
    MsgPktHeader *header = (MsgPktHeader *)msg_buf;
    header->msgetah.msg_code = UB_MSG_CODE_POOL;
    header->msgetah.sub_msg_code = UB_DEV_RLS;
    header->msgetah.code = UB_MSG_RESPONSE;
    header->msgetah.rsp_status = UB_MSG_RSP_SUCCESS;
    header->msgetah.plen = UB_POOL_ENTITY_RLS_SIZE;

    /* 构造 entity_rls_msg_pld */
    rls_msg = (UBPoolEntityRlsMsg *)(msg_buf + MSG_PKT_HEADER_SIZE);
    rls_msg->eid[0] = eid;
    rls_msg->eid[1] = 0;
    rls_msg->eid[2] = 0;
    rls_msg->eid[3] = 0;
    rls_msg->reason = reason;
    rls_msg->rsvd1 = 0;

    /* 注入到 RQ */
    pi = fill_rq(s, msg_buf, msg_size);
    g_free(msg_buf);

    if (pi == UINT32_MAX) {
        error_setg(errp, "fill_rq failed");
        return -1;
    }

    /* 填充 CQE */
    memset(&cqe, 0, sizeof(cqe));
    cqe.status = CQE_SUCCESS;
    cqe.rq_pi = pi;
    cqe.p_len = msg_size;

    if (fill_cq(s, &cqe) == UINT32_MAX) {
        error_setg(errp, "fill_cq failed");
        return -1;
    }

    qemu_log("entity_rls inject: eid=%#x reason=%#x\n", eid, reason);

    return 0;
}
```

**Step 2: Commit**

```bash
git add simulator/vendor/qemu_8.2.0_ub/hw/ub/ub_ubc.c
git commit -m "feat(ub): implement UB_DEV_RLS message injection"
```

---

## Task 9: 添加 Entity Plan FM API

**Files:**
- Modify: `simulator/vendor/qemu_8.2.0_ub/include/hw/ub/hisi/ub_fm.h`
- Modify: `simulator/vendor/qemu_8.2.0_ub/hw/ub/hisi/ub_fm.c`

**Step 1: 添加 Entity Plan 结构和 API 声明**

在 `ub_fm.h` 中添加：

```c
/* Entity Plan Management */
typedef struct UBFMEntityPlanEntry {
    uint32_t     entity_idx;
    uint32_t     device_id;
    uint32_t     eid[4];
    uint32_t     ueid[4];
    uint32_t     cna;
    uint32_t     upi;
    uint32_t     guid[4];
    UBEntityState state;
} UBFMEntityPlanEntry;

typedef struct UBFMEntityPlan {
    GPtrArray *entities;
    char *source_name;
    time_t last_modified;
} UBFMEntityPlan;

int ub_fm_load_entity_plan_from_file(const char *path, Error **errp);
int ub_fm_apply_entity_plan(Error **errp);
void ub_fm_entity_plan_free(UBFMEntityPlan *plan);
```

**Step 2: 实现 Entity Plan 加载函数**

在 `ub_fm.c` 中实现：

```c
static UBFMEntityPlan *ub_fm_current_entity_plan = NULL;

int ub_fm_load_entity_plan_from_file(const char *path, Error **errp)
{
    GKeyFile *keyfile;
    GError *gerr = NULL;
    gchar **groups;
    gsize num_groups;
    UBFMEntityPlan *plan;
    int ret = 0;

    keyfile = g_key_file_new();
    if (!g_key_file_load_from_file(keyfile, path, G_KEY_FILE_NONE, &gerr)) {
        error_setg(errp, "failed to load entity plan from %s: %s",
                   path, gerr ? gerr->message : "unknown error");
        g_clear_error(&gerr);
        g_key_file_free(keyfile);
        return -1;
    }

    plan = g_new0(UBFMEntityPlan, 1);
    plan->entities = g_ptr_array_new_with_free_func(g_free);
    plan->source_name = g_strdup(path);

    groups = g_key_file_get_groups(keyfile, &num_groups);
    for (gsize i = 0; i < num_groups; i++) {
        if (!g_str_has_prefix(groups[i], "entity ")) {
            continue;
        }

        UBFMEntityPlanEntry *entry = g_new0(UBFMEntityPlanEntry, 1);

        entry->entity_idx = g_key_file_get_uint64(keyfile, groups[i],
                                                   "entity_idx", &gerr);
        entry->device_id = g_key_file_get_uint64(keyfile, groups[i],
                                                   "device_id", NULL);
        entry->cna = g_key_file_get_uint64(keyfile, groups[i], "cna", NULL);
        entry->upi = g_key_file_get_uint64(keyfile, groups[i], "upi", NULL);

        gchar *eid_str = g_key_file_get_string(keyfile, groups[i], "eid", NULL);
        if (eid_str) {
            entry->eid[0] = strtol(eid_str, NULL, 0);
            g_free(eid_str);
        }

        gchar *ueid_str = g_key_file_get_string(keyfile, groups[i], "ueid", NULL);
        if (ueid_str) {
            entry->ueid[0] = strtol(ueid_str, NULL, 0);
            g_free(ueid_str);
        }

        entry->guid[0] = g_key_file_get_uint64(keyfile, groups[i],
                                                "guid_vendor", NULL);
        entry->guid[1] = 0;
        entry->guid[2] = g_key_file_get_uint64(keyfile, groups[i],
                                                 "guid_device", NULL);
        entry->guid[3] = g_key_file_get_uint64(keyfile, groups[i],
                                                 "guid_vendor", NULL);

        gchar *state_str = g_key_file_get_string(keyfile, groups[i],
                                                  "state", NULL);
        if (g_strcmp0(state_str, "present") == 0) {
            entry->state = UB_ENTITY_STATE_PRESENT;
        } else if (g_strcmp0(state_str, "absent") == 0) {
            entry->state = UB_ENTITY_STATE_ABSENT;
        } else {
            entry->state = UB_ENTITY_STATE_ERROR;
        }
        g_free(state_str);

        g_ptr_array_add(plan->entities, entry);

        qemu_log("entity_plan: loaded entity %u: state=%s device_id=%#x eid=%#x\n",
                 entry->entity_idx,
                 entry->state == UB_ENTITY_STATE_PRESENT ? "present" : "absent",
                 entry->device_id, entry->eid[0]);
    }

    g_strfreev(groups);
    g_key_file_free(keyfile);

    if (ub_fm_current_entity_plan) {
        ub_fm_entity_plan_free(ub_fm_current_entity_plan);
    }
    ub_fm_current_entity_plan = plan;

    qemu_log("entity_plan: loaded %u entities from %s\n",
             plan->entities->len, path);

    return 0;
}

void ub_fm_entity_plan_free(UBFMEntityPlan *plan)
{
    if (!plan) {
        return;
    }

    if (plan->entities) {
        g_ptr_array_unref(plan->entities);
    }
    g_free(plan->source_name);
    g_free(plan);
}
```

**Step 3: Commit**

```bash
git add simulator/vendor/qemu_8.2.0_ub/include/hw/ub/hisi/ub_fm.h
git add simulator/vendor/qemu_8.2.0_ub/hw/ub/hisi/ub_fm.c
git commit -m "feat(ub): add entity plan file loading support"
```

---

## Task 10: 实现 Entity Plan Diff 和 Apply

**Files:**
- Modify: `simulator/vendor/qemu_8.2.0_ub/hw/ub/hisi/ub_fm.c`

**Step 1: 实现 entity plan diff 和 apply 函数**

```c
int ub_fm_apply_entity_plan(Error **errp)
{
    BusControllerState *s = ub_fm_get_bus_controller();
    BusControllerDev *ubc_dev;
    UBFMEntityPlan *plan;
    int ret = 0;

    if (!s || !s->ubc_dev) {
        error_setg(errp, "no bus controller");
        return -1;
    }

    ubc_dev = s->ubc_dev;
    plan = ub_fm_current_entity_plan;

    if (!plan) {
        qemu_log("entity_plan: no plan to apply\n");
        return 0;
    }

    /* Diff: 期望 present 且 当前 absent -> 注入 UB_DEV_REG */
    for (gsize i = 0; i < plan->entities->len; i++) {
        UBFMEntityPlanEntry *desired = g_ptr_array_index(plan->entities, i);
        UBEntityDesc *current = ub_entity_desc_for_idx(ubc_dev, desired->entity_idx);

        if (desired->state == UB_ENTITY_STATE_PRESENT) {
            if (!current || current->state == UB_ENTITY_STATE_ABSENT) {
                /* 需要添加实体 */
                UBEntityDesc new_entity = {0};
                new_entity.entity_idx = desired->entity_idx;
                new_entity.device_id = desired->device_id;
                new_entity.cna = desired->cna;
                new_entity.upi = desired->upi;
                new_entity.state = UB_ENTITY_STATE_PRESENT;
                memcpy(new_entity.eid, desired->eid, sizeof(desired->eid));
                memcpy(new_entity.ueid, desired->ueid, sizeof(desired->ueid));
                memcpy(new_entity.guid, desired->guid, sizeof(desired->guid));

                /* 初始化 ERS */
                new_entity.ers[0].ss = UBC_ERS0_SPACE_SIZE;
                new_entity.ers[0].sa_l = UBC_ERS0_SPACE_ADDR + desired->entity_idx * 0x200000;
                new_entity.ers[1].ss = UBC_ERS1_SPACE_SIZE;
                new_entity.ers[1].sa_l = UBC_ERS1_SPACE_ADDR + desired->entity_idx * 0x200000;
                new_entity.ers[2].ss = UBC_ERS2_SPACE_SIZE;
                new_entity.ers[2].sa_l = UBC_ERS2_SPACE_ADDR + desired->entity_idx * 0x200000;

                ret = ub_inject_entity_reg(s, &new_entity, errp);
                if (ret) {
                    qemu_log("entity_plan: failed to inject entity_reg for idx=%u\n",
                             desired->entity_idx);
                    /* 标记为错误状态，继续处理其他实体 */
                    if (current) {
                        current->state = UB_ENTITY_STATE_ERROR;
                    }
                } else {
                    qemu_log("entity_plan: injected entity_reg for idx=%u\n",
                             desired->entity_idx);
                    /* 更新当前状态 */
                    if (current) {
                        current->state = UB_ENTITY_STATE_PRESENT;
                    }
                }
            }
        } else if (desired->state == UB_ENTITY_STATE_ABSENT) {
            if (current && current->state == UB_ENTITY_STATE_PRESENT) {
                /* 需要删除实体 */
                ret = ub_inject_entity_rls(s, current->eid[0], 0, errp);
                if (ret) {
                    qemu_log("entity_plan: failed to inject entity_rls for eid=%#x\n",
                             current->eid[0]);
                } else {
                    qemu_log("entity_plan: injected entity_rls for eid=%#x\n",
                             current->eid[0]);
                    current->state = UB_ENTITY_STATE_ABSENT;
                }
            }
        }
    }

    qemu_log("entity_plan: apply completed\n");
    return 0;
}
```

**Step 2: 添加获取 BusController 的辅助函数**

```c
static BusControllerState *ub_fm_get_bus_controller(void)
{
    /* 通过 QOM 查找第一个 UB bus controller */
    Object *container = object_get_objects_root();
    BusControllerState *s = NULL;
    Object *obj;

    object_child_foreach(container, find_ubc, &s);
    return s;
}

static int find_ubc(Object *obj, void *opaque)
{
    BusControllerState **s_ptr = (BusControllerState **)opaque;
    if (object_dynamic_cast(obj, TYPE_BUS_CONTROLLER_DEV)) {
        BusControllerDev *ubc_dev = BUS_CONTROLLER_DEV(obj);
        if (ubc_dev && ubc_dev->parent.bus) {
            *s_ptr = container_of_ubbus(UB_BUS(qdev_get_parent_bus(DEVICE(ubc_dev))));
            return 1;  /* 停止遍历 */
        }
    }
    return 0;
}
```

**Step 3: Commit**

```bash
git add simulator/vendor/qemu_8.2.0_ub/hw/ub/hisi/ub_fm.c
git commit -m "feat(ub): implement entity plan diff and apply"
```

---

## Task 11: 添加启动时 Entity Plan 加载

**Files:**
- Modify: `simulator/vendor/qemu_8.2.0_ub/hw/arm/virt.c`

**Step 1: 在启动时加载 entity plan**

在 `ub_sim_entity_count` 设置之后添加：

```c
/* Load entity plan from file if specified */
{
    const char *entity_plan_path = g_getenv("UB_FM_ENTITY_PLAN_FILE");
    if (entity_plan_path && entity_plan_path[0]) {
        Error *local_err = NULL;
        ret = ub_fm_load_entity_plan_from_file(entity_plan_path, &local_err);
        if (ret) {
            qemu_log("UB: failed to load entity plan from %s: %s\n",
                     entity_plan_path,
                     local_err ? error_get_pretty(local_err) : "unknown error");
            error_free(local_err);
        } else {
            qemu_log("UB: loaded entity plan from %s\n", entity_plan_path);
            /* Apply plan after bus is ready */
            /* Note: actual apply will happen later in realize */
        }
    }
}
```

**Step 2: 在 realize 后应用 entity plan**

在 UBC realize 之后添加 apply 调用：

```c
/* Apply entity plan after UBC realize */
{
    const char *entity_plan_path = g_getenv("UB_FM_ENTITY_PLAN_FILE");
    if (entity_plan_path && entity_plan_path[0]) {
        Error *local_err = NULL;
        ret = ub_fm_apply_entity_plan(&local_err);
        if (ret) {
            qemu_log("UB: failed to apply entity plan: %s\n",
                     local_err ? error_get_pretty(local_err) : "unknown error");
            error_free(local_err);
        }
    }
}
```

**Step 3: Commit**

```bash
git add simulator/vendor/qemu_8.2.0_ub/hw/arm/virt.c
git commit -m "feat(ub): load and apply entity plan at startup"
```

---

## Task 12: 添加 QEMU Monitor 命令支持

**Files:**
- Modify: `simulator/vendor/qemu_8.2.0_ub/hw/ub/hisi/ub_fm.c`

**Step 1: 添加 monitor 命令处理函数**

```c
/* QEMU Monitor command handlers */
void qmp_ub_fm_reload_entity_plan(const char *path, Error **errp)
{
    int ret;

    ret = ub_fm_load_entity_plan_from_file(path, errp);
    if (ret) {
        error_setg(errp, "failed to load entity plan from %s", path);
        return;
    }

    ret = ub_fm_apply_entity_plan(errp);
    if (ret) {
        error_setg(errp, "failed to apply entity plan");
        return;
    }

    qemu_log("entity_plan: reloaded and applied from %s\n", path);
}

void qmp_ub_fm_show_entity_plan(Error **errp)
{
    UBFMEntityPlan *plan = ub_fm_current_entity_plan;

    if (!plan) {
        error_setg(errp, "no entity plan loaded");
        return;
    }

    printf("Entity Plan (source: %s):\n", plan->source_name);
    for (gsize i = 0; i < plan->entities->len; i++) {
        UBFMEntityPlanEntry *entry = g_ptr_array_index(plan->entities, i);
        printf("  [%u] state=%s device_id=%#x eid=%#x cna=%#x upi=%u\n",
               entry->entity_idx,
               entry->state == UB_ENTITY_STATE_PRESENT ? "present" :
               entry->state == UB_ENTITY_STATE_ABSENT ? "absent" : "error",
               entry->device_id, entry->eid[0], entry->cna, entry->upi);
    }
}
```

**Step 2: Commit**

```bash
git add simulator/vendor/qemu_8.2.0_ub/hw/ub/hisi/ub_fm.c
git commit -m "feat(ub): add QMP commands for entity plan management"
```

---

## Task 13: 统一日志关键字

**Files:**
- Modify: `simulator/vendor/qemu_8.2.0_ub/hw/ub/ub_ubc.c`
- Modify: `simulator/vendor/qemu_8.2.0_ub/hw/ub/hisi/ub_fm.c`

**Step 1: 统一日志前缀**

确保所有 entity 相关日志使用统一前缀：

```c
/* entity_reg 注入日志 */
qemu_log("entity_reg inject: entity_idx=%u eid=%#x ueid=%#x device_id=%#x cna=%#x status=%s\n",
         e->entity_idx, e->eid[0], e->ueid[0], e->device_id, e->cna,
         status == UB_MSG_RSP_SUCCESS ? "success" : "failed");

/* entity_rls 注入日志 */
qemu_log("entity_rls inject: eid=%#x reason=%#x status=%s\n",
         eid, reason,
         status == UB_MSG_RSP_SUCCESS ? "success" : "failed");

/* entity_state 变更日志 */
qemu_log("entity_state: idx=%u old=%s new=%s\n",
         entity_idx,
         old_state == UB_ENTITY_STATE_PRESENT ? "present" : "absent",
         new_state == UB_ENTITY_STATE_PRESENT ? "present" : "absent");
```

**Step 2: Commit**

```bash
git add simulator/vendor/qemu_8.2.0_ub/hw/ub/ub_ubc.c
git add simulator/vendor/qemu_8.2.0_ub/hw/ub/hisi/ub_fm.c
git commit -m "feat(ub): unify entity-related log prefixes"
```

---

## Task 14: 添加失败诊断日志

**Files:**
- Modify: `simulator/vendor/qemu_8.2.0_ub/hw/ub/ub_ubc.c`

**Step 1: 在注入失败时打印诊断信息**

```c
int ub_inject_entity_reg(BusControllerState *s, const UBEntityDesc *e, Error **errp)
{
    /* ... existing code ... */

    if (pi == UINT32_MAX) {
        qemu_log("entity_reg inject FAILED: entity_idx=%u eid=%#x device_id=%#x "
                 "reason=fill_rq_failed msgq_inited=%d cluster=%d\n",
                 e->entity_idx, e->eid[0], e->device_id,
                 s->msgq.rq_inited, ubc_dev->parent.cluster);
        error_setg(errp, "fill_rq failed");
        return -1;
    }

    if (fill_cq(s, &cqe) == UINT32_MAX) {
        qemu_log("entity_reg inject FAILED: entity_idx=%u eid=%#x device_id=%#x "
                 "reason=fill_cq_failed rq_pi=%u\n",
                 e->entity_idx, e->eid[0], e->device_id, pi);
        error_setg(errp, "fill_cq failed");
        return -1;
    }

    qemu_log("entity_reg inject SUCCESS: entity_idx=%u eid=%#x ueid=%#x device_id=%#x cna=%#x\n",
             e->entity_idx, e->eid[0], e->ueid[0], e->device_id, e->cna);

    return 0;
}
```

**Step 2: Commit**

```bash
git add simulator/vendor/qemu_8.2.0_ub/hw/ub/ub_ubc.c
git commit -m "feat(ub): add diagnostic logging for injection failures"
```

---

## Task 15: 创建示例 Entity Plan INI 文件

**Files:**
- Create: `simulator/vendor/ub_topology_two_node_v2_entity.ini`

**Step 1: 创建示例配置文件**

```ini
# UB Entity Plan Configuration
# 用于双节点场景的实体配置

[entity 0]
# FE0/MUE (主实体，始终存在)
state=present
entity_idx=0
device_id=0x0541
eid=0x10000
ueid=0x10000
cna=0x200
upi=1
guid_vendor=0xcc08
guid_device=0x0541
guid_type=0x2
guid_seq=0x1

[entity 1]
# FE1/UE (从实体，可动态增删)
state=present
entity_idx=1
device_id=0x0542
eid=0x10001
ueid=0x10001
cna=0x201
upi=1
guid_vendor=0xcc08
guid_device=0x0542
guid_type=0x2
guid_seq=0x2
```

**Step 2: Commit**

```bash
git add simulator/vendor/ub_topology_two_node_v2_entity.ini
git commit -m "docs(ub): add example entity plan configuration"
```

---

## Task 16: 创建 M0 静态枚举测试脚本

**Files:**
- Create: `simulator/guest-linux/aarch64/run_ub_entity_m0_test.sh`

**Step 1: 创建 M0 测试脚本**

```bash
#!/bin/bash
# M0: 静态多实体枚举测试
# 验证 UB_SIM_ENTITY_COUNT=2 时 FE1 可见

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "${SCRIPT_DIR}/test_env.sh"

echo "=== M0: Static Multi-Entity Enumeration Test ==="

# 设置双实体模式
export UB_SIM_ENTITY_COUNT=2

# 启动双节点 QEMU
start_dual_node_qemu

# 等待建链
wait_for_linkup 60

# 检查实体数量
check_entity_count() {
    local node=$1
    local expected=$2

    echo "Checking entity count on ${node}..."
    local count=$(ssh_guest ${node} "grep 'entity_nums=' /var/log/kern.log | tail -1" || echo "0")

    if [[ ! "${count}" =~ "entity_nums=${expected}" ]]; then
        echo "FAIL: Expected ${expected} entities on ${node}, got: ${count}"
        return 1
    fi

    echo "PASS: ${expected} entities found on ${node}"
    return 0
}

# 验证 FE1 设备可见
check_fe1_visible() {
    local node=$1

    echo "Checking FE1 visibility on ${node}..."
    local fe1_found=$(ssh_guest ${node} "grep -i '0x0542' /var/log/kern.log | grep -i 'probe'" || echo "")

    if [ -z "${fe1_found}" ]; then
        echo "FAIL: FE1 (0x0542) not found on ${node}"
        return 1
    fi

    echo "PASS: FE1 found on ${node}"
    return 0
}

# 执行检查
check_entity_count "node0" 2
check_entity_count "node1" 2
check_fe1_visible "node0"
check_fe1_visible "node1"

echo "=== M0 Test PASSED ==="

stop_qemu
exit 0
```

**Step 2: Commit**

```bash
git add simulator/guest-linux/aarch64/run_ub_entity_m0_test.sh
git commit -m "test(ub): add M0 static enumeration test script"
```

---

## Task 17: 创建 M1 动态注入测试脚本

**Files:**
- Create: `simulator/guest-linux/aarch64/run_ub_entity_m1_test.sh`

**Step 1: 创建 M1 测试脚本**

```bash
#!/bin/bash
# M1: 运行期动态注入测试
# 验证运行期 add/remove entity

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "${SCRIPT_DIR}/test_env.sh"

echo "=== M1: Dynamic Entity Injection Test ==="

# 准备测试 plan 文件
PLAN_ADD="/tmp/entity_plan_add.ini"
PLAN_REMOVE="/tmp/entity_plan_remove.ini"

cat > "${PLAN_ADD}" <<'EOF'
[entity 0]
state=present
entity_idx=0
device_id=0x0541
eid=0x10000
ueid=0x10000
cna=0x200
upi=1
guid_vendor=0xcc08
guid_device=0x0541
guid_type=0x2
guid_seq=0x1

[entity 1]
state=present
entity_idx=1
device_id=0x0542
eid=0x10001
ueid=0x10001
cna=0x201
upi=1
guid_vendor=0xcc08
guid_device=0x0542
guid_type=0x2
guid_seq=0x2

[entity 2]
state=present
entity_idx=2
device_id=0x0542
eid=0x10002
ueid=0x10002
cna=0x202
upi=1
guid_vendor=0xcc08
guid_device=0x0542
guid_type=0x2
guid_seq=0x3
EOF

cat > "${PLAN_REMOVE}" <<'EOF'
[entity 0]
state=present
entity_idx=0
device_id=0x0541
eid=0x10000
ueid=0x10000
cna=0x200
upi=1
guid_vendor=0xcc08
guid_device=0x0541
guid_type=0x2
guid_seq=0x1

[entity 1]
state=present
entity_idx=1
device_id=0x0542
eid=0x10001
ueid=0x10001
cna=0x201
upi=1
guid_vendor=0xcc08
guid_device=0x0542
guid_type=0x2
guid_seq=0x2

[entity 2]
state=absent
entity_idx=2
device_id=0x0542
eid=0x10002
ueid=0x10002
cna=0x202
upi=1
guid_vendor=0xcc08
guid_device=0x0542
guid_type=0x2
guid_seq=0x3
EOF

# 启动双节点 QEMU
export UB_SIM_ENTITY_COUNT=2
start_dual_node_qemu
wait_for_linkup 60

# 测试添加实体
echo "Testing entity add..."
qmp_node0 "ub-fm-reload-entity-plan" "${PLAN_ADD}"
sleep 5

check_entity_added() {
    local node=$1
    local entity_idx=$2

    echo "Checking entity ${entity_idx} on ${node}..."
    local found=$(ssh_guest ${node} "grep 'entity_idx=${entity_idx}' /var/log/kern.log | tail -1" || echo "")

    if [ -z "${found}" ]; then
        echo "FAIL: Entity ${entity_idx} not found on ${node}"
        return 1
    fi

    echo "PASS: Entity ${entity_idx} found on ${node}"
    return 0
}

check_entity_added "node0" 2

# 测试删除实体
echo "Testing entity remove..."
qmp_node0 "ub-fm-reload-entity-plan" "${PLAN_REMOVE}"
sleep 5

check_entity_removed() {
    local node=$1
    local eid=$2

    echo "Checking entity removal (eid=${eid}) on ${node}..."
    local released=$(ssh_guest ${node} "grep 'entity_rls.*eid=${eid}' /var/log/kern.log | tail -1" || echo "")

    if [ -z "${released}" ]; then
        echo "FAIL: Entity ${eid} not released on ${node}"
        return 1
    fi

    echo "PASS: Entity ${eid} released on ${node}"
    return 0
}

check_entity_removed "node0" 0x10002

echo "=== M1 Test PASSED ==="

stop_qemu
rm -f "${PLAN_ADD}" "${PLAN_REMOVE}"
exit 0
```

**Step 2: Commit**

```bash
git add simulator/guest-linux/aarch64/run_ub_entity_m1_test.sh
git commit -m "test(ub): add M1 dynamic injection test script"
```

---

## Task 18: 更新现有 E2E 测试脚本

**Files:**
- Modify: `simulator/guest-linux/aarch64/run_ub_dual_node_ubcore_urma_e2e.sh`

**Step 1: 添加实体就绪快速检查**

在脚本中添加实体检查函数：

```bash
# 快速检查实体是否就绪
check_entity_ready() {
    local node=$1
    local timeout_sec=${2:-30}

    echo "Checking entity readiness on ${node} (timeout ${timeout_sec}s)..."

    local elapsed=0
    while [ ${elapsed} -lt ${timeout_sec} ]; do
        local ready=$(ssh_guest ${node} "grep 'entity_reg inject SUCCESS' /var/log/kern.log | wc -l" || echo "0")
        if [ "${ready}" -ge 2 ]; then
            echo "PASS: Entities ready on ${node} (${ready} entities)"
            return 0
        fi
        sleep 1
        elapsed=$((elapsed + 1))
    done

    echo "FAIL: Entities not ready on ${node} after ${timeout_sec}s"
    return 1
}

# 在 wait_for_linkup 之后调用
wait_for_linkup 60
check_entity_ready "node0" 30
check_entity_ready "node1" 30
```

**Step 2: 添加 entity plan 支持**

```bash
# 支持通过环境变量指定 entity plan
if [ -n "${UB_FM_ENTITY_PLAN_FILE}" ]; then
    echo "Using entity plan: ${UB_FM_ENTITY_PLAN_FILE}"
    if [ ! -f "${UB_FM_ENTITY_PLAN_FILE}" ]; then
        echo "WARN: Entity plan file not found: ${UB_FM_ENTITY_PLAN_FILE}"
    fi
fi
```

**Step 3: Commit**

```bash
git add simulator/guest-linux/aarch64/run_ub_dual_node_ubcore_urma_e2e.sh
git commit -m "test(ub): add entity readiness check to e2e test"
```

---

## Task 19: 创建 M2 可观测性测试

**Files:**
- Create: `simulator/guest-linux/aarch64/run_ub_entity_m2_test.sh`

**Step 1: 创建 M2 测试脚本**

```bash
#!/bin/bash
# M2: 可观测性和日志测试
# 验证统一日志关键字和失败诊断

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "${SCRIPT_DIR}/test_env.sh"

echo "=== M2: Observability and Logging Test ==="

export UB_SIM_ENTITY_COUNT=2
start_dual_node_qemu
wait_for_linkup 60

# 检查日志关键字
check_log_keywords() {
    local node=$1
    local log_file=$2

    echo "Checking log keywords on ${node}..."

    local keywords=(
        "entity_reg inject"
        "entity_rls inject"
        "entity_state"
        "entity_nums"
    )

    for kw in "${keywords[@]}"; do
        local found=$(ssh_guest ${node} "grep '${kw}' ${log_file} | wc -l" || echo "0")
        if [ "${found}" -eq 0 ]; then
            echo "WARN: Keyword '${kw}' not found in logs"
        else
            echo "  Found '${kw}': ${found} occurrences"
        fi
    done
}

# 检查失败诊断
check_failure_diagnostics() {
    local node=$1

    echo "Checking failure diagnostic format..."

    # 触发一个错误场景（例如无效 entity_idx）
    local error_found=$(ssh_guest ${node} "grep 'entity_reg inject FAILED' ${log_file}" || echo "")

    if [ -n "${error_found}" ]; then
        # 验证诊断字段
        if [[ "${error_found}" =~ entity_idx= ]] && \
           [[ "${error_found}" =~ eid= ]] && \
           [[ "${error_found}" =~ reason= ]]; then
            echo "PASS: Failure diagnostic format correct"
        else
            echo "FAIL: Failure diagnostic format incomplete"
            return 1
        fi
    else
        echo "INFO: No injection failures found (expected in successful run)"
    fi
}

check_log_keywords "node0" "/var/log/kern.log"
check_log_keywords "node1" "/var/log/kern.log"
check_failure_diagnostics "node0"

echo "=== M2 Test PASSED ==="

stop_qemu
exit 0
```

**Step 2: Commit**

```bash
git add simulator/guest-linux/aarch64/run_ub_entity_m2_test.sh
git commit -m "test(ub): add M2 observability test script"
```

---

## Task 20: 综合回归测试

**Files:**
- Create: `simulator/guest-linux/aarch64/run_ub_entity_full_regression.sh`

**Step 1: 创建综合回归测试脚本**

```bash
#!/bin/bash
# 综合回归测试 - 执行 M0/M1/M2 所有测试

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

echo "=== FM Dynamic Entity Injection - Full Regression ==="

# 运行所有测试
tests=(
    "run_ub_entity_m0_test.sh"
    "run_ub_entity_m1_test.sh"
    "run_ub_entity_m2_test.sh"
)

failed=()
passed=()

for test in "${tests[@]}"; do
    echo ""
    echo "Running ${test}..."
    if "${SCRIPT_DIR}/${test}"; then
        passed+=("${test}")
    else
        failed+=("${test}")
    fi
done

# 汇总结果
echo ""
echo "=== Test Summary ==="
echo "PASSED: ${#passed[@]}"
for t in "${passed[@]}"; do
    echo "  ✓ ${t}"
done

if [ ${#failed[@]} -gt 0 ]; then
    echo "FAILED: ${#failed[@]}"
    for t in "${failed[@]}"; do
        echo "  ✗ ${t}"
    done
    exit 1
fi

echo "All tests PASSED!"
exit 0
```

**Step 2: Commit**

```bash
git add simulator/guest-linux/aarch64/run_ub_entity_full_regression.sh
git commit -m "test(ub): add full regression test script"
```

---

## 执行顺序

按照以下顺序执行上述任务：

1. Task 1-5: M0 静态多实体闭环
2. Task 6-11: M1 运行期动态注入
3. Task 12-14: M2 可观测性
4. Task 15-20: 测试用例

---

## 验收标准

完成后应满足以下条件：

1. **M0**: `UB_SIM_ENTITY_COUNT=2` 时 FE1 可见，ubase probe 成功
2. **M1**: 运行期 entity plan 变更可触发 guest entity add/remove
3. **M2**: 日志关键字统一，失败可快速定位
4. **测试**: 所有测试脚本通过

---

## 回滚开关

如需回滚，设置以下环境变量：
- `UB_SIM_ENTITY_COUNT=1` - 回到单实体模式
- `UB_FM_ENTITY_PLAN_FILE=` - 禁用 entity plan
