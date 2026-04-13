# SIM Decoder Integration Guide

## Remaining Integration Steps

### 1. Message Channel (Phase B)

#### Option A: Using message_sync_request()
```c
/* In ub_sim_decoder_ctrl.c */
#include "ub_message.h"

static int send_control_message(struct ub_sim_dec_ctrl_adapter *adapter,
                                struct sim_dec_msg_hdr *hdr,
                                void *payload, void *resp, u16 resp_len)
{
    uint8_t *msg_buf;
    uint32_t msg_len = sizeof(*hdr) + hdr->payload_len;
    int ret;
    
    msg_buf = kmalloc(msg_len, GFP_KERNEL);
    if (!msg_buf)
        return -ENOMEM;
    
    memcpy(msg_buf, hdr, sizeof(*hdr));
    memcpy(msg_buf + sizeof(*hdr), payload, hdr->payload_len);
    
    /* Use message_sync_request() for blocking call */
    ret = message_sync_request(adapter->dest_cna, MSG_TYPE_SIM_DEC,
                               msg_buf, msg_len, resp, resp_len,
                               SIM_DEC_TIMEOUT_MS);
    
    kfree(msg_buf);
    return ret;
}
```

#### Option B: Using ctrlq (Async)
```c
/* Submit to ctrlq and wait for completion via callback */
struct sim_dec_pending_cmd {
    struct completion done;
    u16 seq;
    void *resp;
    u16 resp_len;
    int status;
};

static int submit_ctrlq_cmd(struct ub_sim_dec_ctrl_adapter *adapter,
                            struct sim_dec_msg_hdr *hdr, void *payload)
{
    struct sim_dec_pending_cmd *cmd;
    /* ... populate ctrlq entry ... */
    init_completion(&cmd->done);
    /* ... submit to ctrlq ... */
    wait_for_completion_timeout(&cmd->done, SIM_DEC_TIMEOUT_MS);
    return cmd->status;
}
```

### 2. OBMM Integration (Phase B)

```c
/* In drivers/ub/ubus/memory.c or obmm.c */

int prepare_import_memory(struct obmm_import_params *params)
{
    struct sim_dec_map_req req;
    u64 map_id;
    int ret;
    
    /* Populate SIM_DEC MAP request */
    req.local_pa = params->local_pa;
    req.size = params->size;
    req.remote_uba = params->remote_uba;  /* From export node */
    req.token_id = params->token_id;
    req.token_value = params->token_value;
    req.scna = params->src_cna;
    req.dcna = params->dst_cna;
    memcpy(req.seid, params->src_eid, 16);
    memcpy(req.deid, params->dst_eid, 16);
    req.upi = params->upi;
    req.src_eid = params->src_eid_compact;
    
    /* Call decoder service */
    ret = ub_sim_decoder_map(&g_decoder->service, &req, &map_id);
    if (ret < 0)
        return ret;
    
    /* Register with OBMM */
    return register_obmm_region(params, map_id);
}

void release_import_memory(struct obmm_region *region)
{
    /* Unmap decoder entry */
    ub_sim_decoder_unmap(&g_decoder->service, region->map_id);
    
    /* Release OBMM resources */
    unregister_obmm_region(region);
}
```

### 3. UMMU Integration (Phase B)

```c
/* In ub_ummu.c - ummu_translate() or ummu_ptw_64_s1() */

int ummu_translate(U MMUState *s, uint64_t iova, uint64_t *pa,
                   uint32_t *attrs, uint32_t *token_id)
{
    /* ... existing translation logic ... */
    
    /* After getting PA, check if it's in decoder map */
    uint64_t remote_uba;
    uint32_t dec_token_id, src_eid;
    
    if (sim_dec_lookup_by_pa(*pa, &remote_uba, &dec_token_id, &src_eid) == 0) {
        /* This PA is mapped for remote access */
        /* Override with decoder info */
        *pa = remote_uba;
        if (token_id)
            *token_id = dec_token_id;
        
        /* Set appropriate attributes for cross-node access */
        *attrs |= UMMU_ATTR_CROSS_NODE;
    }
    
    return 0;
}
```

### 4. UDMA Token Propagation (Phase B)

```c
/* In ub_ubc.c - ubc_process_sq_wqe() */

static void ubc_process_sq_wqe(BusControllerDev *ubc_dev, UBCJettyState *js)
{
    UBCWqe *wqe = ...;
    uint32_t token_id;
    
    /* Extract token_id from WQE */
    token_id = wqe->token_id;  /* From WQE layout */
    
    /* Pass to UMMU for translation */
    ummu_translate_with_token(ubc_dev->ummu, wqe->va, &pa, &attrs, token_id);
}
```

### 5. Cross-Node WRITE (Phase B)

```c
/* In ub_ubc.c - ubc_send_data_to_remote_ex() */

int ubc_send_data_to_remote_ex(BusControllerDev *ubc_dev, uint32_t dcna,
                               UBCWqe *wqe, const uint8_t *data, uint32_t len)
{
    uint64_t remote_addr;
    
    /* Check if using decoder mapping */
    if (wqe->flags & WQE_FLAG_DECODER_MAPPED) {
        /* Get remote_uba from decoder entry */
        remote_addr = wqe->decoder_remote_uba + wqe->offset;
    } else {
        /* Use direct UBA */
        remote_addr = wqe->remote_addr;
    }
    
    /* Build and send WRITE message */
    return send_write_message(dcna, remote_addr, data, len, wqe->token_id);
}
```

## Debugging Tips

### Enable QEMU Logging
```bash
./qemu-system-aarch64 -d sim_dec,ubc,ummu 2>&1 | tee qemu.log
```

### Guest Kernel Debugging
```bash
echo 8 > /proc/sys/kernel/printk  # Enable all kernel logs
echo 'module ub_sim_decoder +p' > /sys/kernel/debug/dynamic_debug/control
```

### Check Decoder Maps
```bash
# In QEMU monitor
(qemu) info sim_dec_maps

# In Guest
$ cat /sys/kernel/debug/ub/sim_decoder/maps
```

## Testing Scenarios

### 1. Basic MAP/UNMAP
```bash
# Guest: Trigger import
echo "import 0x1000000 0x10000 0x2000000" > /sys/bus/ub/devices/ub0/obmm/import

# Check QEMU logs for SIM_DEC_MAP

# Guest: Release
echo "unimport 0x1000000" > /sys/bus/ub/devices/ub0/obmm/unimport
```

### 2. Cross-Node WRITE
```bash
# Node 0: Export memory
echo "export 0x1000000 0x10000" > /sys/bus/ub/devices/ub0/export

# Node 1: Import and WRITE
# UDMA WRITE to imported region
```

### 3. Token Validation
```bash
# Try access with invalid token - should fail
# Try access with valid token - should succeed
```
