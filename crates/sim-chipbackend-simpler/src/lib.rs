//! Thin Rust-side runtime loader for `simpler` host `pto_runtime_c_api`.
//!
//! Current vendored `simpler` exposes the HostBuildGraph runtime through the
//! worker C API: initialize a device context with `simpler_init`, register a
//! `ChipCallable`, then launch it via `simpler_run`.

use std::ffi::{c_int, c_void};
use std::fs;
use std::path::{Path, PathBuf};
use std::ptr::NonNull;
use std::time::{SystemTime, UNIX_EPOCH};

use libc::{free, malloc};
use libloading::os::unix::{Library, Symbol, RTLD_GLOBAL, RTLD_LOCAL, RTLD_NOW};
use thiserror::Error;

pub type RuntimeHandle = *mut c_void;
pub type DeviceContextHandle = *mut c_void;

type CreateDeviceContextFn = unsafe extern "C" fn() -> DeviceContextHandle;
type DestroyDeviceContextFn = unsafe extern "C" fn(DeviceContextHandle);
type GetRuntimeSizeFn = unsafe extern "C" fn() -> usize;
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
) -> c_int;
type SimplerRegisterCallableFn =
    unsafe extern "C" fn(DeviceContextHandle, i32, *const c_void) -> c_int;
type SimplerRunFn = unsafe extern "C" fn(
    DeviceContextHandle,
    RuntimeHandle,
    i32,
    *const c_void,
    *const CallConfig,
) -> c_int;
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
    block_dim: i32,
    aicpu_thread_num: i32,
    enable_l2_swimlane: i32,
    enable_dump_tensor: i32,
    enable_pmu: i32,
    enable_dep_gen: i32,
    enable_scope_stats: i32,
    runtime_env: RuntimeEnv,
    output_prefix: [u8; 1024],
}

