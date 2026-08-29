//! Thin Rust-side runtime loader for `simpler` host `pto_runtime_c_api`.
//!
//! Current vendored `simpler` exposes the HostBuildGraph runtime through the
//! worker C API: initialize a device context with `simpler_init`, register a
//! `ChipCallable`, then launch it via `simpler_run`.

use std::alloc::{alloc_zeroed, dealloc, Layout};
use std::ffi::{c_int, c_void};
use std::fs;
use std::path::{Path, PathBuf};
use std::ptr::NonNull;
use std::sync::atomic::{AtomicU64, Ordering};
use std::time::{SystemTime, UNIX_EPOCH};

use libloading::os::unix::{Library, Symbol, RTLD_GLOBAL, RTLD_LOCAL, RTLD_NOW};
use thiserror::Error;

pub type RuntimeHandle = *mut c_void;
pub type DeviceContextHandle = *mut c_void;

type CreateDeviceContextFn = unsafe extern "C" fn() -> DeviceContextHandle;
type DestroyDeviceContextFn = unsafe extern "C" fn(DeviceContextHandle);
type GetRuntimeSizeFn = unsafe extern "C" fn() -> usize;
type GetRuntimeAlignmentFn = unsafe extern "C" fn() -> usize;
type DeviceMallocCtxFn = unsafe extern "C" fn(DeviceContextHandle, usize) -> *mut c_void;
type DeviceFreeCtxFn = unsafe extern "C" fn(DeviceContextHandle, *mut c_void);
type CopyToDeviceCtxFn =
    unsafe extern "C" fn(DeviceContextHandle, *mut c_void, *const c_void, usize) -> c_int;
type CopyFromDeviceCtxFn =
    unsafe extern "C" fn(DeviceContextHandle, *mut c_void, *const c_void, usize) -> c_int;
type SimplerInitFn = unsafe extern "C" fn(
    DeviceContextHandle,
    c_int,
    *const u8,
    usize,
    *const u8,
    usize,
    *const u8,
    usize,
    *const CallConfig,
) -> c_int;
type SimplerRegisterCallableFn =
    unsafe extern "C" fn(DeviceContextHandle, i32, *const c_void) -> c_int;
type SimplerPrepareRunFn = unsafe extern "C" fn(
    DeviceContextHandle,
    RuntimeHandle,
    i32,
    *const c_void,
    *const CallConfig,
    *const NativeRunDescriptor,
) -> c_int;
type SimplerBindPtoUbGmRunContextFn =
    unsafe extern "C" fn(DeviceContextHandle, RuntimeHandle, *const c_void) -> c_int;
type SimplerRunPhaseFn = unsafe extern "C" fn(DeviceContextHandle, RuntimeHandle) -> c_int;
type SimplerUnregisterCallableFn = unsafe extern "C" fn(DeviceContextHandle, i32) -> c_int;
type FinalizeDeviceFn = unsafe extern "C" fn(DeviceContextHandle) -> c_int;

#[repr(C, packed)]
#[derive(Clone, Copy)]
struct RuntimeEnv {
    ring_task_window: [u64; 4],
    ring_heap: [u64; 4],
    ring_dep_pool: [u64; 4],
}

#[repr(C, packed)]
#[derive(Clone, Copy)]
struct CallConfig {
    aicpu_thread_num: i32,
    enable_chip_swimlane: i32,
    enable_dump_args: i32,
    enable_pmu: i32,
    enable_dep_gen: i32,
    enable_scope_stats: i32,
    runtime_env: RuntimeEnv,
    output_prefix: [u8; 1024],
}

impl CallConfig {
    fn new(aicpu_thread_num: i32) -> Self {
        Self {
            aicpu_thread_num,
            enable_chip_swimlane: 0,
            enable_dump_args: 0,
            enable_pmu: 0,
            enable_dep_gen: 0,
            enable_scope_stats: 0,
            runtime_env: RuntimeEnv {
                ring_task_window: [0; 4],
                ring_heap: [0; 4],
                ring_dep_pool: [0; 4],
            },
            output_prefix: [0; 1024],
        }
    }
}

#[repr(C)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
struct NativeRunDescriptor {
    pipeline_slot: u32,
    arena_bank: u32,
    run_id: u64,
    generation: u64,
    dispatch_id: u64,
    run_epoch: u64,
    accepted_state: *mut i32,
    accepted_value: i32,
}

static NEXT_NATIVE_RUN_EPOCH: AtomicU64 = AtomicU64::new(1);

pub type PtoUbGmReadFn = unsafe extern "C" fn(
    backend_context: *mut c_void,
    request_id: u64,
    binding_id: u64,
    ub_gm_addr: u64,
    dst: *mut c_void,
    length: u64,
) -> c_int;

pub type PtoUbGmWriteFn = unsafe extern "C" fn(
    backend_context: *mut c_void,
    request_id: u64,
    binding_id: u64,
    ub_gm_addr: u64,
    src: *const c_void,
    length: u64,
) -> c_int;

pub type PtoUbGmFenceFn = unsafe extern "C" fn(
    backend_context: *mut c_void,
    request_id: u64,
    binding_id: u64,
    ub_gm_addr: u64,
    length: u64,
    flags: u32,
) -> c_int;

#[repr(C)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct PtoUbGmBinding {
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

#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct PtoUbGmAccessOps {
    pub read: PtoUbGmReadFn,
    pub write: PtoUbGmWriteFn,
    pub fence: PtoUbGmFenceFn,
}

#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct PtoUbGmRunContext {
    request_id: u64,
    bindings: *const PtoUbGmBinding,
    binding_count: usize,
    ops: *const PtoUbGmAccessOps,
    backend_context: *mut c_void,
}

impl PtoUbGmRunContext {
    pub fn new(
        request_id: u64,
        bindings: &[PtoUbGmBinding],
        ops: &PtoUbGmAccessOps,
        backend_context: *mut c_void,
    ) -> Result<Self, SimplerApiError> {
        if request_id == 0 || bindings.is_empty() || backend_context.is_null() {
            return Err(SimplerApiError::InvalidUbGmRunContext);
        }
        Ok(Self {
            request_id,
            bindings: bindings.as_ptr(),
            binding_count: bindings.len(),
            ops,
            backend_context,
        })
    }

