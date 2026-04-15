/*
 * User-space compatible header for URMA ioctl commands.
 *
 * Source: simulator/guest-linux/kernel_ub/drivers/ub/urma/uburma/uburma_cmd.h
 *         simulator/guest-linux/kernel_ub/include/ub/urma/ubcore_types.h
 *
 * This header copies struct definitions from the kernel's uburma_cmd.h
 * for use in user-space demo applications. All struct layouts (field order,
 * types, padding) must remain identical to the kernel versions.
 *
 * Kernel types are mapped as follows:
 *   __u32 -> uint32_t, __u64 -> uint64_t, __u16 -> uint16_t,
 *   __u8  -> uint8_t,  __s32 -> int32_t,  bool -> int (or _Bool)
 */

#ifndef UBURMA_CMD_USER_COMPAT_H
#define UBURMA_CMD_USER_COMPAT_H

#include <stdint.h>
#include <assert.h>

/* ---------- Constants from ubcore_types.h ---------- */

#ifndef UBCORE_EID_SIZE
#define UBCORE_EID_SIZE 16
#endif

#ifndef UBCORE_MAX_DEV_NAME
#define UBCORE_MAX_DEV_NAME 64
#endif

#ifndef UBCORE_MAX_PRIORITY_CNT
#define UBCORE_MAX_PRIORITY_CNT 16
#endif

#ifndef UBCORE_MAX_EID_CNT
#define UBCORE_MAX_EID_CNT 1024
#endif

#ifndef UBCORE_JETTY_GRP_MAX_NAME
#define UBCORE_JETTY_GRP_MAX_NAME 64
#endif

#define UBURMA_CMD_MAX_PORT_CNT 8

/* ---------- Enum values from ubcore_types.h ---------- */

enum ubcore_transport_mode_user {
    UBCORE_TP_RM_USER = 0x1,
    UBCORE_TP_RC_USER = 0x1 << 1,
    UBCORE_TP_UM_USER = 0x1 << 2,
    /* Keep legacy alias used by demo code. */
    UBCORE_TRANSPORT_UB_USER = UBCORE_TP_RM_USER,
};

enum ubcore_target_type_user {
    UBCORE_JETTY_USER = 1,
};

enum ubcore_jetty_grp_policy_user {
    UBCORE_JETTY_GRP_POLICY_RR_USER = 0,
};

enum ubcore_tp_type_user {
    UBCORE_RTP_USER = 0,
    UBCORE_CTP_USER,
    UBCORE_UTP_USER,
};

union ubcore_eid_user {
    uint8_t raw[UBCORE_EID_SIZE];
    struct {
        uint64_t reserved;
        uint32_t prefix;
        uint32_t addr;
    } in4;
    struct {
        uint64_t subnet_prefix;
        uint64_t interface_id;
    } in6;
};

struct ubcore_eid_info_user {
    union ubcore_eid_user eid;
    uint32_t eid_index;
};

/* ---------- Feature / flag unions from ubcore_types.h ---------- */

union ubcore_device_feat_user {
    struct {
        uint32_t oor : 1;
        uint32_t jfc_per_wr : 1;
        uint32_t stride_op : 1;
        uint32_t load_store_op : 1;
        uint32_t non_pin : 1;
        uint32_t pmem : 1;
        uint32_t jfc_inline : 1;
        uint32_t spray_en : 1;
        uint32_t selective_retrans : 1;
        uint32_t live_migrate : 1;
        uint32_t dca : 1;
        uint32_t jetty_grp : 1;
        uint32_t err_suspend : 1;
        uint32_t outorder_comp : 1;
        uint32_t mn : 1;
        uint32_t clan : 1;
        uint32_t muti_seg_per_token_id : 1;
        uint32_t ipourma_en : 1;
        uint32_t ctp_en : 1;
        uint32_t uboe : 1;
        uint32_t reserved : 12;
    } bs;
    uint32_t value;
};

union ubcore_atomic_feat_user {
    struct {
        uint32_t cas : 1;
        uint32_t swap : 1;
        uint32_t fetch_and_add : 1;
        uint32_t fetch_and_sub : 1;
        uint32_t fetch_and_and : 1;
        uint32_t fetch_and_or : 1;
        uint32_t fetch_and_xor : 1;
        uint32_t reserved : 25;
    } bs;
    uint32_t value;
};

