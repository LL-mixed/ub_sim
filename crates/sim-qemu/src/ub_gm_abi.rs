use std::collections::HashSet;
use std::ffi::c_void;
use std::os::raw::c_int;

use sim_core::{
    BufferUsage, SimplerRuntimeArg, SimplerUbGmAccess, SimplerUbGmBinding, SimplerUbGmView,
};

pub const LINGQU_PTO_DISPATCH_ABI_V2: u32 = 2;
pub const LINGQU_SHMEM_MEMREF_ABI_V1: u32 = 1;
pub const LINGQU_PTO_SCALAR_ABI_V1: u32 = 1;
pub const PTO_SIM_UB_GM_ACCESS_ABI_V1: u32 = 1;

pub const LINGQU_PTO_MAX_MEMREFS: u32 = 256;
pub const LINGQU_PTO_MAX_SCALARS: u32 = 128;
pub const LINGQU_PTO_MAX_RANK: u32 = 5;
pub const LINGQU_PTO_DTYPE_MAX: u16 = 14;
pub const LINGQU_PTO_CNA_MAX: u32 = 0x00ff_ffff;

#[repr(u8)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum LingquPtoMemrefRole {
    Input = 1,
    Output = 2,
    InOut = 3,
}

#[repr(u32)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum LingquPtoUbGmError {
    Ok = 0,
    UnsupportedCallable = 1,
    BadControlTable = 2,
    BadMemref = 3,
    Unbound = 4,
    AccessDenied = 5,
    AuthorizationTimeout = 6,
    CallbackFailed = 7,
    ExecutionFailed = 8,
    AuthorizationCancelled = 9,
}

impl LingquPtoUbGmError {
    pub const fn code(self) -> &'static str {
        match self {
            Self::Ok => "ok",
            Self::UnsupportedCallable => "pto_ub_gm_unsupported_callable",
            Self::BadControlTable => "pto_ub_gm_bad_control_table",
            Self::BadMemref => "pto_ub_gm_bad_memref",
            Self::Unbound => "pto_ub_gm_unbound",
            Self::AccessDenied => "pto_ub_gm_access_denied",
            Self::AuthorizationTimeout => "pto_ub_gm_authorization_timeout",
            Self::CallbackFailed => "pto_ub_gm_callback_failed",
            Self::ExecutionFailed => "pto_ub_gm_execution_failed",
            Self::AuthorizationCancelled => "pto_ub_gm_authorization_cancelled",
        }
    }

    pub const fn ffi_status(self) -> c_int {
        -(self as c_int)
    }
}

pub const LINGQU_PTO_UB_GM_READ: u8 = 1 << 0;
pub const LINGQU_PTO_UB_GM_WRITE: u8 = 1 << 1;
pub const LINGQU_PTO_UB_GM_READ_WRITE: u8 = LINGQU_PTO_UB_GM_READ | LINGQU_PTO_UB_GM_WRITE;

#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct LingquPtoDispatchControlV2 {
    pub abi_version: u32,
    pub struct_bytes: u32,
    pub request_id: u64,
    pub callable_id: u64,
    pub memref_count: u32,
    pub scalar_count: u32,
    pub memref_table_iova: u64,
    pub scalar_table_iova: u64,
    pub artifact_fingerprint: u64,
    pub metadata_crc32: u32,
    pub requester_cna: u32,
}

impl LingquPtoDispatchControlV2 {
    pub fn validate(&self) -> Result<(), LingquPtoUbGmError> {
        if self.abi_version != LINGQU_PTO_DISPATCH_ABI_V2
            || self.struct_bytes < size_of_u32::<Self>()
            || self.request_id == 0
            || self.callable_id == 0
            || self.artifact_fingerprint == 0
            || self.requester_cna == 0
            || self.requester_cna > LINGQU_PTO_CNA_MAX
            || self.memref_count > LINGQU_PTO_MAX_MEMREFS
            || self.scalar_count > LINGQU_PTO_MAX_SCALARS
            || (self.memref_count != 0 && self.memref_table_iova == 0)
            || (self.scalar_count != 0 && self.scalar_table_iova == 0)
        {
            return Err(LingquPtoUbGmError::BadControlTable);
        }
        Ok(())
    }
}

