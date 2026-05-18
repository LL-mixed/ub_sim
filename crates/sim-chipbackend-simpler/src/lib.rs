//! Thin Rust-side runtime loader for `simpler` host `pto_runtime_c_api`.
//!
//! Current vendored `simpler` exposes the HostBuildGraph runtime through the
//! worker C API: callers pass a `ChipCallable` plus `ChipStorageTaskArgs` to
//! `run_runtime`, rather than calling separate init/launch/finalize symbols.

use std::cell::Cell;
use std::ffi::{c_char, c_int, c_void, CString};
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
type SetDeviceFn = unsafe extern "C" fn(DeviceContextHandle, c_int) -> c_int;
type DeviceMallocCtxFn = unsafe extern "C" fn(DeviceContextHandle, usize) -> *mut c_void;
type DeviceFreeCtxFn = unsafe extern "C" fn(DeviceContextHandle, *mut c_void);
type CopyToDeviceCtxFn =
    unsafe extern "C" fn(DeviceContextHandle, *mut c_void, *const c_void, usize) -> c_int;
type CopyFromDeviceCtxFn =
    unsafe extern "C" fn(DeviceContextHandle, *mut c_void, *const c_void, usize) -> c_int;
type RunRuntimeFn = unsafe extern "C" fn(
    DeviceContextHandle,
    RuntimeHandle,
    *const c_void,
    *const c_void,
    c_int,
    c_int,
    c_int,
    *const u8,
    usize,
    *const u8,
    usize,
    c_int,
    c_int,
    c_int,
    *const c_char,
) -> c_int;
type SimplerInitFn =
    unsafe extern "C" fn(DeviceContextHandle, c_int, *const u8, usize, *const u8, usize) -> c_int;
type PrepareCallableFn = unsafe extern "C" fn(DeviceContextHandle, c_int, *const c_void) -> c_int;
type RunPreparedFn = unsafe extern "C" fn(
    DeviceContextHandle,
    RuntimeHandle,
    c_int,
    *const c_void,
    c_int,
    c_int,
    c_int,
    c_int,
    c_int,
    c_int,
    *const c_char,
) -> c_int;
type UnregisterCallableFn = unsafe extern "C" fn(DeviceContextHandle, c_int) -> c_int;
type FinalizeDeviceFn = unsafe extern "C" fn(DeviceContextHandle) -> c_int;

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

#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct ContinuousTensor {
    data: u64,
    shapes: [u32; 5],
    ndims: u32,
    dtype: DataType,
    child_memory: u8,
}

impl ContinuousTensor {
    pub fn new(data: u64, bytes: u64, dtype: DataType) -> Result<Self, SimplerApiError> {
        let elem_size = dtype.element_size() as u64;
        if elem_size == 0 || bytes % elem_size != 0 {
            return Err(SimplerApiError::InvalidTensorShape);
        }
        let elems = bytes / elem_size;
        let elems = u32::try_from(elems).map_err(|_| SimplerApiError::InvalidTensorShape)?;
        Ok(Self {
            data,
            shapes: [elems, 1, 1, 1, 1],
            ndims: 1,
            dtype,
            child_memory: 0,
        })
    }

    pub fn from_shape(data: u64, shape: &[u32], dtype: DataType) -> Result<Self, SimplerApiError> {
        Self::from_shape_with_child_memory(data, shape, dtype, false)
    }

