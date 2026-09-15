use std::collections::HashMap;
use std::ffi::c_void;
use std::ffi::CStr;
use std::os::raw::{c_char, c_int};

use sim_config::ScenarioConfig;
use sim_core::PtoUbGmAccessRegistration;
use sim_services::managed_object_ref::ManagedObjectRef;
use sim_services::object::LingquObmmObjectRefWire;
use sim_services::shmem::DEFAULT_MAX_SEGMENT_BYTES;
use sim_topology::SimTopology;
use sim_uapi::{
    pto_ub_gm_registered_callable_fingerprint, PtoUbGmDispatchV2Req,
    PTO_UB_GM_HOST_VECTOR_CALLABLE_ID, PTO_UB_GM_QWEN3_OPERATOR_CALLABLE_ID,
    PTO_UB_GM_QWEN3_RANGE_CALLABLE_ID,
};

use crate::ub_gm_abi::{
    materialize_authorized_dispatch_args, LingquPtoDispatchControlV2, LingquPtoScalarV1,
    LingquPtoUbGmError, PtoSimUbGmAccessOpsV1, PtoSimUbGmAuthorizedMemrefV1, LINGQU_PTO_CNA_MAX,
    LINGQU_PTO_MAX_MEMREFS, LINGQU_PTO_MAX_SCALARS,
};
use crate::{GuestDescriptor, GuestEndpointSession, QemuBackendAdapter};

const DEFAULT_SEGMENT_BYTES_FALLBACK: u64 = DEFAULT_MAX_SEGMENT_BYTES;

/// Borrowed provider-read view. Reference is exactly 256 bytes; payload is the
/// referenced view, not the allocation base. Both remain valid during enqueue.
#[repr(C)]
pub struct LinquManagedModelInputV2 {
    pub reference: *const u8,
    pub payload: *const u8,
    pub payload_len: usize,
}

/// Enqueue V2 inputs with a compact model-range header (kind 9, 64 bytes).
/// This explicit API selects a 256-byte reference table. Ordinary enqueue
/// retains its V1 contract. No holder is granted here: the provider must retain
/// SDK mappings/leases until the original operation's completion is observed.
#[unsafe(no_mangle)]
pub extern "C" fn linqu_ub_bridge_enqueue_managed_model_range_v2(
    ptr: *mut LinquUbBridge,
    endpoint_id: u16,
    slot: *const u8,
    slot_len: usize,
    inputs: *const LinquManagedModelInputV2,
    input_count: usize,
) -> c_int {
    if slot.is_null() || slot_len != 64 || input_count > 2 || (input_count != 0 && inputs.is_null())
    {
        return -1;
    }
    let Ok(bridge) = bridge_mut(ptr) else {
        return -1;
    };
    let Some(endpoint) = bridge.sessions.get(&endpoint_id).cloned() else {
        return -1;
    };
    let slot = unsafe { std::slice::from_raw_parts(slot, slot_len) };
    let Ok(GuestDescriptor::ModelRangeDispatch(req)) = GuestDescriptor::decode(slot) else {
        return -1;
    };
    if req.segment.0 != endpoint.default_segment || req.object_ref_count as usize != input_count {
        return -1;
    }
    let views = if input_count == 0 {
        &[][..]
    } else {
        unsafe { std::slice::from_raw_parts(inputs, input_count) }
    };
    let mut owned = Vec::with_capacity(input_count);
    for view in views {
        if view.reference.is_null()
            || view.payload.is_null()
            || view.payload_len > isize::MAX as usize
        {
            return -1;
        }
        let wire =
            unsafe { std::slice::from_raw_parts(view.reference, ManagedObjectRef::BYTE_LEN) };
        let Ok(reference) = ManagedObjectRef::from_le_bytes(wire) else {
            return -1;
        };
        if reference.payload_bytes() != view.payload_len as u64 {
            return -1;
        }
        let payload = unsafe { std::slice::from_raw_parts(view.payload, view.payload_len) };
        let Ok(input) = sim_uapi::managed_model_input::ManagedModelInput::from_provider_bytes(
            reference,
            payload.to_vec(),
        ) else {
            return -1;
        };
        owned.push(input);
    }
    let req = sim_uapi::ModelRangeDispatchReq {
        op_id: req.op_id,
        segment: req.segment,
        task: req.task,
        object_ref_table_offset: req.object_ref_table_offset,
        object_ref_count: req.object_ref_count,
    };
    match bridge
        .adapter
        .enqueue_managed_model_range(&endpoint.session, req, owned)
    {
        Ok(_) => 0,
        Err(_) => -1,
    }
}
const DEFAULT_SEGMENT_BYTES_MIN: u64 = 1024 * 1024;