#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct LingquShmemMemrefV1 {
    pub abi_version: u32,
    pub struct_bytes: u32,
    pub opaque_mapping_ref: u64,
    pub ub_gm_addr: u64,
    pub byte_offset: u64,
    pub byte_length: u64,
    pub shape_table_iova: u64,
    pub stride_table_iova: u64,
    pub arg_index: u32,
    pub rank: u32,
    pub dtype: u16,
    pub role: u8,
    pub access: u8,
    pub flags: u32,
    pub reserved0: u32,
    pub reserved1: u32,
}

impl LingquShmemMemrefV1 {
    pub fn validate(&self) -> Result<(), LingquPtoUbGmError> {
        let role_access_valid = matches!(
            (self.role, self.access),
            (x, LINGQU_PTO_UB_GM_READ) if x == LingquPtoMemrefRole::Input as u8
        ) || matches!(
            (self.role, self.access),
            (x, LINGQU_PTO_UB_GM_WRITE) if x == LingquPtoMemrefRole::Output as u8
        ) || matches!(
            (self.role, self.access),
            (x, LINGQU_PTO_UB_GM_READ_WRITE) if x == LingquPtoMemrefRole::InOut as u8
        );
        if self.abi_version != LINGQU_SHMEM_MEMREF_ABI_V1
            || self.struct_bytes < size_of_u32::<Self>()
            || self.opaque_mapping_ref == 0
            || self.ub_gm_addr == 0
            || self.byte_length == 0
            || self.shape_table_iova == 0
            || self.stride_table_iova == 0
            || self.arg_index >= LINGQU_PTO_MAX_MEMREFS
            || self.rank == 0
            || self.rank > LINGQU_PTO_MAX_RANK
            || self.dtype > LINGQU_PTO_DTYPE_MAX
            || self.flags != 0
            || self.reserved0 != 0
            || self.reserved1 != 0
            || !role_access_valid
            || self.byte_offset.checked_add(self.byte_length).is_none()
            || self.ub_gm_addr.checked_add(self.byte_length).is_none()
        {
            return Err(LingquPtoUbGmError::BadMemref);
        }
        Ok(())
    }
}

#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct LingquPtoScalarV1 {
    pub abi_version: u32,
    pub struct_bytes: u32,
    pub arg_index: u32,
    pub dtype: u16,
    pub flags: u16,
    pub value: u64,
}

impl LingquPtoScalarV1 {
    pub fn validate(&self) -> Result<(), LingquPtoUbGmError> {
        if self.abi_version != LINGQU_PTO_SCALAR_ABI_V1
            || self.struct_bytes < size_of_u32::<Self>()
            || self.arg_index >= LINGQU_PTO_MAX_MEMREFS + LINGQU_PTO_MAX_SCALARS
            || self.dtype > LINGQU_PTO_DTYPE_MAX
            || self.flags != 0
        {
            return Err(LingquPtoUbGmError::BadControlTable);
        }
        Ok(())
    }
}

#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct PtoSimUbGmBindingV1 {
    pub request_id: u64,
    pub binding_id: u64,
    pub aperture_base: u64,
    pub aperture_length: u64,
    pub ub_gm_base: u64,
    pub mapped_length: u64,
    pub access: u32,
    pub flags: u32,
    pub backend_cookie: u64,
}

impl PtoSimUbGmBindingV1 {
    pub fn validate(&self) -> Result<(), LingquPtoUbGmError> {
        if self.request_id == 0
            || self.binding_id == 0
            || self.aperture_base == 0
            || self.aperture_length == 0
            || self.ub_gm_base == 0
            || self.mapped_length < self.aperture_length
            || !matches!(
                self.access,
                x if x == u32::from(LINGQU_PTO_UB_GM_READ)
                    || x == u32::from(LINGQU_PTO_UB_GM_WRITE)
                    || x == u32::from(LINGQU_PTO_UB_GM_READ_WRITE)
            )
            || self.flags != 0
            || self
                .aperture_base
                .checked_add(self.aperture_length)
                .is_none()
            || self.ub_gm_base.checked_add(self.aperture_length).is_none()
        {
            return Err(LingquPtoUbGmError::BadMemref);
        }
        Ok(())
    }
}