    pub fn from_shape_with_child_memory(
        data: u64,
        shape: &[u32],
        dtype: DataType,
        child_memory: bool,
    ) -> Result<Self, SimplerApiError> {
        if shape.is_empty() || shape.len() > 5 || shape.contains(&0) {
            return Err(SimplerApiError::InvalidTensorShape);
        }
        let mut shapes = [1u32; 5];
        shapes[..shape.len()].copy_from_slice(shape);
        Ok(Self {
            data,
            shapes,
            ndims: shape.len() as u32,
            dtype,
            child_memory: u8::from(child_memory),
        })
    }
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct ChipStorageTaskArgs {
    tensors: [ContinuousTensor; CHIP_MAX_TENSOR_ARGS],
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
    pub fn new(tensors: &[ContinuousTensor], scalars: &[u64]) -> Result<Self, SimplerApiError> {
        if tensors.len() > CHIP_MAX_TENSOR_ARGS || scalars.len() > CHIP_MAX_SCALAR_ARGS {
            return Err(SimplerApiError::TooManyArgs);
        }
        let zero_tensor = ContinuousTensor {
            data: 0,
            shapes: [0; 5],
            ndims: 0,
            dtype: DataType::Uint8,
            child_memory: 0,
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
    write_i32(&mut bytes, 256, signature.len() as i32);
    write_u32(&mut bytes, 260, orch_binary.len() as u32);
    write_cstr(&mut bytes, 264, CALLABLE_FUNC_NAME_MAX, orch_function_name)?;
    write_u32(
        &mut bytes,
        328,
        orch_function_name.len().min(CALLABLE_FUNC_NAME_MAX - 1) as u32,
    );
    for (index, kernel) in kernels.iter().enumerate() {
        write_i32(&mut bytes, 332 + index * 4, kernel.func_id);
    }
    for (index, offset) in child_offsets.iter().enumerate() {
        write_u32(&mut bytes, 4428 + index * 4, *offset);
    }
    write_i32(&mut bytes, 8524, child_buffers.len() as i32);
    write_u32(&mut bytes, 8592, 0);
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
    write_i32(&mut bytes, 64, 0);
    write_u32(&mut bytes, 68, binary_size);
    write_u64(&mut bytes, 72, 0);
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
    #[error("runtime library does not expose a supported launch ABI")]
    UnsupportedRuntimeAbi,
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
    set_device: Option<SetDeviceFn>,
    device_malloc_ctx: DeviceMallocCtxFn,
    device_free_ctx: DeviceFreeCtxFn,
    copy_to_device_ctx: CopyToDeviceCtxFn,
    copy_from_device_ctx: CopyFromDeviceCtxFn,
    run_runtime: Option<RunRuntimeFn>,
    simpler_init: Option<SimplerInitFn>,
    prepare_callable: Option<PrepareCallableFn>,
    run_prepared: Option<RunPreparedFn>,
    unregister_callable: Option<UnregisterCallableFn>,
    finalize_device: FinalizeDeviceFn,
}

impl std::fmt::Debug for RuntimeLibrary {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("RuntimeLibrary").finish_non_exhaustive()
    }
}

pub struct DeviceContext<'a> {
    api: &'a RuntimeLibrary,
    ctx: NonNull<c_void>,
    prepared_runtime_initialized: Cell<bool>,
}

impl std::fmt::Debug for DeviceContext<'_> {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("DeviceContext").finish_non_exhaustive()
    }
}

impl DeviceContext<'_> {
    pub fn as_raw(&self) -> DeviceContextHandle {
        self.ctx.as_ptr()
    }
}