union ubcore_order_type_cap_user {
    struct {
        uint32_t ot : 1;
        uint32_t oi : 1;
        uint32_t ol : 1;
        uint32_t no : 1;
        uint32_t reserved : 28;
    } bs;
    uint32_t value;
};

union urma_tp_type_cap_user {
    struct {
        uint32_t rtp : 1;
        uint32_t ctp : 1;
        uint32_t utp : 1;
        uint32_t reserved : 29;
    } bs;
    uint32_t value;
};

union urma_tp_feature_user {
    struct {
        uint32_t rm_multi_path : 1;
        uint32_t rc_multi_path : 1;
        uint32_t reserved : 30;
    } bs;
    uint32_t value;
};

struct ubcore_sl_info_user {
    uint32_t SL;
    union urma_tp_type_cap_user tp_type;
};

/* ---------- ioctl command definitions ---------- */

struct uburma_cmd_hdr {
    uint32_t command;
    uint32_t args_len;
    uint64_t args_addr;
};

#define UBURMA_CMD_MAGIC 'U'
#define UBURMA_CMD _IOWR(UBURMA_CMD_MAGIC, 1, struct uburma_cmd_hdr)

/* ---------- Command enum values ---------- */

enum uburma_cmd_user {
    UBURMA_CMD_CREATE_CTX = 1,
    UBURMA_CMD_ALLOC_TOKEN_ID,
    UBURMA_CMD_FREE_TOKEN_ID,
    UBURMA_CMD_REGISTER_SEG,
    UBURMA_CMD_UNREGISTER_SEG,
    UBURMA_CMD_IMPORT_SEG,
    UBURMA_CMD_UNIMPORT_SEG,
    UBURMA_CMD_CREATE_JFS,
    UBURMA_CMD_MODIFY_JFS,
    UBURMA_CMD_QUERY_JFS,
    UBURMA_CMD_DELETE_JFS,
    UBURMA_CMD_CREATE_JFR,
    UBURMA_CMD_MODIFY_JFR,
    UBURMA_CMD_QUERY_JFR,
    UBURMA_CMD_DELETE_JFR,
    UBURMA_CMD_CREATE_JFC,
    UBURMA_CMD_MODIFY_JFC,
    UBURMA_CMD_DELETE_JFC,
    UBURMA_CMD_CREATE_JFCE,
    UBURMA_CMD_IMPORT_JFR,
    UBURMA_CMD_UNIMPORT_JFR,
    UBURMA_CMD_CREATE_JETTY,
    UBURMA_CMD_MODIFY_JETTY,
    UBURMA_CMD_QUERY_JETTY,
    UBURMA_CMD_DELETE_JETTY,
    UBURMA_CMD_IMPORT_JETTY,
    UBURMA_CMD_UNIMPORT_JETTY,
    UBURMA_CMD_ADVISE_JFR,
    UBURMA_CMD_UNADVISE_JFR,
    UBURMA_CMD_ADVISE_JETTY,
    UBURMA_CMD_UNADVISE_JETTY,
    UBURMA_CMD_BIND_JETTY,
    UBURMA_CMD_UNBIND_JETTY,
    UBURMA_CMD_CREATE_JETTY_GRP,
    UBURMA_CMD_DESTROY_JETTY_GRP,
    UBURMA_CMD_USER_CTL,
    UBURMA_CMD_GET_EID_LIST,
    UBURMA_CMD_GET_NETADDR_LIST,
    UBURMA_CMD_MODIFY_TP,
    UBURMA_CMD_QUERY_DEV_ATTR,
    UBURMA_CMD_IMPORT_JETTY_ASYNC,
    UBURMA_CMD_UNIMPORT_JETTY_ASYNC,
    UBURMA_CMD_BIND_JETTY_ASYNC,
    UBURMA_CMD_UNBIND_JETTY_ASYNC,
    UBURMA_CMD_CREATE_NOTIFIER,
    UBURMA_CMD_GET_TP_LIST,
    UBURMA_CMD_IMPORT_JETTY_EX,
    UBURMA_CMD_IMPORT_JFR_EX,
    UBURMA_CMD_BIND_JETTY_EX,
    UBURMA_CMD_DELETE_JFS_BATCH,
    UBURMA_CMD_DELETE_JFR_BATCH,
    UBURMA_CMD_DELETE_JFC_BATCH,
    UBURMA_CMD_DELETE_JETTY_BATCH,
    UBURMA_CMD_SET_TP_ATTR,
    UBURMA_CMD_GET_TP_ATTR,
    UBURMA_CMD_EXCHANGE_TP_INFO,
    UBURMA_CMD_GET_EID_BY_IP,
    UBURMA_CMD_GET_IP_BY_EID,
    UBURMA_CMD_GET_SMAC,
    UBURMA_CMD_GET_DMAC,
    UBURMA_CMD_ALLOC_JFC,
    UBURMA_CMD_FREE_JFC,
    UBURMA_CMD_SET_JFC_OPT,
    UBURMA_CMD_GET_JFC_OPT,
    UBURMA_CMD_ACTIVE_JFC,
    UBURMA_CMD_DEACTIVE_JFC,
    UBURMA_CMD_ALLOC_JFR,
    UBURMA_CMD_FREE_JFR,
    UBURMA_CMD_SET_JFR_OPT,
    UBURMA_CMD_GET_JFR_OPT,
    UBURMA_CMD_ACTIVE_JFR,
    UBURMA_CMD_DEACTIVE_JFR,
    UBURMA_CMD_ALLOC_JFS,
    UBURMA_CMD_FREE_JFS,
    UBURMA_CMD_SET_JFS_OPT,
    UBURMA_CMD_GET_JFS_OPT,
    UBURMA_CMD_ACTIVE_JFS,
    UBURMA_CMD_DEACTIVE_JFS,
    UBURMA_CMD_ALLOC_JETTY,
    UBURMA_CMD_FREE_JETTY,
    UBURMA_CMD_SET_JETTY_OPT,
    UBURMA_CMD_GET_JETTY_OPT,
    UBURMA_CMD_ACTIVE_JETTY,
    UBURMA_CMD_DEACTIVE_JETTY,
    UBURMA_CMD_MAX
};