#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct PtoSimUbGmAuthorizedMemrefV1 {
    pub memref: LingquShmemMemrefV1,
    pub binding: PtoSimUbGmBindingV1,
    pub shape: [u32; LINGQU_PTO_MAX_RANK as usize],
    pub strides: [u32; LINGQU_PTO_MAX_RANK as usize],
    pub reserved: u64,
}

pub fn materialize_authorized_dispatch_args(
    control: &LingquPtoDispatchControlV2,
    memrefs: &[PtoSimUbGmAuthorizedMemrefV1],
    scalars: &[LingquPtoScalarV1],
    expected_callable_id: u64,
    expected_artifact_fingerprint: u64,
    registered_pto_device_cna: u32,
) -> Result<Vec<SimplerRuntimeArg>, LingquPtoUbGmError> {
    control.validate()?;
    if control.callable_id != expected_callable_id
        || control.artifact_fingerprint != expected_artifact_fingerprint
    {
        return Err(LingquPtoUbGmError::UnsupportedCallable);
    }
    if control.requester_cna != registered_pto_device_cna {
        return Err(LingquPtoUbGmError::AccessDenied);
    }
    if usize::try_from(control.memref_count).ok() != Some(memrefs.len())
        || usize::try_from(control.scalar_count).ok() != Some(scalars.len())
    {
        return Err(LingquPtoUbGmError::BadControlTable);
    }
    for authorized in memrefs {
        authorized.memref.validate()?;
        authorized.binding.validate()?;
        if authorized.reserved != 0 {
            return Err(LingquPtoUbGmError::BadMemref);
        }
    }
    for scalar in scalars {
        scalar.validate()?;
    }
    if authorized_metadata_crc32(control, memrefs, scalars)? != control.metadata_crc32 {
        return Err(LingquPtoUbGmError::BadControlTable);
    }

    let total_arg_count = memrefs
        .len()
        .checked_add(scalars.len())
        .ok_or(LingquPtoUbGmError::BadControlTable)?;
    let mut args = vec![None; total_arg_count];
    let mut binding_ids = HashSet::new();
    let mut aperture_ranges = Vec::with_capacity(memrefs.len());
    for authorized in memrefs {
        let memref = &authorized.memref;
        let binding = &authorized.binding;
        let arg_index =
            usize::try_from(memref.arg_index).map_err(|_| LingquPtoUbGmError::BadMemref)?;
        if arg_index >= memrefs.len()
            || args[arg_index].is_some()
            || binding.request_id != control.request_id
            || binding.backend_cookie != memref.opaque_mapping_ref
            || binding.access != u32::from(memref.access)
            || !binding_ids.insert(binding.binding_id)
        {
            return Err(LingquPtoUbGmError::BadMemref);
        }
        let aperture_end = binding
            .aperture_base
            .checked_add(binding.aperture_length)
            .ok_or(LingquPtoUbGmError::BadMemref)?;
        if aperture_ranges
            .iter()
            .any(|(start, end)| binding.aperture_base < *end && *start < aperture_end)
        {
            return Err(LingquPtoUbGmError::BadMemref);
        }
        aperture_ranges.push((binding.aperture_base, aperture_end));

        let rank = usize::try_from(memref.rank).map_err(|_| LingquPtoUbGmError::BadMemref)?;
        if authorized.shape[rank..].iter().any(|value| *value != 0)
            || authorized.strides[rank..].iter().any(|value| *value != 0)
        {
            return Err(LingquPtoUbGmError::BadMemref);
        }
        validate_contiguous_shape(
            &authorized.shape[..rank],
            &authorized.strides[..rank],
            memref.dtype,
            memref.byte_length,
        )?;
        let view_address = memref
            .ub_gm_addr
            .checked_add(memref.byte_offset)
            .ok_or(LingquPtoUbGmError::BadMemref)?;
        let aperture_offset = view_address
            .checked_sub(binding.ub_gm_base)
            .ok_or(LingquPtoUbGmError::BadMemref)?;
        let view_end = aperture_offset
            .checked_add(memref.byte_length)
            .ok_or(LingquPtoUbGmError::BadMemref)?;
        if view_end > binding.aperture_length || view_end > binding.mapped_length {
            return Err(LingquPtoUbGmError::BadMemref);
        }
        let (access, usage) = access_and_usage(memref)?;
        args[arg_index] = Some(SimplerRuntimeArg::UbGmMemref {
            binding: SimplerUbGmBinding {
                request_id: binding.request_id,
                binding_id: binding.binding_id,
                aperture_base: binding.aperture_base,
                aperture_length: binding.aperture_length,
                ub_gm_base: binding.ub_gm_base,
                mapped_length: binding.mapped_length,
                access,
                flags: binding.flags,
                backend_cookie: binding.backend_cookie,
            },
            view: SimplerUbGmView {
                aperture_offset,
                byte_length: memref.byte_length,
                shape: authorized.shape[..rank].to_vec(),
                strides: authorized.strides[..rank].to_vec(),
                dtype: memref.dtype,
            },
            usage,
        });
    }
    for scalar in scalars {
        let arg_index =
            usize::try_from(scalar.arg_index).map_err(|_| LingquPtoUbGmError::BadControlTable)?;
        if arg_index < memrefs.len() || arg_index >= total_arg_count || args[arg_index].is_some() {
            return Err(LingquPtoUbGmError::BadControlTable);
        }
        args[arg_index] = Some(SimplerRuntimeArg::ScalarU64(scalar.value));
    }
    args.into_iter()
        .collect::<Option<Vec<_>>>()
        .ok_or(LingquPtoUbGmError::BadControlTable)
}