fn parse_default_segment_bytes(value: Option<&str>) -> u64 {
    value
        .and_then(|value| value.parse::<u64>().ok())
        .filter(|bytes| *bytes >= DEFAULT_SEGMENT_BYTES_MIN)
        .unwrap_or(DEFAULT_SEGMENT_BYTES_FALLBACK)
}

fn default_segment_bytes() -> u64 {
    let value = std::env::var("SIM_QEMU_DEFAULT_SEGMENT_BYTES").ok();
    parse_default_segment_bytes(value.as_deref())
}

pub struct LinquUbBridge {
    adapter: QemuBackendAdapter,
    sessions: HashMap<u16, BridgeEndpointSession>,
    ub_gm_access: Option<RegisteredUbGmAccessV1>,
}

#[derive(Clone, Copy)]
struct RegisteredUbGmAccessV1 {
    _ops: PtoSimUbGmAccessOpsV1,
    _qemu_context: *mut c_void,
    pto_device_cna: u32,
}

#[derive(Clone)]
struct BridgeEndpointSession {
    session: GuestEndpointSession,
    default_segment: u64,
}

impl LinquUbBridge {
    fn from_yaml_path(path: &str) -> Result<Self, &'static str> {
        let config = ScenarioConfig::from_yaml_file(path).map_err(|_| "invalid scenario file")?;
        let topology = SimTopology::from_config(&config).map_err(|_| "invalid topology")?;
        Ok(Self {
            adapter: QemuBackendAdapter::new(topology),
            sessions: HashMap::new(),
            ub_gm_access: None,
        })
    }

    fn register_endpoint(&mut self, endpoint_id: u16, entity_id: u32) -> Result<(), &'static str> {
        let session = self
            .adapter
            .register_endpoint(entity_id)
            .map_err(|_| "register endpoint failed")?;
        let default_segment = self
            .adapter
            .create_segment(&session, default_segment_bytes())
            .map_err(|_| "create default segment failed")?;
        self.sessions.insert(
            endpoint_id,
            BridgeEndpointSession {
                session,
                default_segment: default_segment.0,
            },
        );
        Ok(())
    }

    fn default_segment(&self, endpoint_id: u16) -> Result<u64, &'static str> {
        self.sessions
            .get(&endpoint_id)
            .map(|session| session.default_segment)
            .ok_or("unknown endpoint")
    }

    fn register_ub_gm_access_v1(
        &mut self,
        ops: PtoSimUbGmAccessOpsV1,
        qemu_context: *mut c_void,
        pto_device_cna: u32,
    ) -> Result<(), LingquPtoUbGmError> {
        ops.validate()?;
        if qemu_context.is_null() || pto_device_cna == 0 || pto_device_cna > LINGQU_PTO_CNA_MAX {
            return Err(LingquPtoUbGmError::BadControlTable);
        }
        self.adapter
            .register_pto_ub_gm_access(PtoUbGmAccessRegistration {
                read: ops.read.ok_or(LingquPtoUbGmError::CallbackFailed)?,
                write: ops.write.ok_or(LingquPtoUbGmError::CallbackFailed)?,
                fence: ops.fence.ok_or(LingquPtoUbGmError::CallbackFailed)?,
                backend_context: qemu_context as usize,
                pto_device_cna,
            })
            .map_err(|_| LingquPtoUbGmError::BadControlTable)?;
        self.ub_gm_access = Some(RegisteredUbGmAccessV1 {
            _ops: ops,
            _qemu_context: qemu_context,
            pto_device_cna,
        });
        Ok(())
    }

    fn query_ub_gm_callable_v1(&self, callable_id: u64) -> Result<u64, LingquPtoUbGmError> {
        if !matches!(
            callable_id,
            PTO_UB_GM_HOST_VECTOR_CALLABLE_ID
                | PTO_UB_GM_QWEN3_OPERATOR_CALLABLE_ID
                | PTO_UB_GM_QWEN3_RANGE_CALLABLE_ID
        ) {
            return Err(LingquPtoUbGmError::UnsupportedCallable);
        }
        pto_ub_gm_registered_callable_fingerprint(callable_id).map_err(|error| {
            eprintln!(
                "SIM_QEMU_UB_GM_CALLABLE_QUERY_FAILED callable={} error={}",
                callable_id, error
            );
            LingquPtoUbGmError::ExecutionFailed
        })
    }

    fn submit_ub_gm_v2(
        &mut self,
        endpoint_id: u16,
        op_id: u64,
        control: LingquPtoDispatchControlV2,
        memrefs: &[PtoSimUbGmAuthorizedMemrefV1],
        scalars: &[LingquPtoScalarV1],
    ) -> Result<(), LingquPtoUbGmError> {
        if op_id == 0 {
            return Err(LingquPtoUbGmError::BadControlTable);
        }
        let access = self.ub_gm_access.ok_or(LingquPtoUbGmError::Unbound)?;
        let expected_fingerprint = self.query_ub_gm_callable_v1(control.callable_id)?;
        let args = materialize_authorized_dispatch_args(
            &control,
            memrefs,
            scalars,
            control.callable_id,
            expected_fingerprint,
            access.pto_device_cna,
        )?;
        let session = self
            .sessions
            .get(&endpoint_id)
            .ok_or(LingquPtoUbGmError::BadControlTable)?
            .session
            .clone();
        self.adapter
            .enqueue_pto_ub_gm_dispatch(
                &session,
                PtoUbGmDispatchV2Req {
                    op_id,
                    request_id: control.request_id,
                    callable_id: control.callable_id,
                    artifact_fingerprint: control.artifact_fingerprint,
                    requester_cna: control.requester_cna,
                    args,
                },
            )
            .map_err(|_| LingquPtoUbGmError::ExecutionFailed)?;
        Ok(())
    }

    fn submit_slot(&mut self, endpoint_id: u16, slot: &[u8]) -> Result<(), &'static str> {
        let session = self
            .sessions
            .get(&endpoint_id)
            .ok_or("unknown endpoint")?
            .session
            .clone();
        let desc = GuestDescriptor::decode(slot)?;
        let _ = self
            .adapter
            .enqueue_descriptor(&session, desc)
            .map_err(|_| "enqueue failed")?;
        Ok(())
    }

    fn write_segment_payload(
        &mut self,
        segment: u64,
        offset: usize,
        bytes: &[u8],
    ) -> Result<(), &'static str> {
        self.adapter
            .write_segment_payload(sim_core::SegmentHandle(segment), offset, bytes)
            .map_err(|_| "write segment payload failed")
    }

    fn read_segment_payload(
        &self,
        segment: u64,
        offset: usize,
        out: &mut [u8],
    ) -> Result<(), &'static str> {
        self.adapter
            .read_segment_payload(sim_core::SegmentHandle(segment), offset, out)
            .map_err(|_| "read segment payload failed")
    }

    fn register_model_runtime_object_payload(
        &mut self,
        object_ref: LingquObmmObjectRefWire,
        payload: &[u8],
    ) -> Result<(), &'static str> {
        self.adapter
            .register_model_runtime_object_payload(
                object_ref,
                payload.to_vec(),
                "qemu_obmm_bridge_live_payload",
            )
            .map_err(|_| "register model runtime object payload failed")
    }

    fn ring_doorbell(
        &mut self,
        endpoint_id: u16,
        max_batch: u32,
    ) -> Result<(u32, u32), &'static str> {
        let session = self
            .sessions
            .get(&endpoint_id)
            .ok_or("unknown endpoint")?
            .session
            .clone();
        let (submitted, pending) = self
            .adapter
            .ring_doorbell(&session, Some(max_batch as usize))
            .map_err(|_| "doorbell failed")?;
        Ok((submitted as u32, pending as u32))
    }

    fn poll_completion(
        &mut self,
        endpoint_id: u16,
        slot_out: &mut [u8],
    ) -> Result<bool, &'static str> {
        let session = self
            .sessions
            .get(&endpoint_id)
            .ok_or("unknown endpoint")?
            .session
            .clone();
        let (events, _) = self
            .adapter
            .poll_cq(&session, Some(1))
            .map_err(|_| "poll completion failed")?;
        if let Some(event) = events.into_iter().next() {
            let encoded = crate::types::encode_completion(&event, slot_out.len())?;
            slot_out.copy_from_slice(&encoded);
            Ok(true)
        } else {
            Ok(false)
        }
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn linqu_ub_bridge_register_model_runtime_object_payload(
    ptr: *mut LinquUbBridge,
    object_ref_data: *const u8,
    object_ref_len: usize,
    payload: *const u8,
    payload_len: usize,
) -> c_int {
    if object_ref_data.is_null() || payload.is_null() {
        return -1;
    }
    let object_ref_data = unsafe { std::slice::from_raw_parts(object_ref_data, object_ref_len) };
    let payload = unsafe { std::slice::from_raw_parts(payload, payload_len) };
    let object_ref = match LingquObmmObjectRefWire::from_le_bytes(object_ref_data) {
        Ok(object_ref) => object_ref,
        Err(_) => return -1,
    };
    match bridge_mut(ptr).and_then(|bridge| {
        bridge
            .register_model_runtime_object_payload(object_ref, payload)
            .map(|_| 0)
            .map_err(|_| -1)
    }) {
        Ok(code) => code,
        Err(code) => code,
    }
}