    fn as_ptr(&self) -> *const c_void {
        self as *const Self as *const c_void
    }
}

fn next_native_run_descriptor() -> NativeRunDescriptor {
    let run_epoch = NEXT_NATIVE_RUN_EPOCH.fetch_add(1, Ordering::Relaxed);
    assert_ne!(run_epoch, 0, "simpler native-run epoch exhausted");
    NativeRunDescriptor {
        pipeline_slot: 0,
        arena_bank: 0,
        run_id: run_epoch,
        generation: 1,
        dispatch_id: run_epoch,
        run_epoch,
        accepted_state: std::ptr::null_mut(),
        accepted_value: 0,
    }
}

#[repr(i32)]
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ArgDirection {
    Scalar = 0,
    In = 1,
    Out = 2,
    Inout = 3,
}

#[repr(u8)]
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum DataType {
    Float32 = 0,
    Float16 = 1,
    Int32 = 2,
    Int16 = 3,
    Int8 = 4,
    Uint8 = 5,
    Bfloat16 = 6,
    Int64 = 7,
    Uint64 = 8,
    Uint16 = 9,
    Uint32 = 10,
    Bool = 11,
    Fp8E4M3Fn = 12,
    Fp8E8M0 = 13,
    Fp4E2M1 = 14,
}

impl DataType {
    pub fn element_size(self) -> usize {
        match self {
            Self::Float32 | Self::Int32 | Self::Uint32 => 4,
            Self::Float16 | Self::Int16 | Self::Bfloat16 | Self::Uint16 => 2,
            Self::Int8
            | Self::Uint8
            | Self::Bool
            | Self::Fp8E4M3Fn
            | Self::Fp8E8M0
            | Self::Fp4E2M1 => 1,
            Self::Int64 | Self::Uint64 => 8,
        }
    }
}

impl TryFrom<u16> for DataType {
    type Error = SimplerApiError;