fn access_and_usage(
    memref: &LingquShmemMemrefV1,
) -> Result<(SimplerUbGmAccess, BufferUsage), LingquPtoUbGmError> {
    match (memref.role, memref.access) {
        (role, LINGQU_PTO_UB_GM_READ) if role == LingquPtoMemrefRole::Input as u8 => {
            Ok((SimplerUbGmAccess::Read, BufferUsage::Input))
        }
        (role, LINGQU_PTO_UB_GM_WRITE) if role == LingquPtoMemrefRole::Output as u8 => {
            Ok((SimplerUbGmAccess::Write, BufferUsage::Output))
        }
        (role, LINGQU_PTO_UB_GM_READ_WRITE) if role == LingquPtoMemrefRole::InOut as u8 => {
            Ok((SimplerUbGmAccess::ReadWrite, BufferUsage::Inout))
        }
        _ => Err(LingquPtoUbGmError::BadMemref),
    }
}

fn validate_contiguous_shape(
    shape: &[u32],
    strides: &[u32],
    dtype: u16,
    byte_length: u64,
) -> Result<(), LingquPtoUbGmError> {
    if shape.is_empty() || shape.len() != strides.len() || shape.iter().any(|dim| *dim == 0) {
        return Err(LingquPtoUbGmError::BadMemref);
    }
    let mut expected_stride = 1u64;
    for (dim, stride) in shape.iter().zip(strides).rev() {
        if u64::from(*stride) != expected_stride {
            return Err(LingquPtoUbGmError::BadMemref);
        }
        expected_stride = expected_stride
            .checked_mul(u64::from(*dim))
            .ok_or(LingquPtoUbGmError::BadMemref)?;
    }
    let expected_bytes = expected_stride
        .checked_mul(dtype_bytes(dtype).ok_or(LingquPtoUbGmError::BadMemref)?)
        .ok_or(LingquPtoUbGmError::BadMemref)?;
    if expected_bytes != byte_length {
        return Err(LingquPtoUbGmError::BadMemref);
    }
    Ok(())
}

fn dtype_bytes(dtype: u16) -> Option<u64> {
    match dtype {
        0 | 2 | 10 => Some(4),
        1 | 3 | 6 | 9 => Some(2),
        4 | 5 | 11 | 12 | 13 | 14 => Some(1),
        7 | 8 => Some(8),
        _ => None,
    }
}