fn bridge_mut<'a>(ptr: *mut LinquUbBridge) -> Result<&'a mut LinquUbBridge, c_int> {
    unsafe { ptr.as_mut() }.ok_or(-1)
}

#[unsafe(no_mangle)]
pub extern "C" fn linqu_ub_bridge_new_from_yaml(path: *const c_char) -> *mut LinquUbBridge {
    if path.is_null() {
        return std::ptr::null_mut();
    }
    let path = unsafe { CStr::from_ptr(path) };
    let path = match path.to_str() {
        Ok(path) => path,
        Err(_) => return std::ptr::null_mut(),
    };
    match LinquUbBridge::from_yaml_path(path) {
        Ok(bridge) => Box::into_raw(Box::new(bridge)),
        Err(_) => std::ptr::null_mut(),
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn linqu_ub_bridge_free(ptr: *mut LinquUbBridge) {
    if ptr.is_null() {
        return;
    }
    unsafe {
        drop(Box::from_raw(ptr));
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn linqu_ub_bridge_register_endpoint(
    ptr: *mut LinquUbBridge,
    endpoint_id: u16,
    entity_id: u32,
) -> c_int {
    match bridge_mut(ptr).and_then(|bridge| {
        bridge
            .register_endpoint(endpoint_id, entity_id)
            .map(|_| 0)
            .map_err(|_| -1)
    }) {
        Ok(code) => code,
        Err(code) => code,
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn linqu_ub_bridge_register_ub_gm_access_v1(
    ptr: *mut LinquUbBridge,
    ops: *const PtoSimUbGmAccessOpsV1,
    qemu_context: *mut c_void,
    pto_device_cna: u32,
) -> c_int {
    let Some(ops) = (unsafe { ops.as_ref() }).copied() else {
        return LingquPtoUbGmError::CallbackFailed.ffi_status();
    };
    match bridge_mut(ptr)
        .map_err(|_| LingquPtoUbGmError::BadControlTable)
        .and_then(|bridge| bridge.register_ub_gm_access_v1(ops, qemu_context, pto_device_cna))
    {
        Ok(()) => 0,
        Err(error) => error.ffi_status(),
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn linqu_ub_bridge_query_ub_gm_callable_v1(
    ptr: *mut LinquUbBridge,
    callable_id: u64,
    fingerprint_out: *mut u64,
) -> c_int {
    if fingerprint_out.is_null() {
        return LingquPtoUbGmError::BadControlTable.ffi_status();
    }
    match bridge_mut(ptr)
        .map_err(|_| LingquPtoUbGmError::BadControlTable)
        .and_then(|bridge| bridge.query_ub_gm_callable_v1(callable_id))
    {
        Ok(fingerprint) => {
            unsafe {
                *fingerprint_out = fingerprint;
            }
            0
        }
        Err(error) => error.ffi_status(),
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn linqu_ub_bridge_submit_ub_gm_v2(
    ptr: *mut LinquUbBridge,
    endpoint_id: u16,
    op_id: u64,
    control: *const LingquPtoDispatchControlV2,
    memrefs: *const PtoSimUbGmAuthorizedMemrefV1,
    memref_count: u32,
    scalars: *const LingquPtoScalarV1,
    scalar_count: u32,
) -> c_int {
    let Some(control) = (unsafe { control.as_ref() }).copied() else {
        return LingquPtoUbGmError::BadControlTable.ffi_status();
    };
    if memref_count > LINGQU_PTO_MAX_MEMREFS || scalar_count > LINGQU_PTO_MAX_SCALARS {
        return LingquPtoUbGmError::BadControlTable.ffi_status();
    }
    let memrefs = if memref_count == 0 {
        &[]
    } else {
        if memrefs.is_null() {
            return LingquPtoUbGmError::BadControlTable.ffi_status();
        }
        unsafe { std::slice::from_raw_parts(memrefs, memref_count as usize) }
    };
    let scalars = if scalar_count == 0 {
        &[]
    } else {
        if scalars.is_null() {
            return LingquPtoUbGmError::BadControlTable.ffi_status();
        }
        unsafe { std::slice::from_raw_parts(scalars, scalar_count as usize) }
    };
    match bridge_mut(ptr)
        .map_err(|_| LingquPtoUbGmError::BadControlTable)
        .and_then(|bridge| bridge.submit_ub_gm_v2(endpoint_id, op_id, control, memrefs, scalars))
    {
        Ok(()) => 0,
        Err(error) => error.ffi_status(),
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn linqu_ub_bridge_get_default_segment(
    ptr: *mut LinquUbBridge,
    endpoint_id: u16,
    segment_out: *mut u64,
) -> c_int {
    if segment_out.is_null() {
        return -1;
    }
    match bridge_mut(ptr).and_then(|bridge| bridge.default_segment(endpoint_id).map_err(|_| -1)) {
        Ok(segment) => {
            unsafe {
                *segment_out = segment;
            }
            0
        }
        Err(code) => code,
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn linqu_ub_bridge_submit_slot(
    ptr: *mut LinquUbBridge,
    endpoint_id: u16,
    slot: *const u8,
    slot_len: usize,
) -> c_int {
    if slot.is_null() {
        return -1;
    }
    let slot = unsafe { std::slice::from_raw_parts(slot, slot_len) };
    match bridge_mut(ptr).and_then(|bridge| {
        bridge
            .submit_slot(endpoint_id, slot)
            .map(|_| 0)
            .map_err(|_| -1)
    }) {
        Ok(code) => code,
        Err(code) => code,
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn linqu_ub_bridge_write_segment_payload(
    ptr: *mut LinquUbBridge,
    segment: u64,
    offset: usize,
    data: *const u8,
    data_len: usize,
) -> c_int {
    if data.is_null() {
        return -1;
    }
    let data = unsafe { std::slice::from_raw_parts(data, data_len) };
    match bridge_mut(ptr).and_then(|bridge| {
        bridge
            .write_segment_payload(segment, offset, data)
            .map(|_| 0)
            .map_err(|_| -1)
    }) {
        Ok(code) => code,
        Err(code) => code,
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn linqu_ub_bridge_read_segment_payload(
    ptr: *mut LinquUbBridge,
    segment: u64,
    offset: usize,
    out: *mut u8,
    out_len: usize,
) -> c_int {
    if out.is_null() {
        return -1;
    }
    let out = unsafe { std::slice::from_raw_parts_mut(out, out_len) };
    match bridge_mut(ptr).and_then(|bridge| {
        bridge
            .read_segment_payload(segment, offset, out)
            .map(|_| 0)
            .map_err(|_| -1)
    }) {
        Ok(code) => code,
        Err(code) => code,
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn linqu_ub_bridge_ring_doorbell(
    ptr: *mut LinquUbBridge,
    endpoint_id: u16,
    max_batch: u32,
    submitted_out: *mut u32,
    pending_out: *mut u32,
) -> c_int {
    if submitted_out.is_null() || pending_out.is_null() {
        return -1;
    }
    match bridge_mut(ptr)
        .and_then(|bridge| bridge.ring_doorbell(endpoint_id, max_batch).map_err(|_| -1))
    {
        Ok((submitted, pending)) => {
            unsafe {
                *submitted_out = submitted;
                *pending_out = pending;
            }
            0
        }
        Err(code) => code,
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn linqu_ub_bridge_poll_completion(
    ptr: *mut LinquUbBridge,
    endpoint_id: u16,
    slot_out: *mut u8,
    slot_len: usize,
) -> c_int {
    if slot_out.is_null() {
        return -1;
    }
    let slot_out = unsafe { std::slice::from_raw_parts_mut(slot_out, slot_len) };
    match bridge_mut(ptr).and_then(|bridge| {
        bridge
            .poll_completion(endpoint_id, slot_out)
            .map_err(|_| -1)
    }) {
        Ok(true) => 0,
        Ok(false) => 1,
        Err(code) => code,
    }
}

#[cfg(test)]
mod tests {
    use super::{linqu_ub_bridge_enqueue_managed_model_range_v2, LinquUbBridge};

    #[test]
    fn managed_enqueue_rejects_invalid_abi_and_preserves_completion_identity() {
        let path = std::path::Path::new(env!("CARGO_MANIFEST_DIR"))
            .join("../../scenarios/mvp_8host_single_domain.yaml");
        let mut bridge = LinquUbBridge::from_yaml_path(path.to_str().unwrap()).unwrap();
        bridge.register_endpoint(0, 0).unwrap();
        let segment = bridge.default_segment(0).unwrap();
        let mut slot = [0u8; 64];
        slot[0] = 9;
        slot[1..9].copy_from_slice(&459u64.to_le_bytes());
        slot[9..17].copy_from_slice(&segment.to_le_bytes());
        let call = |bridge: &mut LinquUbBridge, slot: &[u8]| {
            linqu_ub_bridge_enqueue_managed_model_range_v2(
                bridge,
                0,
                slot.as_ptr(),
                slot.len(),
                std::ptr::null(),
                0,
            )
        };
        assert_eq!(call(&mut bridge, &slot[..63]), -1);
        let mut wrong = slot;
        wrong[0] = 1;
        assert_eq!(call(&mut bridge, &wrong), -1);
        wrong = slot;
        wrong[9..17].copy_from_slice(&(segment + 1).to_le_bytes());
        assert_eq!(call(&mut bridge, &wrong), -1);
        wrong = slot;
        wrong[53..57].copy_from_slice(&1u32.to_le_bytes());
        assert_eq!(call(&mut bridge, &wrong), -1);
        assert_eq!(call(&mut bridge, &slot), 0);
        bridge.ring_doorbell(0, 1).unwrap();
        let session = bridge.sessions.get(&0).unwrap().session.clone();
        let events = bridge.adapter.drain_cq(&session).unwrap();
        assert_eq!(events.len(), 1);
        assert_eq!(events[0].op_id, 459);
        assert!(matches!(
            events[0].status,
            sim_core::CompletionStatus::FatalFailure { .. }
        ));
    }
    use super::{
        parse_default_segment_bytes, DEFAULT_SEGMENT_BYTES_FALLBACK, DEFAULT_SEGMENT_BYTES_MIN,
    };

    #[test]
    fn default_segment_bytes_uses_qwen_safe_fallback() {
        assert_eq!(
            parse_default_segment_bytes(None),
            DEFAULT_SEGMENT_BYTES_FALLBACK
        );
        assert!(DEFAULT_SEGMENT_BYTES_FALLBACK >= 8 * 1024 * 1024);
    }

    #[test]
    fn default_segment_bytes_accepts_explicit_value() {
        assert_eq!(
            parse_default_segment_bytes(Some("16777216")),
            16 * 1024 * 1024
        );
    }

    #[test]
    fn default_segment_bytes_rejects_invalid_or_too_small_value() {
        assert_eq!(
            parse_default_segment_bytes(Some("invalid")),
            DEFAULT_SEGMENT_BYTES_FALLBACK
        );
        assert_eq!(
            parse_default_segment_bytes(Some("4096")),
            DEFAULT_SEGMENT_BYTES_FALLBACK
        );
        assert_eq!(
            parse_default_segment_bytes(Some("1048576")),
            DEFAULT_SEGMENT_BYTES_MIN
        );
    }
}