    fn try_from(value: u16) -> Result<Self, Self::Error> {
        match value {
            0 => Ok(Self::Float32),
            1 => Ok(Self::Float16),
            2 => Ok(Self::Int32),
            3 => Ok(Self::Int16),
            4 => Ok(Self::Int8),
            5 => Ok(Self::Uint8),
            6 => Ok(Self::Bfloat16),
            7 => Ok(Self::Int64),
            8 => Ok(Self::Uint64),
            9 => Ok(Self::Uint16),
            10 => Ok(Self::Uint32),
            11 => Ok(Self::Bool),
            12 => Ok(Self::Fp8E4M3Fn),
            13 => Ok(Self::Fp8E8M0),
            14 => Ok(Self::Fp4E2M1),
            _ => Err(SimplerApiError::InvalidDataType(value)),
        }
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct DevicePtr(NonNull<c_void>);

impl DevicePtr {
    pub fn as_ptr(self) -> *mut c_void {
        self.0.as_ptr()
    }

    pub fn from_raw(ptr: *mut c_void) -> Option<Self> {
        NonNull::new(ptr).map(Self)
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct OwnedRuntime {
    ptr: NonNull<c_void>,
}

#[repr(u8)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum AddressSpace {
    Host = 0,
    Device = 1,
    UbGm = 2,
}

impl From<bool> for AddressSpace {
    fn from(device_memory: bool) -> Self {
        if device_memory {
            Self::Device
        } else {
            Self::Host
        }
    }
}

impl OwnedRuntime {
    pub fn as_raw(self) -> RuntimeHandle {
        self.ptr.as_ptr()
    }
}

#[repr(C, align(64))]
#[derive(Clone, Copy, Debug)]
pub struct Tensor {
    buffer_addr: u64,
    buffer_size: u64,
    owner_task_id: u64,
    start_offset: u64,
    version: i32,
    ndims: u32,
    dtype: DataType,
    manual_dep: u8,
    is_contiguous: u8,
    address_space: u8,
    shapes: [u32; 5],
    extent_elem_cache: u64,
    strides: [u32; 5],
    _pad_cl2: [u8; 36],
}

impl Tensor {
    pub fn new(data: u64, bytes: u64, dtype: DataType) -> Result<Self, SimplerApiError> {
        let elem_size = dtype.element_size() as u64;
        if elem_size == 0 || bytes % elem_size != 0 {
            return Err(SimplerApiError::InvalidTensorShape);
        }
        let elems = bytes / elem_size;
        let elems = u32::try_from(elems).map_err(|_| SimplerApiError::InvalidTensorShape)?;
        Self::from_shape(data, bytes, &[elems], dtype, false)
    }

    pub fn from_shape(
        data: u64,
        bytes: u64,
        shape: &[u32],
        dtype: DataType,
        device_memory: bool,
    ) -> Result<Self, SimplerApiError> {
        if shape.is_empty() || shape.len() > 5 || shape.contains(&0) {
            return Err(SimplerApiError::InvalidTensorShape);
        }
        let mut row_major_strides = vec![0u32; shape.len()];
        let mut elements = 1u64;
        for (index, dimension) in shape.iter().copied().enumerate().rev() {
            row_major_strides[index] =
                u32::try_from(elements).map_err(|_| SimplerApiError::InvalidTensorShape)?;
            elements = elements
                .checked_mul(u64::from(dimension))
                .ok_or(SimplerApiError::InvalidTensorShape)?;
        }
        Self::from_shape_and_strides(
            data,
            bytes,
            shape,
            &row_major_strides,
            dtype,
            AddressSpace::from(device_memory),
        )
    }

    pub fn from_ub_gm(
        aperture_addr: u64,
        bytes: u64,
        shape: &[u32],
        strides: &[u32],
        dtype: DataType,
    ) -> Result<Self, SimplerApiError> {
        if aperture_addr == 0 || bytes == 0 || aperture_addr.checked_add(bytes).is_none() {
            return Err(SimplerApiError::InvalidTensorShape);
        }
        Self::from_shape_and_strides(
            aperture_addr,
            bytes,
            shape,
            strides,
            dtype,
            AddressSpace::UbGm,
        )
    }

    fn from_shape_and_strides(
        data: u64,
        bytes: u64,
        shape: &[u32],
        strides: &[u32],
        dtype: DataType,
        address_space: AddressSpace,
    ) -> Result<Self, SimplerApiError> {
        if shape.is_empty()
            || shape.len() > 5
            || shape.len() != strides.len()
            || shape.contains(&0)
            || strides.contains(&0)
        {
            return Err(SimplerApiError::InvalidTensorShape);
        }

        let mut shapes = [0u32; 5];
        let mut tensor_strides = [0u32; 5];
        let mut extent_elements = 1u64;
        let mut expected_stride = 1u64;
        let mut contiguous = true;
        for index in (0..shape.len()).rev() {
            let dimension = u64::from(shape[index]);
            let stride = u64::from(strides[index]);
            shapes[index] = shape[index];
            tensor_strides[index] = strides[index];
            contiguous &= stride == expected_stride;
            extent_elements = extent_elements
                .checked_add(
                    (dimension - 1)
                        .checked_mul(stride)
                        .ok_or(SimplerApiError::InvalidTensorShape)?,
                )
                .ok_or(SimplerApiError::InvalidTensorShape)?;
            expected_stride = expected_stride
                .checked_mul(dimension)
                .ok_or(SimplerApiError::InvalidTensorShape)?;
        }
        let required_bytes = extent_elements
            .checked_mul(dtype.element_size() as u64)
            .ok_or(SimplerApiError::InvalidTensorShape)?;
        if required_bytes > bytes {
            return Err(SimplerApiError::InvalidTensorShape);
        }

        Ok(Self {
            buffer_addr: data,
            buffer_size: bytes,
            owner_task_id: u64::MAX,
            start_offset: 0,
            version: 0,
            ndims: shape.len() as u32,
            dtype,
            manual_dep: 0,
            is_contiguous: u8::from(contiguous),
            address_space: address_space as u8,
            shapes,
            extent_elem_cache: extent_elements,
            strides: tensor_strides,
            _pad_cl2: [0; 36],
        })
    }

    pub fn from_shape_with_child_memory(
        data: u64,
        bytes: u64,
        shape: &[u32],
        dtype: DataType,
        child_memory: bool,
    ) -> Result<Self, SimplerApiError> {
        Self::from_shape(data, bytes, shape, dtype, child_memory)
    }

    pub fn address_space(&self) -> AddressSpace {
        match self.address_space {
            0 => AddressSpace::Host,
            1 => AddressSpace::Device,
            2 => AddressSpace::UbGm,
            value => panic!("invalid Tensor address space {value}"),
        }
    }

    pub fn buffer_addr(&self) -> u64 {
        self.buffer_addr
    }

    pub fn buffer_size(&self) -> u64 {
        self.buffer_size
    }

    pub fn is_contiguous(&self) -> bool {
        self.is_contiguous != 0
    }
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct ChipStorageTaskArgs {
    tensors: [Tensor; CHIP_MAX_TENSOR_ARGS],
    scalars: [u64; CHIP_MAX_SCALAR_ARGS],
    tensor_count: i32,
    scalar_count: i32,
}

impl std::fmt::Debug for ChipStorageTaskArgs {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("ChipStorageTaskArgs")
            .field("tensor_count", &self.tensor_count)
            .field("scalar_count", &self.scalar_count)
            .finish()
    }
}

impl ChipStorageTaskArgs {
    pub fn new(tensors: &[Tensor], scalars: &[u64]) -> Result<Self, SimplerApiError> {
        if tensors.len() > CHIP_MAX_TENSOR_ARGS || scalars.len() > CHIP_MAX_SCALAR_ARGS {
            return Err(SimplerApiError::TooManyArgs);
        }
        let zero_tensor = Tensor {
            buffer_addr: 0,
            buffer_size: 0,
            owner_task_id: u64::MAX,
            start_offset: 0,
            version: 0,
            ndims: 0,
            dtype: DataType::Uint8,
            manual_dep: 0,
            is_contiguous: 0,
            address_space: 0,
            shapes: [0; 5],
            extent_elem_cache: 0,
            strides: [0; 5],
            _pad_cl2: [0; 36],
        };
        let mut out = Self {
            tensors: [zero_tensor; CHIP_MAX_TENSOR_ARGS],
            scalars: [0; CHIP_MAX_SCALAR_ARGS],
            tensor_count: tensors.len() as i32,
            scalar_count: scalars.len() as i32,
        };
        out.tensors[..tensors.len()].copy_from_slice(tensors);
        out.scalars[..scalars.len()].copy_from_slice(scalars);
        Ok(out)
    }

    pub fn tensor_count(&self) -> usize {
        self.tensor_count as usize
    }

    pub fn tensor(&self, index: usize) -> Option<&Tensor> {
        self.tensors
            .get(index)
            .filter(|_| index < self.tensor_count())
    }
}

pub struct KernelCallableInput<'a> {
    pub func_id: i32,
    pub binary: &'a [u8],
}

#[derive(Debug)]
pub struct CallableBuffer {
    storage: Vec<u8>,
    offset: usize,
    len: usize,
}

impl CallableBuffer {
    fn new(bytes: &[u8]) -> Self {
        let mut storage = vec![0u8; bytes.len() + CALLABLE_CHILD_ALIGN - 1];
        let address = storage.as_ptr() as usize;
        let offset = (CALLABLE_CHILD_ALIGN - address % CALLABLE_CHILD_ALIGN) % CALLABLE_CHILD_ALIGN;
        storage[offset..offset + bytes.len()].copy_from_slice(bytes);
        Self {
            storage,
            offset,
            len: bytes.len(),
        }
    }

    pub fn as_ptr(&self) -> *const c_void {
        unsafe { self.storage.as_ptr().add(self.offset) as *const c_void }
    }

    fn as_bytes(&self) -> &[u8] {
        &self.storage[self.offset..self.offset + self.len]
    }
}

impl Clone for CallableBuffer {
    fn clone(&self) -> Self {
        Self::new(self.as_bytes())
    }
}

pub fn make_chip_callable(
    orch_function_name: &str,
    orch_binary: &[u8],
    kernels: &[KernelCallableInput<'_>],
    signature: &[ArgDirection],
) -> Result<CallableBuffer, SimplerApiError> {
    if signature.len() > CHIP_MAX_TENSOR_ARGS || kernels.len() > CHIP_MAX_CHILDREN {
        return Err(SimplerApiError::TooManyArgs);
    }
    let mut child_buffers = Vec::with_capacity(kernels.len());
    for kernel in kernels {
        child_buffers.push(make_core_callable(kernel.binary)?);
    }

    let mut storage_size = orch_binary.len();
    let mut child_offsets = Vec::with_capacity(child_buffers.len());
    for child in &child_buffers {
        storage_size = align_up(storage_size, CALLABLE_ALIGN);
        child_offsets.push(storage_size as u32);
        storage_size += child.len();
    }

    let mut bytes = vec![0u8; CHIP_CALLABLE_HEADER_SIZE + storage_size];
    for (index, direction) in signature.iter().enumerate() {
        write_i32(&mut bytes, index * 4, *direction as i32);
    }
    write_i32(
        &mut bytes,
        CHIP_CALLABLE_SIG_COUNT_OFFSET,
        signature.len() as i32,
    );
    write_u32(
        &mut bytes,
        CHIP_CALLABLE_BINARY_SIZE_OFFSET,
        orch_binary.len() as u32,
    );
    write_cstr(
        &mut bytes,
        CHIP_CALLABLE_FUNC_NAME_OFFSET,
        CALLABLE_FUNC_NAME_MAX,
        orch_function_name,
    )?;
    write_u32(
        &mut bytes,
        CHIP_CALLABLE_FUNC_NAME_LEN_OFFSET,
        orch_function_name.len().min(CALLABLE_FUNC_NAME_MAX - 1) as u32,
    );
    for (index, kernel) in kernels.iter().enumerate() {
        write_i32(
            &mut bytes,
            CHIP_CALLABLE_CHILD_FUNC_IDS_OFFSET + index * 4,
            kernel.func_id,
        );
    }
    for (index, offset) in child_offsets.iter().enumerate() {
        write_u32(
            &mut bytes,
            CHIP_CALLABLE_CHILD_OFFSETS_OFFSET + index * 4,
            *offset,
        );
    }
    write_i32(
        &mut bytes,
        CHIP_CALLABLE_CHILD_COUNT_OFFSET,
        child_buffers.len() as i32,
    );
    write_u32(&mut bytes, CHIP_CALLABLE_CONFIG_NAME_LEN_OFFSET, 0);
    bytes[CHIP_CALLABLE_HEADER_SIZE..CHIP_CALLABLE_HEADER_SIZE + orch_binary.len()]
        .copy_from_slice(orch_binary);
    for (offset, child) in child_offsets.iter().zip(child_buffers.iter()) {
        let start = CHIP_CALLABLE_HEADER_SIZE + *offset as usize;
        bytes[start..start + child.len()].copy_from_slice(child);
    }
    Ok(CallableBuffer::new(&bytes))
}

fn make_core_callable(binary: &[u8]) -> Result<Vec<u8>, SimplerApiError> {
    let binary_size = u32::try_from(binary.len()).map_err(|_| SimplerApiError::CallableTooLarge)?;
    let mut bytes = vec![0u8; CORE_CALLABLE_BINARY_OFFSET + binary.len()];
    write_i32(&mut bytes, CORE_CALLABLE_SIG_COUNT_OFFSET, 0);
    write_u32(&mut bytes, CORE_CALLABLE_BINARY_SIZE_OFFSET, binary_size);
    write_u64(&mut bytes, CORE_CALLABLE_RESOLVED_ADDR_OFFSET, 0);
    bytes[CORE_CALLABLE_BINARY_OFFSET..].copy_from_slice(binary);
    Ok(bytes)
}

#[derive(Debug)]
pub struct RuntimeBuffer {
    ptr: NonNull<c_void>,
    layout: Layout,
}

// Runtime buffers are only used while the owning simpler device-context mutex
// is held. Moving one into that serialized context does not permit concurrent
// access to the raw allocation.
unsafe impl Send for RuntimeBuffer {}

impl RuntimeBuffer {
    pub fn allocate(api: &RuntimeLibrary) -> Result<Self, SimplerApiError> {
        let size = api.runtime_size();
        let alignment = api.runtime_alignment();
        if size == 0 {
            return Err(SimplerApiError::InvalidRuntimeLayout { size, alignment });
        }
        let layout = Layout::from_size_align(size, alignment)
            .map_err(|_| SimplerApiError::InvalidRuntimeLayout { size, alignment })?;
        let ptr = unsafe { alloc_zeroed(layout) } as *mut c_void;
        let ptr = NonNull::new(ptr).ok_or(SimplerApiError::NullRuntime)?;
        Ok(Self { ptr, layout })
    }

    pub fn handle(&self) -> OwnedRuntime {
        OwnedRuntime { ptr: self.ptr }
    }
}

impl Drop for RuntimeBuffer {
    fn drop(&mut self) {
        unsafe { dealloc(self.ptr.as_ptr() as *mut u8, self.layout) }
    }
}

#[derive(Debug, Error)]
pub enum SimplerApiError {
    #[error("failed to load simpler runtime library: {0}")]
    LoadLibrary(String),
    #[error("missing required symbol: {0}")]
    MissingSymbol(&'static str),
    #[error("invalid runtime symbol name")]
    InvalidSymbolName,
    #[error("invalid tensor shape")]
    InvalidTensorShape,
    #[error("invalid PTO UB GM run context")]
    InvalidUbGmRunContext,
    #[error("invalid tensor data type {0}")]
    InvalidDataType(u16),
    #[error("too many simpler runtime args")]
    TooManyArgs,
    #[error("callable binary too large")]
    CallableTooLarge,
    #[error("null runtime pointer")]
    NullRuntime,
    #[error("invalid runtime layout size={size} alignment={alignment}")]
    InvalidRuntimeLayout { size: usize, alignment: usize },
    #[error("null device context")]
    NullDeviceContext,
    #[error("null device pointer")]
    NullDevicePointer,
    #[error("{operation} returned error code {code}")]
    ApiFailure { operation: &'static str, code: i32 },
}

impl SimplerApiError {
    fn from_code(operation: &'static str, code: i32) -> Result<(), Self> {
        if code == 0 {
            Ok(())
        } else {
            Err(Self::ApiFailure { operation, code })
        }
    }

    pub fn pto_ub_gm_code(&self) -> Option<&'static str> {
        let Self::ApiFailure { code, .. } = self else {
            return None;
        };
        match *code {
            -3 => Some("pto_ub_gm_bad_memref"),
            -4 => Some("pto_ub_gm_unbound"),
            -5 => Some("pto_ub_gm_access_denied"),
            -7 => Some("pto_ub_gm_callback_failed"),
            -8 => Some("pto_ub_gm_execution_failed"),
            _ => None,
        }
    }
}

pub struct RuntimeLibrary {
    _preloaded_libs: Vec<Library>,
    _lib: Library,
    _staged_path: PathBuf,
    create_device_context: CreateDeviceContextFn,
    destroy_device_context: DestroyDeviceContextFn,
    get_runtime_size: GetRuntimeSizeFn,
    get_runtime_alignment: GetRuntimeAlignmentFn,
    device_malloc_ctx: DeviceMallocCtxFn,
    device_free_ctx: DeviceFreeCtxFn,
    copy_to_device_ctx: CopyToDeviceCtxFn,
    copy_from_device_ctx: CopyFromDeviceCtxFn,
    simpler_init: SimplerInitFn,
    register_callable: SimplerRegisterCallableFn,
    prepare_run: SimplerPrepareRunFn,
    bind_pto_ub_gm_run_context: Option<SimplerBindPtoUbGmRunContextFn>,
    launch_run: SimplerRunPhaseFn,
    wait_run: SimplerRunPhaseFn,
    finalize_run: SimplerRunPhaseFn,
    unregister_callable: SimplerUnregisterCallableFn,
    finalize_device: FinalizeDeviceFn,
}

impl std::fmt::Debug for RuntimeLibrary {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("RuntimeLibrary").finish_non_exhaustive()
    }
}

pub struct DeviceContext<'a> {
    api: &'a RuntimeLibrary,
    ctx: Option<NonNull<c_void>>,
}

// The C runtime attaches the calling thread during simpler_init. Callers must
// still serialize access to a context; Send allows ownership behind a Mutex.
unsafe impl Send for DeviceContext<'_> {}

impl std::fmt::Debug for DeviceContext<'_> {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("DeviceContext").finish_non_exhaustive()
    }
}