pub fn authorized_metadata_crc32(
    control: &LingquPtoDispatchControlV2,
    memrefs: &[PtoSimUbGmAuthorizedMemrefV1],
    scalars: &[LingquPtoScalarV1],
) -> Result<u32, LingquPtoUbGmError> {
    if usize::try_from(control.memref_count).ok() != Some(memrefs.len())
        || usize::try_from(control.scalar_count).ok() != Some(scalars.len())
    {
        return Err(LingquPtoUbGmError::BadControlTable);
    }
    let mut bytes = Vec::with_capacity(
        std::mem::size_of::<LingquPtoDispatchControlV2>()
            + memrefs.len() * std::mem::size_of::<LingquShmemMemrefV1>()
            + scalars.len() * std::mem::size_of::<LingquPtoScalarV1>()
            + memrefs.len() * 2 * LINGQU_PTO_MAX_RANK as usize * std::mem::size_of::<u32>(),
    );
    append_control_for_crc(&mut bytes, control);
    for authorized in memrefs {
        append_memref_for_crc(&mut bytes, &authorized.memref);
    }
    for scalar in scalars {
        append_scalar_for_crc(&mut bytes, scalar);
    }
    for authorized in memrefs {
        let rank =
            usize::try_from(authorized.memref.rank).map_err(|_| LingquPtoUbGmError::BadMemref)?;
        if rank == 0 || rank > LINGQU_PTO_MAX_RANK as usize {
            return Err(LingquPtoUbGmError::BadMemref);
        }
        for value in &authorized.shape[..rank] {
            bytes.extend_from_slice(&value.to_le_bytes());
        }
        for value in &authorized.strides[..rank] {
            bytes.extend_from_slice(&value.to_le_bytes());
        }
    }
    Ok(crc32_ieee(&bytes))
}

fn append_control_for_crc(bytes: &mut Vec<u8>, control: &LingquPtoDispatchControlV2) {
    bytes.extend_from_slice(&control.abi_version.to_le_bytes());
    bytes.extend_from_slice(&control.struct_bytes.to_le_bytes());
    bytes.extend_from_slice(&control.request_id.to_le_bytes());
    bytes.extend_from_slice(&control.callable_id.to_le_bytes());
    bytes.extend_from_slice(&control.memref_count.to_le_bytes());
    bytes.extend_from_slice(&control.scalar_count.to_le_bytes());
    bytes.extend_from_slice(&control.memref_table_iova.to_le_bytes());
    bytes.extend_from_slice(&control.scalar_table_iova.to_le_bytes());
    bytes.extend_from_slice(&control.artifact_fingerprint.to_le_bytes());
    bytes.extend_from_slice(&0u32.to_le_bytes());
    bytes.extend_from_slice(&control.requester_cna.to_le_bytes());
}

fn append_memref_for_crc(bytes: &mut Vec<u8>, memref: &LingquShmemMemrefV1) {
    bytes.extend_from_slice(&memref.abi_version.to_le_bytes());
    bytes.extend_from_slice(&memref.struct_bytes.to_le_bytes());
    bytes.extend_from_slice(&memref.opaque_mapping_ref.to_le_bytes());
    bytes.extend_from_slice(&memref.ub_gm_addr.to_le_bytes());
    bytes.extend_from_slice(&memref.byte_offset.to_le_bytes());
    bytes.extend_from_slice(&memref.byte_length.to_le_bytes());
    bytes.extend_from_slice(&memref.shape_table_iova.to_le_bytes());
    bytes.extend_from_slice(&memref.stride_table_iova.to_le_bytes());
    bytes.extend_from_slice(&memref.arg_index.to_le_bytes());
    bytes.extend_from_slice(&memref.rank.to_le_bytes());
    bytes.extend_from_slice(&memref.dtype.to_le_bytes());
    bytes.push(memref.role);
    bytes.push(memref.access);
    bytes.extend_from_slice(&memref.flags.to_le_bytes());
    bytes.extend_from_slice(&memref.reserved0.to_le_bytes());
    bytes.extend_from_slice(&memref.reserved1.to_le_bytes());
}

fn append_scalar_for_crc(bytes: &mut Vec<u8>, scalar: &LingquPtoScalarV1) {
    bytes.extend_from_slice(&scalar.abi_version.to_le_bytes());
    bytes.extend_from_slice(&scalar.struct_bytes.to_le_bytes());
    bytes.extend_from_slice(&scalar.arg_index.to_le_bytes());
    bytes.extend_from_slice(&scalar.dtype.to_le_bytes());
    bytes.extend_from_slice(&scalar.flags.to_le_bytes());
    bytes.extend_from_slice(&scalar.value.to_le_bytes());
}