impl CallConfig {
    fn new(block_dim: i32, aicpu_thread_num: i32) -> Self {
        Self {
            block_dim,
            aicpu_thread_num,
            enable_l2_swimlane: 0,
            enable_dump_tensor: 0,
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
}

impl DataType {
    pub fn element_size(self) -> usize {
        match self {
            Self::Float32 | Self::Int32 | Self::Uint32 => 4,
            Self::Float16 | Self::Int16 | Self::Bfloat16 | Self::Uint16 => 2,
            Self::Int8 | Self::Uint8 => 1,
            Self::Int64 | Self::Uint64 => 8,
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
    child_memory: u8,
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
        child_memory: bool,
    ) -> Result<Self, SimplerApiError> {
        if shape.is_empty() || shape.len() > 5 || shape.contains(&0) {
            return Err(SimplerApiError::InvalidTensorShape);
        }
        let mut shapes = [0u32; 5];
        let mut strides = [0u32; 5];
        let mut elements = 1u64;
        for (index, dimension) in shape.iter().copied().enumerate().rev() {
            strides[index] =
                u32::try_from(elements).map_err(|_| SimplerApiError::InvalidTensorShape)?;
            elements = elements
                .checked_mul(u64::from(dimension))
                .ok_or(SimplerApiError::InvalidTensorShape)?;
            shapes[index] = dimension;
        }
        let required_bytes = elements
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
            is_contiguous: 1,
            child_memory: u8::from(child_memory),
            shapes,
            extent_elem_cache: elements,
            strides,
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
            child_memory: 0,
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
}

pub struct KernelCallableInput<'a> {
    pub func_id: i32,
    pub binary: &'a [u8],
}

#[derive(Debug, Clone)]
pub struct CallableBuffer {
    bytes: Vec<u8>,
}

impl CallableBuffer {
    pub fn as_ptr(&self) -> *const c_void {
        self.bytes.as_ptr() as *const c_void
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
    Ok(CallableBuffer { bytes })
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
}

impl RuntimeBuffer {
    pub fn allocate(api: &RuntimeLibrary) -> Result<Self, SimplerApiError> {
        let ptr = unsafe { malloc(api.runtime_size()) };
        let ptr = NonNull::new(ptr).ok_or(SimplerApiError::NullRuntime)?;
        Ok(Self { ptr })
    }

    pub fn handle(&self) -> OwnedRuntime {
        OwnedRuntime { ptr: self.ptr }
    }
}

impl Drop for RuntimeBuffer {
    fn drop(&mut self) {
        unsafe { free(self.ptr.as_ptr()) }
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
    #[error("too many simpler runtime args")]
    TooManyArgs,
    #[error("callable binary too large")]
    CallableTooLarge,
    #[error("null runtime pointer")]
    NullRuntime,
    #[error("null device context")]
    NullDeviceContext,
    #[error("null device pointer")]
    NullDevicePointer,
    #[error("api returned error code {code}")]
    ApiFailure { code: i32 },
}

impl SimplerApiError {
    fn from_code(code: i32) -> Result<(), Self> {
        if code == 0 {
            Ok(())
        } else {
            Err(Self::ApiFailure { code })
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
    device_malloc_ctx: DeviceMallocCtxFn,
    device_free_ctx: DeviceFreeCtxFn,
    copy_to_device_ctx: CopyToDeviceCtxFn,
    copy_from_device_ctx: CopyFromDeviceCtxFn,
    simpler_init: SimplerInitFn,
    register_callable: SimplerRegisterCallableFn,
    simpler_run: SimplerRunFn,
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
        SimplerApiError::from_code(finalize_result)
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
                simpler_run: *load_symbol::<SimplerRunFn>(&lib, b"simpler_run\0")?,
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
            SimplerApiError::from_code((self.copy_to_device_ctx)(
                ctx.as_raw(),
                dev_ptr.as_ptr(),
                host_ptr,
                size,
            ))
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
            SimplerApiError::from_code((self.copy_from_device_ctx)(
                ctx.as_raw(),
                host_ptr,
                dev_ptr.as_ptr(),
                size,
            ))
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
        self.run_prepared_callable(
            ctx,
            runtime,
            callable,
            args,
            callable_id,
            true,
            block_dim,
            aicpu_thread_num,
            device_id,
            aicpu_binary,
            aicpu_size,
            aicore_binary,
            aicore_size,
        )?;
        unsafe { SimplerApiError::from_code((self.unregister_callable)(ctx.as_raw(), callable_id)) }
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
        block_dim: i32,
        aicpu_thread_num: i32,
        device_id: i32,
        aicpu_binary: *const u8,
        aicpu_size: usize,
        aicore_binary: *const u8,
        aicore_size: usize,
    ) -> Result<(), SimplerApiError> {
        let config = CallConfig::new(block_dim, aicpu_thread_num);
        unsafe {
            SimplerApiError::from_code((self.simpler_init)(
                ctx.as_raw(),
                device_id as c_int,
                aicpu_binary,
                aicpu_size,
                aicore_binary,
                aicore_size,
                std::ptr::null(),
                0,
            ))?;
            if prepare {
                SimplerApiError::from_code((self.register_callable)(
                    ctx.as_raw(),
                    callable_id,
                    callable.as_ptr(),
                ))?;
            }
            SimplerApiError::from_code((self.simpler_run)(
                ctx.as_raw(),
                runtime.as_raw(),
                callable_id,
                args as *const _ as *const c_void,
                &config,
            ))
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

const CHIP_MAX_TENSOR_ARGS: usize = 128;
const CHIP_MAX_SCALAR_ARGS: usize = 128;
const CHIP_MAX_CHILDREN: usize = 1024;
const CALLABLE_ALIGN: usize = 64;
const CALLABLE_FUNC_NAME_MAX: usize = 64;
const CORE_CALLABLE_SIG_COUNT_OFFSET: usize = 128;
const CORE_CALLABLE_BINARY_SIZE_OFFSET: usize = 132;
const CORE_CALLABLE_RESOLVED_ADDR_OFFSET: usize = 136;
const CORE_CALLABLE_BINARY_OFFSET: usize = 192;
const CHIP_CALLABLE_SIG_COUNT_OFFSET: usize = 512;
const CHIP_CALLABLE_BINARY_SIZE_OFFSET: usize = 516;
const CHIP_CALLABLE_FUNC_NAME_OFFSET: usize = 520;
const CHIP_CALLABLE_FUNC_NAME_LEN_OFFSET: usize = 584;
const CHIP_CALLABLE_CHILD_FUNC_IDS_OFFSET: usize = 588;
const CHIP_CALLABLE_CHILD_OFFSETS_OFFSET: usize = 4684;
const CHIP_CALLABLE_CHILD_COUNT_OFFSET: usize = 8780;
const CHIP_CALLABLE_CONFIG_NAME_LEN_OFFSET: usize = 8848;
const CHIP_CALLABLE_HEADER_SIZE: usize = 8864;

const _: () = assert!(std::mem::size_of::<Tensor>() == 128);
const _: () = assert!(std::mem::align_of::<Tensor>() == 64);
const _: () = assert!(std::mem::size_of::<ChipStorageTaskArgs>() == 17472);
const _: () = assert!(std::mem::size_of::<RuntimeEnv>() == 96);
const _: () = assert!(std::mem::size_of::<CallConfig>() == 1148);
