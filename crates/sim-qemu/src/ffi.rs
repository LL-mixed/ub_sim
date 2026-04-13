use std::collections::HashMap;
use std::ffi::CStr;
use std::os::raw::{c_char, c_int};

use sim_config::ScenarioConfig;
use sim_topology::SimTopology;

use crate::{GuestDescriptor, GuestEndpointSession, QemuBackendAdapter};

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
            .create_segment(&session, 4096)
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

    fn submit_slot(
        &mut self,
        endpoint_id: u16,
        slot: &[u8],
    ) -> Result<(), &'static str> {
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
    match bridge_mut(ptr).and_then(|bridge| {
        bridge
            .ring_doorbell(endpoint_id, max_batch)
            .map_err(|_| -1)
    }) {
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