fn crc32_ieee(bytes: &[u8]) -> u32 {
    let mut crc = u32::MAX;
    for byte in bytes {
        crc ^= u32::from(*byte);
        for _ in 0..8 {
            crc = (crc >> 1) ^ (0xedb8_8320 & 0u32.wrapping_sub(crc & 1));
        }
    }
    !crc
}

pub type PtoSimUbGmReadV1 = unsafe extern "C" fn(
    qemu_context: *mut c_void,
    request_id: u64,
    binding_id: u64,
    ub_gm_addr: u64,
    dst: *mut c_void,
    length: u64,
) -> c_int;

pub type PtoSimUbGmWriteV1 = unsafe extern "C" fn(
    qemu_context: *mut c_void,
    request_id: u64,
    binding_id: u64,
    ub_gm_addr: u64,
    src: *const c_void,
    length: u64,
) -> c_int;

pub type PtoSimUbGmFenceV1 = unsafe extern "C" fn(
    qemu_context: *mut c_void,
    request_id: u64,
    binding_id: u64,
    ub_gm_addr: u64,
    length: u64,
    flags: u32,
) -> c_int;

#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct PtoSimUbGmAccessOpsV1 {
    pub abi_version: u32,
    pub struct_bytes: u32,
    pub read: Option<PtoSimUbGmReadV1>,
    pub write: Option<PtoSimUbGmWriteV1>,
    pub fence: Option<PtoSimUbGmFenceV1>,
}

impl PtoSimUbGmAccessOpsV1 {
    pub fn validate(&self) -> Result<(), LingquPtoUbGmError> {
        if self.abi_version != PTO_SIM_UB_GM_ACCESS_ABI_V1
            || self.struct_bytes < size_of_u32::<Self>()
            || self.read.is_none()
            || self.write.is_none()
            || self.fence.is_none()
        {
            return Err(LingquPtoUbGmError::CallbackFailed);
        }
        Ok(())
    }
}

#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct LingquPtoUbGmCountersV1 {
    pub abi_version: u32,
    pub struct_bytes: u32,
    pub request_id: u64,
    pub h2d_bytes: u64,
    pub d2h_bytes: u64,
    pub segment_payload_staging_bytes: u64,
    pub pto_ub_gm_read_bytes: u64,
    pub pto_ub_gm_write_bytes: u64,
    pub qemu_ub_gm_load_bytes: u64,
    pub qemu_ub_gm_store_bytes: u64,
    pub qemu_ub_gm_fence_count: u64,
    pub producer_verify: u32,
    pub last_error: u32,
    pub reserved: u64,
}

const fn size_of_u32<T>() -> u32 {
    std::mem::size_of::<T>() as u32
}

const _: () = assert!(std::mem::size_of::<LingquPtoDispatchControlV2>() == 64);
const _: () = assert!(std::mem::size_of::<LingquShmemMemrefV1>() == 80);
const _: () = assert!(std::mem::size_of::<LingquPtoScalarV1>() == 24);
const _: () = assert!(std::mem::size_of::<PtoSimUbGmBindingV1>() == 64);
const _: () = assert!(std::mem::size_of::<PtoSimUbGmAuthorizedMemrefV1>() == 192);
const _: () = assert!(std::mem::size_of::<PtoSimUbGmAccessOpsV1>() == 32);
const _: () = assert!(std::mem::size_of::<LingquPtoUbGmCountersV1>() == 96);

#[cfg(test)]
mod tests {
    use super::*;

    unsafe extern "C" fn read(
        _: *mut c_void,
        _: u64,
        _: u64,
        _: u64,
        _: *mut c_void,
        _: u64,
    ) -> c_int {
        0
    }

    unsafe extern "C" fn write(
        _: *mut c_void,
        _: u64,
        _: u64,
        _: u64,
        _: *const c_void,
        _: u64,
    ) -> c_int {
        0
    }

    unsafe extern "C" fn fence(_: *mut c_void, _: u64, _: u64, _: u64, _: u64, _: u32) -> c_int {
        0
    }