impl DeviceContext<'_> {
    pub fn as_raw(&self) -> DeviceContextHandle {
        self.ctx.expect("live device context").as_ptr()
    }

    fn close(&mut self) -> Result<(), SimplerApiError> {
        let Some(ctx) = self.ctx.take() else {
            return Ok(());
        };
        let finalize_result = unsafe { (self.api.finalize_device)(ctx.as_ptr()) };
        unsafe { (self.api.destroy_device_context)(ctx.as_ptr()) };
        SimplerApiError::from_code("finalize_device", finalize_result)
    }

    pub fn shutdown(mut self) -> Result<(), SimplerApiError> {
        self.close()
    }
}

impl Drop for DeviceContext<'_> {
    fn drop(&mut self) {
        let _ = self.close();
    }
}

impl RuntimeLibrary {
    pub fn load(path: &Path) -> Result<Self, SimplerApiError> {
        let mut preloaded_libs = Vec::new();
        // Load libsimpler_log.so FIRST with RTLD_GLOBAL so that libcpu_sim_context.so
        // and the host runtime can resolve unified_log_* symbols against the single
        // process-wide HostLogger instance.
        if let Some(log_lib_path) = std::env::var_os("SIMPLER_LOG_LIBRARY") {
            let log_lib_path = Path::new(&log_lib_path);
            let lib = unsafe { Library::open(Some(log_lib_path), RTLD_NOW | RTLD_GLOBAL) }
                .map_err(|err| SimplerApiError::LoadLibrary(err.to_string()))?;
            preloaded_libs.push(lib);
        }
        // Then load libcpu_sim_context.so with RTLD_GLOBAL so the host runtime can
        // resolve sim_context_set_* and pto_sim_get_* symbols.
        if let Some(sim_context_path) = std::env::var_os("SIMPLER_SIM_CONTEXT_LIBRARY") {
            let sim_context_path = Path::new(&sim_context_path);
            let lib = unsafe { Library::open(Some(sim_context_path), RTLD_NOW | RTLD_GLOBAL) }
                .map_err(|err| SimplerApiError::LoadLibrary(err.to_string()))?;
            preloaded_libs.push(lib);
        }
        let staged_path = stage_runtime_library(path)?;
        let lib = unsafe { Library::open(Some(&staged_path), RTLD_NOW | RTLD_LOCAL) }
            .map_err(|err| SimplerApiError::LoadLibrary(err.to_string()))?;
        unsafe {
            Ok(Self {
                create_device_context: *load_symbol::<CreateDeviceContextFn>(
                    &lib,
                    b"create_device_context\0",
                )?,
                destroy_device_context: *load_symbol::<DestroyDeviceContextFn>(
                    &lib,
                    b"destroy_device_context\0",
                )?,
                get_runtime_size: *load_symbol::<GetRuntimeSizeFn>(&lib, b"get_runtime_size\0")?,
                get_runtime_alignment: *load_symbol::<GetRuntimeAlignmentFn>(
                    &lib,
                    b"get_runtime_alignment\0",
                )?,
                device_malloc_ctx: *load_symbol::<DeviceMallocCtxFn>(&lib, b"device_malloc_ctx\0")?,
                device_free_ctx: *load_symbol::<DeviceFreeCtxFn>(&lib, b"device_free_ctx\0")?,
                copy_to_device_ctx: *load_symbol::<CopyToDeviceCtxFn>(
                    &lib,
                    b"copy_to_device_ctx\0",
                )?,
                copy_from_device_ctx: *load_symbol::<CopyFromDeviceCtxFn>(
                    &lib,
                    b"copy_from_device_ctx\0",
                )?,
                simpler_init: *load_symbol::<SimplerInitFn>(&lib, b"simpler_init\0")?,
                register_callable: *load_symbol::<SimplerRegisterCallableFn>(
                    &lib,
                    b"simpler_register_callable\0",
                )?,
                prepare_run: *load_symbol::<SimplerPrepareRunFn>(&lib, b"simpler_prepare_run\0")?,
                bind_pto_ub_gm_run_context: load_optional_symbol::<SimplerBindPtoUbGmRunContextFn>(
                    &lib,
                    b"simpler_bind_pto_ub_gm_run_context\0",
                ),
                launch_run: *load_symbol::<SimplerRunPhaseFn>(&lib, b"simpler_launch_run\0")?,
                wait_run: *load_symbol::<SimplerRunPhaseFn>(&lib, b"simpler_wait_run\0")?,
                finalize_run: *load_symbol::<SimplerRunPhaseFn>(&lib, b"simpler_finalize_run\0")?,
                unregister_callable: *load_symbol::<SimplerUnregisterCallableFn>(
                    &lib,
                    b"simpler_unregister_callable\0",
                )?,
                finalize_device: *load_symbol::<FinalizeDeviceFn>(&lib, b"finalize_device\0")?,
                _preloaded_libs: preloaded_libs,
                _lib: lib,
                _staged_path: staged_path,
            })
        }
    }