impl Drop for DeviceContext<'_> {
    fn drop(&mut self) {
        // `run_runtime` already performs runtime-level cleanup. The current
        // simpler sim C API can crash during Rust test-process teardown if
        // device finalization is repeated here, so native context cleanup stays
        // opt-in until the upstream teardown contract is stable.
        unsafe {
            if std::env::var_os("SIMPLER_CAPI_FINALIZE_DEVICE").is_some() {
                (self.api.finalize_device)(self.as_raw());
            }
            if std::env::var_os("SIMPLER_CAPI_DESTROY_CONTEXT").is_some() {
                (self.api.destroy_device_context)(self.as_raw());
            }
        }
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
                set_device: load_optional_symbol::<SetDeviceFn>(&lib, b"set_device\0")?,
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
                run_runtime: load_optional_symbol::<RunRuntimeFn>(&lib, b"run_runtime\0")?,
                simpler_init: load_optional_symbol::<SimplerInitFn>(&lib, b"simpler_init\0")?,
                prepare_callable: load_optional_symbol::<PrepareCallableFn>(
                    &lib,
                    b"prepare_callable\0",
                )?,
                run_prepared: load_optional_symbol::<RunPreparedFn>(&lib, b"run_prepared\0")?,
                unregister_callable: load_optional_symbol::<UnregisterCallableFn>(
                    &lib,
                    b"unregister_callable\0",
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

    pub fn create_context(&self, device_id: i32) -> Result<DeviceContext<'_>, SimplerApiError> {
        let ctx = unsafe { (self.create_device_context)() };
        let ctx = NonNull::new(ctx).ok_or(SimplerApiError::NullDeviceContext)?;
        let context = DeviceContext {
            api: self,
            ctx,
            prepared_runtime_initialized: Cell::new(false),
        };
        if let Some(set_device) = self.set_device {
            unsafe {
                SimplerApiError::from_code((set_device)(context.as_raw(), device_id as c_int))?;
            }
        }
        Ok(context)
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

    pub fn run_runtime(
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
        let output_prefix = CString::new("").map_err(|_| SimplerApiError::InvalidSymbolName)?;
        if let Some(run_runtime) = self.run_runtime {
            unsafe {
                return SimplerApiError::from_code((run_runtime)(
                    ctx.as_raw(),
                    runtime.as_raw(),
                    callable.as_ptr(),
                    args as *const _ as *const c_void,
                    block_dim as c_int,
                    aicpu_thread_num as c_int,
                    device_id as c_int,
                    aicpu_binary,
                    aicpu_size,
                    aicore_binary,
                    aicore_size,
                    0,
                    0,
                    0,
                    output_prefix.as_ptr(),
                ));
            }
        }
        let (simpler_init, prepare_callable, run_prepared) =
            match (self.simpler_init, self.prepare_callable, self.run_prepared) {
                (Some(simpler_init), Some(prepare_callable), Some(run_prepared)) => {
                    (simpler_init, prepare_callable, run_prepared)
                }
                _ => return Err(SimplerApiError::UnsupportedRuntimeAbi),
            };
        let trace = std::env::var_os("SIMPLER_CAPI_TRACE").is_some();
        if !ctx.prepared_runtime_initialized.get() {
            if trace {
                eprintln!("simpler_capi: simpler_init");
            }
            unsafe {
                SimplerApiError::from_code((simpler_init)(
                    ctx.as_raw(),
                    device_id as c_int,
                    aicpu_binary,
                    aicpu_size,
                    aicore_binary,
                    aicore_size,
                ))?;
            }
            ctx.prepared_runtime_initialized.set(true);
        }
        let callable_id = 0;
        unsafe {
            if trace {
                eprintln!("simpler_capi: prepare_callable");
            }
            SimplerApiError::from_code((prepare_callable)(
                ctx.as_raw(),
                callable_id,
                callable.as_ptr(),
            ))?;
            if trace {
                eprintln!("simpler_capi: run_prepared");
            }
            let result = SimplerApiError::from_code((run_prepared)(
                ctx.as_raw(),
                runtime.as_raw(),
                callable_id,
                args as *const _ as *const c_void,
                block_dim as c_int,
                aicpu_thread_num as c_int,
                0,
                0,
                0,
                0,
                output_prefix.as_ptr(),
            ));
            if trace {
                eprintln!("simpler_capi: unregister_callable");
            }
            if let Some(unregister_callable) = self.unregister_callable {
                let _ = (unregister_callable)(ctx.as_raw(), callable_id);
            }
            result
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

unsafe fn load_optional_symbol<T: Copy>(
    lib: &Library,
    symbol: &'static [u8],
) -> Result<Option<T>, SimplerApiError> {
    match lib.get::<T>(symbol) {
        Ok(symbol) => Ok(Some(*symbol)),
        Err(_) => Ok(None),
    }
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

const CHIP_MAX_TENSOR_ARGS: usize = 64;
const CHIP_MAX_SCALAR_ARGS: usize = 128;
const CHIP_MAX_CHILDREN: usize = 1024;
const CALLABLE_ALIGN: usize = 64;
const CALLABLE_FUNC_NAME_MAX: usize = 64;
const CORE_CALLABLE_BINARY_OFFSET: usize = 128;
const CHIP_CALLABLE_HEADER_SIZE: usize = 8596;

const _: () = assert!(std::mem::size_of::<ContinuousTensor>() == 40);
const _: () = assert!(std::mem::size_of::<ChipStorageTaskArgs>() == 3592);