    fn valid_control() -> LingquPtoDispatchControlV2 {
        LingquPtoDispatchControlV2 {
            abi_version: LINGQU_PTO_DISPATCH_ABI_V2,
            struct_bytes: size_of_u32::<LingquPtoDispatchControlV2>(),
            request_id: 42,
            callable_id: 7,
            memref_count: 1,
            scalar_count: 1,
            memref_table_iova: 0x1000,
            scalar_table_iova: 0x2000,
            artifact_fingerprint: 0x1234,
            metadata_crc32: 0xabcd,
            requester_cna: 1,
        }
    }

    fn valid_memref() -> LingquShmemMemrefV1 {
        LingquShmemMemrefV1 {
            abi_version: LINGQU_SHMEM_MEMREF_ABI_V1,
            struct_bytes: size_of_u32::<LingquShmemMemrefV1>(),
            opaque_mapping_ref: 3,
            ub_gm_addr: 0x4000,
            byte_offset: 64,
            byte_length: 4096,
            shape_table_iova: 0x5000,
            stride_table_iova: 0x6000,
            arg_index: 0,
            rank: 1,
            dtype: 0,
            role: LingquPtoMemrefRole::Input as u8,
            access: LINGQU_PTO_UB_GM_READ,
            flags: 0,
            reserved0: 0,
            reserved1: 0,
        }
    }

    fn valid_authorized_memref() -> PtoSimUbGmAuthorizedMemrefV1 {
        PtoSimUbGmAuthorizedMemrefV1 {
            memref: valid_memref(),
            binding: PtoSimUbGmBindingV1 {
                request_id: 42,
                binding_id: 1,
                aperture_base: 0x7000_0000_0000,
                aperture_length: 8192,
                ub_gm_base: 0x4000,
                mapped_length: 8192,
                access: u32::from(LINGQU_PTO_UB_GM_READ),
                flags: 0,
                backend_cookie: 3,
            },
            shape: [1024, 0, 0, 0, 0],
            strides: [1, 0, 0, 0, 0],
            reserved: 0,
        }
    }

    #[test]
    fn layouts_match_the_c_abi() {
        assert_eq!(size_of_u32::<LingquPtoDispatchControlV2>(), 64);
        assert_eq!(size_of_u32::<LingquShmemMemrefV1>(), 80);
        assert_eq!(size_of_u32::<LingquPtoScalarV1>(), 24);
        assert_eq!(size_of_u32::<PtoSimUbGmBindingV1>(), 64);
        assert_eq!(size_of_u32::<PtoSimUbGmAuthorizedMemrefV1>(), 192);
        assert_eq!(size_of_u32::<PtoSimUbGmAccessOpsV1>(), 32);
        assert_eq!(size_of_u32::<LingquPtoUbGmCountersV1>(), 96);
        assert_eq!(
            std::mem::offset_of!(LingquPtoDispatchControlV2, request_id),
            8
        );
        assert_eq!(std::mem::offset_of!(LingquShmemMemrefV1, arg_index), 56);
    }

    #[test]
    fn malformed_control_metadata_fails_deterministically() {
        let mut control = valid_control();
        assert_eq!(control.validate(), Ok(()));
        control.abi_version = 99;
        assert_eq!(control.validate(), Err(LingquPtoUbGmError::BadControlTable));
        control = valid_control();
        control.memref_table_iova = 0;
        assert_eq!(control.validate(), Err(LingquPtoUbGmError::BadControlTable));
    }

    #[test]
    fn malformed_memrefs_fail_closed() {
        let mut memref = valid_memref();
        assert_eq!(memref.validate(), Ok(()));
        memref.byte_offset = u64::MAX;
        assert_eq!(memref.validate(), Err(LingquPtoUbGmError::BadMemref));
        memref = valid_memref();
        memref.access = LINGQU_PTO_UB_GM_WRITE;
        assert_eq!(memref.validate(), Err(LingquPtoUbGmError::BadMemref));
    }

    #[test]
    fn callback_table_requires_the_complete_v1_contract() {
        let mut ops = PtoSimUbGmAccessOpsV1 {
            abi_version: PTO_SIM_UB_GM_ACCESS_ABI_V1,
            struct_bytes: size_of_u32::<PtoSimUbGmAccessOpsV1>(),
            read: Some(read),
            write: Some(write),
            fence: Some(fence),
        };
        assert_eq!(ops.validate(), Ok(()));
        ops.fence = None;
        assert_eq!(ops.validate(), Err(LingquPtoUbGmError::CallbackFailed));
    }