    pub fn runtime_size(&self) -> usize {
        unsafe { (self.get_runtime_size)() }
    }

    pub fn runtime_alignment(&self) -> usize {
        unsafe { (self.get_runtime_alignment)() }
    }

    pub fn create_context(&self) -> Result<DeviceContext<'_>, SimplerApiError> {
        let ctx = unsafe { (self.create_device_context)() };
        let ctx = NonNull::new(ctx).ok_or(SimplerApiError::NullDeviceContext)?;
        Ok(DeviceContext {
            api: self,
            ctx: Some(ctx),
        })
    }

    pub fn alloc_device(
        &self,
        ctx: &DeviceContext<'_>,
        size: usize,
    ) -> Result<DevicePtr, SimplerApiError> {
        let ptr = unsafe { (self.device_malloc_ctx)(ctx.as_raw(), size) };
        DevicePtr::from_raw(ptr).ok_or(SimplerApiError::NullDevicePointer)
    }

    pub fn free_device(&self, ctx: &DeviceContext<'_>, ptr: DevicePtr) {
        unsafe { (self.device_free_ctx)(ctx.as_raw(), ptr.as_ptr()) }
    }

    pub fn host_to_device(
        &self,
        ctx: &DeviceContext<'_>,
        dev_ptr: DevicePtr,
        host_ptr: *const c_void,
        size: usize,
    ) -> Result<(), SimplerApiError> {
        unsafe {
            SimplerApiError::from_code(
                "copy_to_device_ctx",
                (self.copy_to_device_ctx)(ctx.as_raw(), dev_ptr.as_ptr(), host_ptr, size),
            )
        }
    }

    pub fn device_to_host(
        &self,
        ctx: &DeviceContext<'_>,
        host_ptr: *mut c_void,
        dev_ptr: DevicePtr,
        size: usize,
    ) -> Result<(), SimplerApiError> {
        unsafe {
            SimplerApiError::from_code(
                "copy_from_device_ctx",
                (self.copy_from_device_ctx)(ctx.as_raw(), host_ptr, dev_ptr.as_ptr(), size),
            )
        }
    }

    pub fn run_callable(
        &self,
        ctx: &DeviceContext<'_>,
        runtime: OwnedRuntime,
        callable: &CallableBuffer,
        args: &ChipStorageTaskArgs,
        block_dim: i32,
        aicpu_thread_num: i32,
        device_id: i32,
        aicpu_binary: *const u8,
        aicpu_size: usize,
        aicore_binary: *const u8,
        aicore_size: usize,
    ) -> Result<(), SimplerApiError> {
        let callable_id = 0;
        self.initialize_context(
            ctx,
            device_id,
            aicpu_binary,
            aicpu_size,
            aicore_binary,
            aicore_size,
        )?;
        self.run_prepared_callable(
            ctx,
            runtime,
            callable,
            args,
            callable_id,
            true,
            block_dim,
            aicpu_thread_num,
            None,
        )?;
        unsafe {
            SimplerApiError::from_code(
                "simpler_unregister_callable",
                (self.unregister_callable)(ctx.as_raw(), callable_id),
            )
        }
    }

    pub fn initialize_context(
        &self,
        ctx: &DeviceContext<'_>,
        device_id: i32,
        aicpu_binary: *const u8,
        aicpu_size: usize,
        aicore_binary: *const u8,
        aicore_size: usize,
    ) -> Result<(), SimplerApiError> {
        unsafe {
            SimplerApiError::from_code(
                "simpler_init",
                (self.simpler_init)(
                    ctx.as_raw(),
                    device_id as c_int,
                    aicpu_binary,
                    aicpu_size,
                    aicore_binary,
                    aicore_size,
                    std::ptr::null(),
                    0,
                    std::ptr::null(),
                ),
            )
        }
    }

    #[allow(clippy::too_many_arguments)]
    pub fn run_prepared_callable(
        &self,
        ctx: &DeviceContext<'_>,
        runtime: OwnedRuntime,
        callable: &CallableBuffer,
        args: &ChipStorageTaskArgs,
        callable_id: i32,
        prepare: bool,
        _block_dim: i32,
        aicpu_thread_num: i32,
        ub_gm_run_context: Option<&PtoUbGmRunContext>,
    ) -> Result<(), SimplerApiError> {
        let config = CallConfig::new(aicpu_thread_num);
        let descriptor = next_native_run_descriptor();
        unsafe {
            if prepare {
                SimplerApiError::from_code(
                    "simpler_register_callable",
                    (self.register_callable)(ctx.as_raw(), callable_id, callable.as_ptr()),
                )?;
            }
            SimplerApiError::from_code(
                "simpler_prepare_run",
                (self.prepare_run)(
                    ctx.as_raw(),
                    runtime.as_raw(),
                    callable_id,
                    args as *const _ as *const c_void,
                    &config,
                    &descriptor,
                ),
            )?;
            if let Some(run_context) = ub_gm_run_context {
                let bind_result = match self.bind_pto_ub_gm_run_context {
                    Some(bind) => SimplerApiError::from_code(
                        "simpler_bind_pto_ub_gm_run_context",
                        bind(ctx.as_raw(), runtime.as_raw(), run_context.as_ptr()),
                    ),
                    None => Err(SimplerApiError::MissingSymbol(
                        "simpler_bind_pto_ub_gm_run_context",
                    )),
                };
                if let Err(error) = bind_result {
                    let _ = (self.finalize_run)(ctx.as_raw(), runtime.as_raw());
                    return Err(error);
                }
            }
            let launch_code = (self.launch_run)(ctx.as_raw(), runtime.as_raw());
            let wait_code = if launch_code == 0 {
                (self.wait_run)(ctx.as_raw(), runtime.as_raw())
            } else {
                0
            };
            let finalize_code = (self.finalize_run)(ctx.as_raw(), runtime.as_raw());
            SimplerApiError::from_code("simpler_finalize_run", finalize_code)?;
            SimplerApiError::from_code("simpler_launch_run", launch_code)?;
            SimplerApiError::from_code("simpler_wait_run", wait_code)
        }
    }
}

