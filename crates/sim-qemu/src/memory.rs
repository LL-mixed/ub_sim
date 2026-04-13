use sim_core::CompletionEvent;

use crate::types::{decode_completion, encode_completion, GuestDescriptor};

#[derive(Debug)]
pub struct GuestRingMemory {
    slot_bytes: usize,
    cmd_slots: Vec<Vec<u8>>,
    cmd_valid: Vec<bool>,
    cq_slots: Vec<Vec<u8>>,
    cq_valid: Vec<bool>,
}

impl GuestRingMemory {
    pub fn new(cmd_depth: usize, cq_depth: usize, slot_bytes: usize) -> Self {
        Self {
            slot_bytes,
            cmd_slots: vec![vec![0; slot_bytes]; cmd_depth],
            cmd_valid: vec![false; cmd_depth],
            cq_slots: vec![vec![0; slot_bytes]; cq_depth],
            cq_valid: vec![false; cq_depth],
        }
    }

    pub fn cmd_depth(&self) -> usize {
        self.cmd_slots.len()
    }

    pub fn cq_depth(&self) -> usize {
        self.cq_slots.len()
    }

    pub fn write_cmd_slot(
        &mut self,
        slot: usize,
        desc: GuestDescriptor,
    ) -> Result<(), &'static str> {
        if slot >= self.cmd_slots.len() {
            return Err("cmd slot out of range");
        }
        self.cmd_slots[slot] = desc.encode(self.slot_bytes)?;
        self.cmd_valid[slot] = true;
        Ok(())
    }

    pub fn take_cmd_slot(&mut self, slot: usize) -> Result<GuestDescriptor, &'static str> {
        if slot >= self.cmd_slots.len() {
            return Err("cmd slot out of range");
        }
        if !self.cmd_valid[slot] {
            return Err("missing descriptor in cmd ring");
        }
        self.cmd_valid[slot] = false;
        GuestDescriptor::decode(&self.cmd_slots[slot])
    }

    pub fn write_cq_slot(
        &mut self,
        slot: usize,
        completion: CompletionEvent,
    ) -> Result<(), &'static str> {
        if slot >= self.cq_slots.len() {
            return Err("cq slot out of range");
        }
        self.cq_slots[slot] = encode_completion(&completion, self.slot_bytes)?;
        self.cq_valid[slot] = true;
        Ok(())
    }

    pub fn take_cq_slot(&mut self, slot: usize) -> Result<CompletionEvent, &'static str> {
        if slot >= self.cq_slots.len() {
            return Err("cq slot out of range");
        }
        if !self.cq_valid[slot] {
            return Err("missing completion in cq ring");
        }
        self.cq_valid[slot] = false;
        decode_completion(&self.cq_slots[slot])
    }
}
