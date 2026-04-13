use sim_core::{CompletionEvent, EntityId, HealthStatus, SegmentHandle, SimError};
use sim_services::{
    block::BlockServiceProfile, db::DbServiceProfile, dfs::DfsServiceProfile,
    shmem::ShmemServiceProfile,
};
use sim_topology::{SimTopology, TopologySnapshot};
use sim_uapi::{LocalGuestUapiSurface, UapiCommand, UapiResponse};

use crate::types::{GuestDescriptor, GuestEndpointSession, MachineProfile};

#[derive(Debug)]
pub struct QemuBackendAdapter {
    profile: MachineProfile,
    surface: LocalGuestUapiSurface,
}

impl QemuBackendAdapter {
    pub fn new(topology: SimTopology) -> Self {
        Self {
            profile: MachineProfile::default(),
            surface: tuned_surface_from_env(topology),
        }
    }

    pub fn with_profile(topology: SimTopology, profile: MachineProfile) -> Self {
        Self {
            profile,
            surface: tuned_surface_from_env(topology),
        }
    }

    pub fn profile(&self) -> &MachineProfile {
        &self.profile
    }

    pub fn query_topology(&mut self) -> Result<TopologySnapshot, SimError> {
        match self.surface.execute(UapiCommand::QueryTopology)? {
            UapiResponse::TopologySnapshot(snapshot) => Ok(snapshot),
            _ => Err(SimError::InvalidInput("unexpected topology response")),
        }
    }

    pub fn register_endpoint(
        &mut self,
        entity: EntityId,
    ) -> Result<GuestEndpointSession, SimError> {
        let cq = match self
            .surface
            .execute(UapiCommand::RegisterCq { owner: entity })?
        {
            UapiResponse::CqRegistered(cq) => cq,
            _ => return Err(SimError::InvalidInput("unexpected cq registration response")),
        };
        let cmdq = match self.surface.execute(UapiCommand::CreateCmdQueue {
            cq,
            owner: entity,
            depth: self.profile.endpoint.cmdq_depth as usize,
        })? {
            UapiResponse::CmdQueueCreated(cmdq) => cmdq,
            _ => return Err(SimError::InvalidInput("unexpected cmdq creation response")),
        };
        Ok(GuestEndpointSession {
            entity,
            cq,
            cmdq,
            layout: self.profile.endpoint,
        })
    }

    pub fn create_segment(
        &mut self,
        _session: &GuestEndpointSession,
        bytes: u64,
    ) -> Result<SegmentHandle, SimError> {
        match self.surface.execute(UapiCommand::CreateSegment { bytes })? {
            UapiResponse::SegmentCreated(segment) => Ok(segment),
            _ => Err(SimError::InvalidInput("unexpected segment response")),
        }
    }

    pub fn enqueue_descriptor(
        &mut self,
        session: &GuestEndpointSession,
        desc: GuestDescriptor,
    ) -> Result<(usize, usize), SimError> {
        match self.surface.execute(UapiCommand::EnqueueCmd {
            cmdq: session.cmdq,
            owner: session.entity,
            desc: desc.to_uapi_descriptor(),
        })? {
            UapiResponse::CommandEnqueued {
                depth,
                remaining_capacity,
            } => Ok((depth, remaining_capacity)),
            _ => Err(SimError::InvalidInput("unexpected enqueue response")),
        }
    }

    pub fn ring_doorbell(
        &mut self,
        session: &GuestEndpointSession,
        max_batch: Option<usize>,
    ) -> Result<(usize, usize), SimError> {
        match self.surface.execute(UapiCommand::RingDoorbell {
            cmdq: session.cmdq,
            owner: session.entity,
            max_batch,
        })? {
            UapiResponse::DoorbellRung { submitted, pending } => Ok((submitted, pending)),
            _ => Err(SimError::InvalidInput("unexpected doorbell response")),
        }
    }

    pub fn poll_cq(
        &mut self,
        session: &GuestEndpointSession,
        max_entries: Option<usize>,
    ) -> Result<(Vec<CompletionEvent>, usize), SimError> {
        match self.surface.execute(UapiCommand::PollCq {
            cq: session.cq,
            owner: session.entity,
            max_entries,
        })? {
            UapiResponse::Completions { events, remaining } => Ok((events, remaining)),
            _ => Err(SimError::InvalidInput("unexpected cq poll response")),
        }
    }

    pub fn drain_cq(
        &mut self,
        session: &GuestEndpointSession,
    ) -> Result<Vec<CompletionEvent>, SimError> {
        match self.surface.execute(UapiCommand::DrainCq {
            cq: session.cq,
            owner: session.entity,
        })? {
            UapiResponse::Completions { events, .. } => Ok(events),
            _ => Err(SimError::InvalidInput("unexpected cq drain response")),
        }
    }

    pub fn get_health(&mut self, entity: EntityId) -> Result<HealthStatus, SimError> {
        match self.surface.execute(UapiCommand::GetHealth { entity })? {
            UapiResponse::HealthStatus(health) => Ok(health),
            _ => Err(SimError::InvalidInput("unexpected health response")),
        }
    }
}

fn env_usize(name: &str) -> Option<usize> {
    std::env::var(name).ok()?.parse().ok()
}

fn env_u64(name: &str) -> Option<u64> {
    std::env::var(name).ok()?.parse().ok()
}

fn env_u32(name: &str) -> Option<u32> {
    std::env::var(name).ok()?.parse().ok()
}

fn tuned_surface_from_env(topology: SimTopology) -> LocalGuestUapiSurface {
    let block_profile = BlockServiceProfile {
        queue_depth: env_usize("LINQU_UB_BLOCK_QUEUE_DEPTH").unwrap_or(16),
        ..BlockServiceProfile::default()
    };
    let shmem_profile = ShmemServiceProfile {
        queue_depth: env_usize("LINQU_UB_SHMEM_QUEUE_DEPTH").unwrap_or(16),
        ..ShmemServiceProfile::default()
    };
    let dfs_profile = DfsServiceProfile {
        queue_depth: env_usize("LINQU_UB_DFS_QUEUE_DEPTH").unwrap_or(16),
        ..DfsServiceProfile::default()
    };
    let db_profile = DbServiceProfile {
        queue_depth: env_usize("LINQU_UB_DB_QUEUE_DEPTH").unwrap_or(16),
        ..DbServiceProfile::default()
    };
    LocalGuestUapiSurface::with_profiles_and_runtime_policy(
        topology,
        block_profile,
        shmem_profile,
        dfs_profile,
        db_profile,
        env_u64("LINQU_UB_RUNTIME_ISSUE_LATENCY_US").unwrap_or(4),
        env_u64("LINQU_UB_RUNTIME_RETRY_DELAY_US").unwrap_or(5),
        env_usize("LINQU_UB_RUNTIME_QUEUE_DEPTH").unwrap_or(32),
        env_u32("LINQU_UB_RUNTIME_MAX_RETRIES").unwrap_or(1),
    )
}