fn stage_runtime_library(path: &Path) -> Result<PathBuf, SimplerApiError> {
    let ext = path
        .extension()
        .and_then(|ext| ext.to_str())
        .unwrap_or("so");
    let nanos = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map_err(|err| SimplerApiError::LoadLibrary(err.to_string()))?
        .as_nanos();
    let staged_path = std::env::temp_dir().join(format!(
        "simpler-runtime-{}-{}.{}",
        std::process::id(),
        nanos,
        ext
    ));
    fs::copy(path, &staged_path)
        .map_err(|err| SimplerApiError::LoadLibrary(format!("stage_copy_failed:{err}")))?;
    Ok(staged_path)
}

unsafe fn load_symbol<T>(
    lib: &Library,
    symbol: &'static [u8],
) -> Result<Symbol<T>, SimplerApiError> {
    lib.get::<T>(symbol).map_err(|_| {
        SimplerApiError::MissingSymbol(std::str::from_utf8(symbol).unwrap_or("invalid_symbol"))
    })
}

unsafe fn load_optional_symbol<T: Copy>(lib: &Library, symbol: &'static [u8]) -> Option<T> {
    lib.get::<T>(symbol).ok().map(|symbol| *symbol)
}

fn write_i32(bytes: &mut [u8], offset: usize, value: i32) {
    bytes[offset..offset + 4].copy_from_slice(&value.to_ne_bytes());
}