/* ---------- Shared structs ---------- */

struct uburma_cmd_udrv_priv {
    uint64_t in_addr;
    uint32_t in_len;
    uint64_t out_addr;
    uint32_t out_len;
};

union uburma_cmd_token_id_flag {
    struct {
        uint32_t multi_seg : 1;
        uint32_t reserved : 31;
    } bs;
    uint32_t value;
};

/* ---------- alloc/free token_id ---------- */

struct uburma_cmd_alloc_token_id {
    struct {
        uint32_t token_id;
        uint64_t handle;
    } out;
    struct uburma_cmd_udrv_priv udata;
    union uburma_cmd_token_id_flag flag;
};

struct uburma_cmd_free_token_id {
    struct {
        uint64_t handle;
        uint32_t token_id;
    } in;
};

/* ---------- create_ctx ---------- */

struct uburma_cmd_create_ctx {
    struct {
        uint8_t eid[UBCORE_EID_SIZE];
        uint32_t eid_index;
    } in;
    struct {
        int32_t async_fd;
    } out;
    struct uburma_cmd_udrv_priv udata;
};

/* ---------- alloc_jfc / active_jfc ---------- */

struct uburma_cmd_alloc_jfc {
    struct {
        uint32_t depth;
        uint32_t flag;
        int32_t jfce_fd;
        uint64_t urma_jfc;
        uint32_t ceqn;
    } in;
    struct {
        uint32_t id;
        uint32_t depth;
        uint64_t handle;
    } out;
    struct uburma_cmd_udrv_priv udata;
};

struct uburma_cmd_active_jfc {
    struct {
        uint64_t handle;
        uint32_t depth;
        uint32_t flag;
        uint32_t ceqn;
        uint64_t urma_jfc_opt;
    } in;
    struct {
        uint32_t id;
        uint32_t depth;
        uint64_t handle;
    } out;
    struct uburma_cmd_udrv_priv udata;
};

/* ---------- alloc_jfr / active_jfr ---------- */

