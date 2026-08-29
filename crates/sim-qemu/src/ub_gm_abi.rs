use std::ffi::c_void;
use std::os::raw::c_int;

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
            || self.arg_index >= LINGQU_PTO_MAX_SCALARS
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

    #[test]
    fn layouts_match_the_c_abi() {
        assert_eq!(size_of_u32::<LingquPtoDispatchControlV2>(), 64);
        assert_eq!(size_of_u32::<LingquShmemMemrefV1>(), 80);
        assert_eq!(size_of_u32::<LingquPtoScalarV1>(), 24);
        assert_eq!(size_of_u32::<PtoSimUbGmBindingV1>(), 64);
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
    }
}