fn write_u32(bytes: &mut [u8], offset: usize, value: u32) {
    bytes[offset..offset + 4].copy_from_slice(&value.to_ne_bytes());
}

fn write_u64(bytes: &mut [u8], offset: usize, value: u64) {
    bytes[offset..offset + 8].copy_from_slice(&value.to_ne_bytes());
}

fn write_cstr(
    bytes: &mut [u8],
    offset: usize,
    capacity: usize,
    value: &str,
) -> Result<(), SimplerApiError> {
    if value.as_bytes().contains(&0) {
        return Err(SimplerApiError::InvalidSymbolName);
    }
    let len = value.len().min(capacity - 1);
    bytes[offset..offset + capacity].fill(0);
    bytes[offset..offset + len].copy_from_slice(&value.as_bytes()[..len]);
    Ok(())
}

fn align_up(value: usize, align: usize) -> usize {
    (value + align - 1) & !(align - 1)
}

const CHIP_MAX_TENSOR_ARGS: usize = 256;
const CHIP_MAX_SCALAR_ARGS: usize = 128;
const CHIP_MAX_CHILDREN: usize = 1024;
const CALLABLE_ALIGN: usize = 64;
const CALLABLE_CHILD_ALIGN: usize = 16;
const CALLABLE_FUNC_NAME_MAX: usize = 64;
const CORE_CALLABLE_SIG_COUNT_OFFSET: usize = 128;
const CORE_CALLABLE_BINARY_SIZE_OFFSET: usize = 132;
const CORE_CALLABLE_RESOLVED_ADDR_OFFSET: usize = 136;
const CORE_CALLABLE_BINARY_OFFSET: usize = 192;
const CHIP_CALLABLE_SIG_COUNT_OFFSET: usize = 1024;
const CHIP_CALLABLE_BINARY_SIZE_OFFSET: usize = 1028;
const CHIP_CALLABLE_FUNC_NAME_OFFSET: usize = 1032;
const CHIP_CALLABLE_FUNC_NAME_LEN_OFFSET: usize = 1096;
const CHIP_CALLABLE_CHILD_FUNC_IDS_OFFSET: usize = 1100;
const CHIP_CALLABLE_CHILD_OFFSETS_OFFSET: usize = 5196;
const CHIP_CALLABLE_CHILD_COUNT_OFFSET: usize = 9292;
const CHIP_CALLABLE_CONFIG_NAME_LEN_OFFSET: usize = 9360;
const CHIP_CALLABLE_HEADER_SIZE: usize = 9376;

const _: () = assert!(std::mem::size_of::<Tensor>() == 128);
const _: () = assert!(std::mem::align_of::<Tensor>() == 64);
const _: () = assert!(std::mem::size_of::<ChipStorageTaskArgs>() == 33856);
const _: () = assert!(std::mem::size_of::<RuntimeEnv>() == 96);
const _: () = assert!(std::mem::size_of::<CallConfig>() == 1144);
const _: () = assert!(std::mem::size_of::<NativeRunDescriptor>() == 56);
const _: () = assert!(std::mem::align_of::<NativeRunDescriptor>() == 8);
const _: () = assert!(std::mem::size_of::<PtoUbGmBinding>() == 64);
const _: () = assert!(std::mem::size_of::<PtoUbGmAccessOps>() == 24);
const _: () = assert!(std::mem::size_of::<PtoUbGmRunContext>() == 40);

#[cfg(test)]
mod tests {
    use super::{
        make_chip_callable, next_native_run_descriptor, AddressSpace, ArgDirection,
        ChipStorageTaskArgs, DataType, KernelCallableInput, PtoUbGmAccessOps, PtoUbGmBinding,
        PtoUbGmRunContext, SimplerApiError, Tensor, CALLABLE_CHILD_ALIGN,
        CHIP_CALLABLE_BINARY_SIZE_OFFSET, CHIP_CALLABLE_CHILD_COUNT_OFFSET,
        CHIP_CALLABLE_CHILD_OFFSETS_OFFSET, CHIP_CALLABLE_HEADER_SIZE,
        CHIP_CALLABLE_SIG_COUNT_OFFSET, CHIP_MAX_TENSOR_ARGS,
    };
    use std::ffi::{c_int, c_void};