struct uburma_cmd_alloc_jfr {
    struct {
        uint32_t depth;
        uint32_t flag;
        uint32_t trans_mode;
        uint8_t max_sge;
        uint8_t min_rnr_timer;
        uint32_t jfc_id;
        uint64_t jfc_handle;
        uint32_t token;
        uint32_t id;
        uint64_t urma_jfr;
    } in;
    struct {
        uint32_t id;
        uint32_t depth;
        uint64_t handle;
        uint8_t max_sge;
    } out;
    struct uburma_cmd_udrv_priv udata;
};

struct uburma_cmd_active_jfr {
    struct {
        uint64_t handle;
        uint32_t depth;
        uint32_t flag;
        uint32_t trans_mode;
        uint8_t max_sge;
        uint8_t min_rnr_timer;
        uint32_t jfc_id;
        uint64_t jfc_handle;
        uint32_t token_value;
        uint64_t urma_jfr_opt;
    } in;
    struct {
        uint32_t id;
        uint32_t depth;
        uint64_t handle;
        uint8_t max_sge;
    } out;
    struct uburma_cmd_udrv_priv udata;
};

/* ---------- alloc_jfs / active_jfs ---------- */

struct uburma_cmd_alloc_jfs {
    struct {
        uint32_t depth;
        uint32_t flag;
        uint32_t trans_mode;
        uint8_t priority;
        uint8_t max_sge;
        uint8_t max_rsge;
        uint32_t max_inline_data;
        uint8_t rnr_retry;
        uint8_t err_timeout;
        uint32_t jfc_id;
        uint64_t jfc_handle;
        uint64_t urma_jfs;
    } in;
    struct {
        uint32_t id;
        uint32_t depth;
        uint8_t max_sge;
        uint8_t max_rsge;
        uint32_t max_inline_data;
        uint64_t handle;
    } out;
    struct uburma_cmd_udrv_priv udata;
};

struct uburma_cmd_active_jfs {
    struct {
        uint64_t handle;
        uint32_t depth;
        uint32_t flag;
        uint32_t trans_mode;
        uint32_t priority;
        uint8_t max_sge;
        uint8_t max_rsge;
        uint32_t max_inline_data;
        uint8_t rnr_retry;
        uint8_t err_timeout;
        uint32_t jfc_id;
        uint64_t jfc_handle;
        uint64_t jfs_opt;
    } in;
    struct {
        uint32_t id;
        uint32_t depth;
        uint8_t max_sge;
        uint8_t max_rsge;
        uint32_t max_inline_data;
        uint64_t handle;
    } out;
    struct uburma_cmd_udrv_priv udata;
};

/* ---------- create_jetty (used by alloc_jetty which wraps it) ---------- */

struct uburma_cmd_create_jetty {
    struct {
        uint32_t id;
        uint32_t jetty_flag;
        uint32_t jfs_depth;
        uint32_t jfs_flag;
        uint32_t trans_mode;
        uint8_t priority;
        uint8_t max_send_sge;
        uint8_t max_send_rsge;
        uint32_t max_inline_data;
        uint8_t rnr_retry;
        uint8_t err_timeout;
        uint32_t send_jfc_id;
        uint64_t send_jfc_handle;
        uint32_t jfr_depth;
        uint32_t jfr_flag;
        uint8_t max_recv_sge;
        uint8_t min_rnr_timer;
        uint32_t recv_jfc_id;
        uint64_t recv_jfc_handle;
        uint32_t token;
        uint32_t jfr_id;
        uint64_t jfr_handle;
        uint64_t jetty_grp_handle;
        uint8_t is_jetty_grp;
        uint64_t urma_jetty;
    } in;
    struct {
        uint32_t id;
        uint64_t handle;
        uint32_t jfs_depth;
        uint32_t jfr_depth;
        uint8_t max_send_sge;
        uint8_t max_send_rsge;
        uint8_t max_recv_sge;
        uint32_t max_inline_data;
    } out;
    struct uburma_cmd_udrv_priv udata;
};

struct uburma_cmd_alloc_jetty {
    struct uburma_cmd_create_jetty create_jetty;
};

