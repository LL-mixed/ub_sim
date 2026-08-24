use std::collections::HashMap;
use std::ffi::CStr;
use std::os::raw::{c_char, c_int};

use sim_config::ScenarioConfig;
use sim_services::object::LingquObmmObjectRefWire;
use sim_services::shmem::DEFAULT_MAX_SEGMENT_BYTES;
use sim_topology::SimTopology;

use crate::{GuestDescriptor, GuestEndpointSession, QemuBackendAdapter};

const DEFAULT_SEGMENT_BYTES_FALLBACK: u64 = DEFAULT_MAX_SEGMENT_BYTES;
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