    #[test]
    fn stable_error_codes_match_the_completion_contract() {
        assert_eq!(
            LingquPtoUbGmError::BadControlTable.code(),
            "pto_ub_gm_bad_control_table"
        );
        assert_eq!(
            LingquPtoUbGmError::CallbackFailed.code(),
            "pto_ub_gm_callback_failed"
        );
        assert_eq!(
            LingquPtoUbGmError::AuthorizationCancelled.code(),
            "pto_ub_gm_authorization_cancelled"
        );
        assert_eq!(LingquPtoUbGmError::AuthorizationCancelled.ffi_status(), -9);
    }

    #[test]
    fn ieee_crc_matches_the_standard_check_value() {
        assert_eq!(crc32_ieee(b"123456789"), 0xcbf4_3926);
    }

    #[test]
    fn authorized_metadata_materializes_without_payload_bytes() {
        let authorized = valid_authorized_memref();
        let mut control = valid_control();
        control.scalar_count = 0;
        control.scalar_table_iova = 0;
        control.metadata_crc32 =
            authorized_metadata_crc32(&control, &[authorized], &[]).expect("metadata crc");
        let args = materialize_authorized_dispatch_args(
            &control,
            &[authorized],
            &[],
            control.callable_id,
            control.artifact_fingerprint,
            control.requester_cna,
        )
        .expect("authorized args");
        assert_eq!(args.len(), 1);
        let SimplerRuntimeArg::UbGmMemref {
            binding,
            view,
            usage,
        } = &args[0]
        else {
            panic!("expected UB_GM memref");
        };
        assert_eq!(binding.request_id, control.request_id);
        assert_eq!(view.aperture_offset, 64);
        assert_eq!(view.byte_length, 4096);
        assert_eq!(*usage, BufferUsage::Input);
    }

    #[test]
    fn authorized_metadata_rejects_crc_identity_and_requester_mismatch() {
        let authorized = valid_authorized_memref();
        let mut control = valid_control();
        control.scalar_count = 0;
        control.scalar_table_iova = 0;
        control.metadata_crc32 =
            authorized_metadata_crc32(&control, &[authorized], &[]).expect("metadata crc");
        assert_eq!(
            materialize_authorized_dispatch_args(
                &control,
                &[authorized],
                &[],
                control.callable_id,
                control.artifact_fingerprint ^ 1,
                control.requester_cna,
            ),
            Err(LingquPtoUbGmError::UnsupportedCallable)
        );
        assert_eq!(
            materialize_authorized_dispatch_args(
                &control,
                &[authorized],
                &[],
                control.callable_id,
                control.artifact_fingerprint,
                control.requester_cna + 1,
            ),
            Err(LingquPtoUbGmError::AccessDenied)
        );
        control.metadata_crc32 ^= 1;
        assert_eq!(
            materialize_authorized_dispatch_args(
                &control,
                &[authorized],
                &[],
                control.callable_id,
                control.artifact_fingerprint,
                control.requester_cna,
            ),
            Err(LingquPtoUbGmError::BadControlTable)
        );
    }

    #[test]
    fn authorized_metadata_rejects_overlapping_apertures() {
        let first = valid_authorized_memref();
        let mut second = first;
        second.memref.arg_index = 1;
        second.memref.opaque_mapping_ref = 4;
        second.binding.binding_id = 2;
        second.binding.backend_cookie = 4;
        second.binding.ub_gm_base = 0x8000;
        let mut control = valid_control();
        control.memref_count = 2;
        control.scalar_count = 0;
        control.scalar_table_iova = 0;
        control.metadata_crc32 =
            authorized_metadata_crc32(&control, &[first, second], &[]).expect("metadata crc");
        assert_eq!(
            materialize_authorized_dispatch_args(
                &control,
                &[first, second],
                &[],
                control.callable_id,
                control.artifact_fingerprint,
                control.requester_cna,
            ),
            Err(LingquPtoUbGmError::BadMemref)
        );
    }
}