struct uburma_cmd_active_jetty {
    struct {
        uint32_t flag;
        uint64_t handle;
        uint64_t send_jfc_handle;
        uint64_t recv_jfc_handle;
        uint64_t urma_jetty;
        uint64_t jetty_opt;
    } in;
    struct {
        uint32_t jetty_id;
    } out;
    struct uburma_cmd_udrv_priv udata;
};

/* ---------- register_seg ---------- */

struct uburma_cmd_register_seg {
    struct {
        uint64_t va;
        uint64_t len;
        uint32_t token_id;
        uint64_t token_id_handle;
        uint32_t token;
        uint32_t flag;
    } in;
    struct {
        uint32_t token_id;
        uint64_t handle;
    } out;
    struct uburma_cmd_udrv_priv udata;
};

struct uburma_cmd_unregister_seg {
    struct {
        uint64_t handle;
    } in;
};

/* ---------- get_eid_list ---------- */

struct uburma_cmd_get_eid_list {
    struct {
        uint32_t max_eid_cnt;
    } in;
    struct {
        uint32_t eid_cnt;
        struct ubcore_eid_info_user eid_list[UBCORE_MAX_EID_CNT];
    } out;
};

/* ---------- import_jetty ---------- */

struct uburma_cmd_import_jetty {
    struct {
        uint8_t eid[UBCORE_EID_SIZE];
        uint32_t id;
        uint32_t flag;
        uint32_t token;
        uint32_t trans_mode;
        uint32_t policy;
        uint32_t type;
        uint32_t tp_type;
    } in;
    struct {
        uint32_t tpn;
        uint64_t handle;
    } out;
    struct uburma_cmd_udrv_priv udata;
};

/* ---------- bind_jetty ---------- */

struct uburma_cmd_bind_jetty {
    struct {
        uint64_t jetty_handle;
        uint64_t tjetty_handle;
    } in;
    struct {
        uint32_t tpn;
    } out;
    struct uburma_cmd_udrv_priv udata;
};

/* ---------- query_device_attr ---------- */

struct ubcore_guid_user {
    uint8_t raw[16];
};

struct uburma_cmd_device_cap {
    union ubcore_device_feat_user feature;
    uint32_t max_jfc;
    uint32_t max_jfs;
    uint32_t max_jfr;
    uint32_t max_jetty;
    uint32_t max_jetty_grp;
    uint32_t max_jetty_in_jetty_grp;
    uint32_t max_jfc_depth;
    uint32_t max_jfs_depth;
    uint32_t max_jfr_depth;
    uint32_t max_jfs_inline_len;
    uint32_t max_jfs_sge;
    uint32_t max_jfs_rsge;
    uint32_t max_jfr_sge;
    uint64_t max_msg_size;
    uint32_t max_read_size;
    uint32_t max_write_size;
    uint32_t max_cas_size;
    uint32_t max_swap_size;
    uint32_t max_fetch_and_add_size;
    uint32_t max_fetch_and_sub_size;
    uint32_t max_fetch_and_and_size;
    uint32_t max_fetch_and_or_size;
    uint32_t max_fetch_and_xor_size;
    union ubcore_atomic_feat_user atomic_feat;
    uint16_t trans_mode;
    uint16_t sub_trans_mode_cap;
    uint16_t congestion_ctrl_alg;
    uint32_t ceq_cnt;
    uint32_t max_tp_in_tpg;
    uint32_t max_eid_cnt;
    uint64_t page_size_cap;
    uint32_t max_oor_cnt;
    uint32_t mn;
    uint32_t max_netaddr_cnt;
    union ubcore_order_type_cap_user rm_order_cap;
    union ubcore_order_type_cap_user rc_order_cap;
    union urma_tp_type_cap_user rm_tp_cap;
    union urma_tp_type_cap_user rc_tp_cap;
    union urma_tp_type_cap_user um_tp_cap;
    union urma_tp_feature_user tp_feature;
    struct ubcore_sl_info_user priority_info[UBCORE_MAX_PRIORITY_CNT];
};

enum ubcore_mtu_user {
    UBCORE_MTU_256_USER = 1,
    UBCORE_MTU_512_USER,
    UBCORE_MTU_1024_USER,
    UBCORE_MTU_2048_USER,
    UBCORE_MTU_4096_USER,
    UBCORE_MTU_8192_USER
};