    unsafe extern "C" fn test_ub_gm_read(
        _backend_context: *mut c_void,
        _request_id: u64,
        _binding_id: u64,
        _ub_gm_addr: u64,
        _dst: *mut c_void,
        _length: u64,
    ) -> c_int {
        0
    }

    unsafe extern "C" fn test_ub_gm_write(
        _backend_context: *mut c_void,
        _request_id: u64,
        _binding_id: u64,
        _ub_gm_addr: u64,
        _src: *const c_void,
        _length: u64,
    ) -> c_int {
        0
    }

    unsafe extern "C" fn test_ub_gm_fence(
        _backend_context: *mut c_void,
        _request_id: u64,
        _binding_id: u64,
        _ub_gm_addr: u64,
        _length: u64,
        _flags: u32,
    ) -> c_int {
        0
    }

    #[test]
    fn tensor_address_space_values_and_layout_are_frozen() {
        assert_eq!(AddressSpace::Host as u8, 0);
        assert_eq!(AddressSpace::Device as u8, 1);
        assert_eq!(AddressSpace::UbGm as u8, 2);
        assert_eq!(std::mem::size_of::<Tensor>(), 128);
        assert_eq!(std::mem::align_of::<Tensor>(), 64);
        assert_eq!(std::mem::offset_of!(Tensor, address_space), 43);

        let tensor =
            Tensor::from_shape(0x1000, 64, &[16], DataType::Float32, false).expect("host tensor");
        assert_eq!(tensor.address_space(), AddressSpace::Host);
    }

    #[test]
    fn ub_gm_tensor_preserves_synthetic_aperture_without_host_allocation() {
        let tensor = Tensor::from_ub_gm(
            0x7000_0000_1000,
            16_384,
            &[64, 64],
            &[64, 1],
            DataType::Float32,
        )
        .expect("UB GM tensor");

        assert_eq!(tensor.address_space(), AddressSpace::UbGm);
        assert_eq!(tensor.buffer_addr(), 0x7000_0000_1000);
        assert_eq!(tensor.buffer_size(), 16_384);
        assert!(tensor.is_contiguous());
    }

    #[test]
    fn ub_gm_tensor_rejects_invalid_dtype_and_out_of_bounds_view() {
        assert!(DataType::try_from(15).is_err());
        assert!(Tensor::from_ub_gm(0x7000_0000_1000, 16, &[8], &[1], DataType::Float32,).is_err());
        assert!(Tensor::from_ub_gm(0, 32, &[8], &[1], DataType::Float32).is_err());
    }

    fn read_u32(bytes: &[u8], offset: usize) -> u32 {
        u32::from_ne_bytes(bytes[offset..offset + 4].try_into().expect("u32 field"))
    }

    #[test]
    fn native_run_descriptors_have_unique_valid_identities() {
        let first = next_native_run_descriptor();
        let second = next_native_run_descriptor();

        assert_ne!(first.run_epoch, 0);
        assert!(second.run_epoch > first.run_epoch);
        assert_eq!(first.generation, 1);
        assert_eq!(first.run_id, first.run_epoch);
        assert_eq!(first.dispatch_id, first.run_epoch);
        assert!(first.accepted_state.is_null());
    }

    #[test]
    fn pto_ub_gm_run_context_preserves_c_abi_pointers() {
        let binding = PtoUbGmBinding {
            request_id: 91,
            binding_id: 7,
            aperture_base: 0x7000_0000_0000,
            aperture_length: 4096,
            ub_gm_base: 0x20_0000,
            mapped_length: 4096,
            access: 3,
            flags: 0,
            backend_cookie: 11,
        };
        let ops = PtoUbGmAccessOps {
            read: test_ub_gm_read,
            write: test_ub_gm_write,
            fence: test_ub_gm_fence,
        };
        let context = PtoUbGmRunContext::new(91, &[binding], &ops, 1usize as *mut c_void)
            .expect("valid context");

        assert_eq!(context.request_id, 91);
        assert_eq!(context.binding_count, 1);
        assert!(!context.bindings.is_null());
        assert_eq!(context.ops, &ops);
        assert_eq!(context.backend_context as usize, 1);
    }

    #[test]
    fn pto_ub_gm_errors_map_to_stable_completion_codes() {
        for (raw, expected) in [
            (-3, "pto_ub_gm_bad_memref"),
            (-4, "pto_ub_gm_unbound"),
            (-5, "pto_ub_gm_access_denied"),
            (-7, "pto_ub_gm_callback_failed"),
            (-8, "pto_ub_gm_execution_failed"),
        ] {
            let error = SimplerApiError::ApiFailure {
                operation: "simpler_wait_run",
                code: raw,
            };
            assert_eq!(error.pto_ub_gm_code(), Some(expected));
        }
    }

    #[test]
    fn chip_callable_matches_current_simpler_wire_layout() {
        let kernel = [0x33u8, 0x44];
        let callable = make_chip_callable(
            "test_entry",
            &[0x11, 0x22],
            &[KernelCallableInput {
                func_id: 7,
                binary: &kernel,
            }],
            &[ArgDirection::In],
        )
        .expect("callable");
        let bytes = callable.as_bytes();

        assert_eq!(callable.as_ptr() as usize % CALLABLE_CHILD_ALIGN, 0);
        assert_eq!(read_u32(bytes, CHIP_CALLABLE_SIG_COUNT_OFFSET), 1);
        assert_eq!(read_u32(bytes, CHIP_CALLABLE_BINARY_SIZE_OFFSET), 2);
        assert_eq!(read_u32(bytes, CHIP_CALLABLE_CHILD_COUNT_OFFSET), 1);
        assert_eq!(read_u32(bytes, CHIP_CALLABLE_CHILD_OFFSETS_OFFSET), 64);
        assert_eq!(&bytes[CHIP_CALLABLE_HEADER_SIZE..][..2], &[0x11, 0x22]);
    }

    #[test]
    fn chip_task_args_accept_current_tensor_capacity() {
        let tensor = Tensor::new(0, 4, DataType::Float32).expect("tensor");
        let tensors = vec![tensor; CHIP_MAX_TENSOR_ARGS];
        let args = ChipStorageTaskArgs::new(&tensors, &[]).expect("task args");

        assert_eq!(args.tensor_count, CHIP_MAX_TENSOR_ARGS as i32);
    }
}