enum ubcore_port_state_user {
    UBCORE_PORT_DOWN_USER = 1,
    UBCORE_PORT_INIT_USER,
    UBCORE_PORT_ARMED_USER,
    UBCORE_PORT_ACTIVE_USER,
};

enum ubcore_speed_user {
    UBCORE_SP_10M_USER = 0,
};

enum ubcore_link_width_user {
    UBCORE_LINK_X1_USER = 0x1,
};

struct uburma_cmd_port_attr {
    enum ubcore_mtu_user max_mtu;
    enum ubcore_port_state_user state;
    enum ubcore_link_width_user active_width;
    enum ubcore_speed_user active_speed;
    enum ubcore_mtu_user active_mtu;
};

struct uburma_cmd_device_attr {
    struct ubcore_guid_user guid;
    struct uburma_cmd_device_cap dev_cap;
    uint8_t port_cnt;
    struct uburma_cmd_port_attr port_attr[UBURMA_CMD_MAX_PORT_CNT];
    uint32_t reserved_jetty_id_min;
    uint32_t reserved_jetty_id_max;
};

struct uburma_cmd_query_device_attr {
    struct {
        char dev_name[UBCORE_MAX_DEV_NAME];
    } in;
    struct {
        struct uburma_cmd_device_attr attr;
    } out;
};

/* ---------- Static assertions for struct sizes ---------- */

static_assert(sizeof(struct uburma_cmd_hdr) == 16,
              "uburma_cmd_hdr size mismatch");
static_assert(sizeof(struct uburma_cmd_udrv_priv) == 32,
              "uburma_cmd_udrv_priv size mismatch");
static_assert(sizeof(struct uburma_cmd_alloc_token_id) == 56,
              "uburma_cmd_alloc_token_id size mismatch");
static_assert(sizeof(struct uburma_cmd_free_token_id) == 16,
              "uburma_cmd_free_token_id size mismatch");
static_assert(sizeof(struct uburma_cmd_create_ctx) == 56,
              "uburma_cmd_create_ctx size mismatch");
static_assert(sizeof(struct uburma_cmd_alloc_jfc) == 80,
              "uburma_cmd_alloc_jfc size mismatch");
static_assert(sizeof(struct uburma_cmd_active_jfc) == 80,
              "uburma_cmd_active_jfc size mismatch");
static_assert(sizeof(struct uburma_cmd_alloc_jfr) == 104,
              "uburma_cmd_alloc_jfr size mismatch");
static_assert(sizeof(struct uburma_cmd_active_jfr) == 112,
              "uburma_cmd_active_jfr size mismatch");
static_assert(sizeof(struct uburma_cmd_alloc_jfs) == 104,
              "uburma_cmd_alloc_jfs size mismatch");
static_assert(sizeof(struct uburma_cmd_active_jfs) == 112,
              "uburma_cmd_active_jfs size mismatch");
static_assert(sizeof(struct uburma_cmd_create_jetty) == 176,
              "uburma_cmd_create_jetty size mismatch");
static_assert(sizeof(struct uburma_cmd_alloc_jetty) == 176,
              "uburma_cmd_alloc_jetty size mismatch");
static_assert(sizeof(struct uburma_cmd_active_jetty) == 88,
              "uburma_cmd_active_jetty size mismatch");
static_assert(sizeof(struct uburma_cmd_register_seg) == 88,
              "uburma_cmd_register_seg size mismatch");
static_assert(sizeof(struct uburma_cmd_unregister_seg) == 8,
              "uburma_cmd_unregister_seg size mismatch");
static_assert(sizeof(struct uburma_cmd_import_jetty) == 96,
              "uburma_cmd_import_jetty size mismatch");
static_assert(sizeof(struct uburma_cmd_bind_jetty) == 56,
              "uburma_cmd_bind_jetty size mismatch");
static_assert(sizeof(struct uburma_cmd_device_cap) == 304,
              "uburma_cmd_device_cap size mismatch");
static_assert(sizeof(struct uburma_cmd_device_attr) == 496,
              "uburma_cmd_device_attr size mismatch");
static_assert(sizeof(struct uburma_cmd_query_device_attr) == 560,
              "uburma_cmd_query_device_attr size mismatch");

#endif /* UBURMA_CMD_USER_COMPAT_H */
