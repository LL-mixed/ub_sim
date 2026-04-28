//! Thin Rust-side runtime loader for `simpler` host `pto_runtime_c_api`.
//!
//! The C API lives inside a platform/runtime-specific dynamic library produced
//! by `simpler`. This crate keeps `sim-runtime` free from raw symbol loading and
//! FFI boilerplate.

use std::ffi::{c_char, c_int, c_void, CString};
use std::fs;
use std::path::{Path, PathBuf};
use std::ptr::NonNull;
use std::time::{SystemTime, UNIX_EPOCH};

use libc::{free, malloc};
use libloading::os::unix::{Library, Symbol, RTLD_GLOBAL, RTLD_NOW};
use thiserror::Error;

pub type RuntimeHandle = *mut c_void;

type GetRuntimeSizeFn = unsafe extern "C" fn() -> usize;
type InitRuntimeFn = unsafe extern "C" fn(
    RuntimeHandle,
    *const u8,
    usize,
    *const c_char,
    *mut u64,
    c_int,
    *mut c_int,
    *mut u64,
    *const c_int,
    *const *const u8,
    *const usize,
    c_int,
) -> c_int;
type DeviceMallocFn = unsafe extern "C" fn(usize) -> *mut c_void;
type DeviceFreeFn = unsafe extern "C" fn(*mut c_void);
type CopyToDeviceFn = unsafe extern "C" fn(*mut c_void, *const c_void, usize) -> c_int;
type CopyFromDeviceFn = unsafe extern "C" fn(*mut c_void, *const c_void, usize) -> c_int;
type LaunchRuntimeFn = unsafe extern "C" fn(
    RuntimeHandle,
    c_int,
    c_int,
    c_int,
    *const u8,
    usize,
    *const u8,
    usize,
    c_int,
) -> c_int;
type FinalizeRuntimeFn = unsafe extern "C" fn(RuntimeHandle) -> c_int;
type SetDeviceFn = unsafe extern "C" fn(c_int) -> c_int;
type RecordTensorPairFn = unsafe extern "C" fn(RuntimeHandle, *mut c_void, *mut c_void, usize);
type EnableRuntimeProfilingFn = unsafe extern "C" fn(RuntimeHandle, c_int) -> c_int;

#[repr(i32)]
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum SimplerArgType {
    Scalar = 0,
    InputPtr = 1,
    OutputPtr = 2,
    InoutPtr = 3,
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
    #[error("null runtime pointer")]
    NullRuntime,
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
    get_runtime_size: GetRuntimeSizeFn,
    init_runtime: InitRuntimeFn,
    device_malloc: DeviceMallocFn,
    device_free: DeviceFreeFn,
    copy_to_device: CopyToDeviceFn,
    copy_from_device: CopyFromDeviceFn,
    launch_runtime: LaunchRuntimeFn,
    finalize_runtime: FinalizeRuntimeFn,
    set_device: SetDeviceFn,
    record_tensor_pair: RecordTensorPairFn,
    enable_runtime_profiling: EnableRuntimeProfilingFn,
}

impl std::fmt::Debug for RuntimeLibrary {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("RuntimeLibrary").finish_non_exhaustive()
    }
}

impl RuntimeLibrary {
    pub fn load(path: &Path) -> Result<Self, SimplerApiError> {
        let mut preloaded_libs = Vec::new();
        if let Some(sim_context_path) = std::env::var_os("SIMPLER_SIM_CONTEXT_LIBRARY") {
            let sim_context_path = Path::new(&sim_context_path);
            let lib = unsafe { Library::open(Some(sim_context_path), RTLD_NOW | RTLD_GLOBAL) }
                .map_err(|err| SimplerApiError::LoadLibrary(err.to_string()))?;
            preloaded_libs.push(lib);
        }
        let staged_path = stage_runtime_library(path)?;
        let lib = unsafe { Library::open(Some(&staged_path), RTLD_NOW | RTLD_GLOBAL) }
            .map_err(|err| SimplerApiError::LoadLibrary(err.to_string()))?;
        unsafe {
            Ok(Self {
                get_runtime_size: *load_symbol::<GetRuntimeSizeFn>(&lib, b"get_runtime_size\0")?,
                init_runtime: *load_symbol::<InitRuntimeFn>(&lib, b"init_runtime\0")?,
                device_malloc: *load_symbol::<DeviceMallocFn>(&lib, b"device_malloc\0")?,
                device_free: *load_symbol::<DeviceFreeFn>(&lib, b"device_free\0")?,
                copy_to_device: *load_symbol::<CopyToDeviceFn>(&lib, b"copy_to_device\0")?,
                copy_from_device: *load_symbol::<CopyFromDeviceFn>(&lib, b"copy_from_device\0")?,
                launch_runtime: *load_symbol::<LaunchRuntimeFn>(&lib, b"launch_runtime\0")?,
                finalize_runtime: *load_symbol::<FinalizeRuntimeFn>(&lib, b"finalize_runtime\0")?,
                set_device: *load_symbol::<SetDeviceFn>(&lib, b"set_device\0")?,
                record_tensor_pair: *load_symbol::<RecordTensorPairFn>(
                    &lib,
                    b"record_tensor_pair\0",
                )?,
                enable_runtime_profiling: *load_symbol::<EnableRuntimeProfilingFn>(
                    &lib,
                    b"enable_runtime_profiling\0",
                )?,
                _preloaded_libs: preloaded_libs,
                _lib: lib,
                _staged_path: staged_path,
            })
        }
    }

    pub fn runtime_size(&self) -> usize {
        unsafe { (self.get_runtime_size)() }
    }

    pub fn bind_device(&self, device_id: i32) -> Result<(), SimplerApiError> {
        unsafe { SimplerApiError::from_code((self.set_device)(device_id as c_int)) }
    }

    pub fn alloc_device(&self, size: usize) -> Result<DevicePtr, SimplerApiError> {
        let ptr = unsafe { (self.device_malloc)(size) };
        DevicePtr::from_raw(ptr).ok_or(SimplerApiError::NullDevicePointer)
    }

    pub fn free_device(&self, ptr: DevicePtr) {
        unsafe { (self.device_free)(ptr.as_ptr()) }
    }

    pub fn host_to_device(
        &self,
        dev_ptr: DevicePtr,
        host_ptr: *const c_void,
        size: usize,
    ) -> Result<(), SimplerApiError> {
        unsafe {
            SimplerApiError::from_code((self.copy_to_device)(dev_ptr.as_ptr(), host_ptr, size))
        }
    }

    pub fn device_to_host(
        &self,
        host_ptr: *mut c_void,
        dev_ptr: DevicePtr,
        size: usize,
    ) -> Result<(), SimplerApiError> {
        unsafe {
            SimplerApiError::from_code((self.copy_from_device)(host_ptr, dev_ptr.as_ptr(), size))
        }
    }

    pub fn init_runtime(
        &self,
        runtime: OwnedRuntime,
        orch_so_binary: *const u8,
        orch_so_size: usize,
        orch_func_name: &str,
        func_args: *mut u64,
        func_args_count: i32,
        arg_types: *mut i32,
        arg_sizes: *mut u64,
        kernel_func_ids: *const i32,
        kernel_binaries: *const *const u8,
        kernel_sizes: *const usize,
        kernel_count: i32,
    ) -> Result<(), SimplerApiError> {
        let orch_func_name =
            CString::new(orch_func_name).map_err(|_| SimplerApiError::InvalidSymbolName)?;
        unsafe {
            SimplerApiError::from_code((self.init_runtime)(
                runtime.as_raw(),
                orch_so_binary,
                orch_so_size,
                orch_func_name.as_ptr(),
                func_args,
                func_args_count as c_int,
                arg_types,
                arg_sizes,
                kernel_func_ids as *const c_int,
                kernel_binaries,
                kernel_sizes,
                kernel_count as c_int,
            ))
        }
    }

    pub fn launch_runtime(
        &self,
        runtime: OwnedRuntime,
        aicpu_thread_num: i32,
        block_dim: i32,
        device_id: i32,
        aicpu_binary: *const u8,
        aicpu_size: usize,
        aicore_binary: *const u8,
        aicore_size: usize,
        orch_thread_num: i32,
    ) -> Result<(), SimplerApiError> {
        unsafe {
            SimplerApiError::from_code((self.launch_runtime)(
                runtime.as_raw(),
                aicpu_thread_num as c_int,
                block_dim as c_int,
                device_id as c_int,
                aicpu_binary,
                aicpu_size,
                aicore_binary,
                aicore_size,
                orch_thread_num as c_int,
            ))
        }
    }

    pub fn finalize(&self, runtime: OwnedRuntime) -> Result<(), SimplerApiError> {
        unsafe { SimplerApiError::from_code((self.finalize_runtime)(runtime.as_raw())) }
    }

    pub fn set_profiling(
        &self,
        runtime: OwnedRuntime,
        enabled: bool,
    ) -> Result<(), SimplerApiError> {
        unsafe {
            SimplerApiError::from_code((self.enable_runtime_profiling)(
                runtime.as_raw(),
                if enabled { 1 } else { 0 },
            ))
        }
    }

    pub fn remember_tensor_pair(
        &self,
        runtime: OwnedRuntime,
        host_ptr: *mut c_void,
        dev_ptr: DevicePtr,
        size: usize,
    ) {
        unsafe { (self.record_tensor_pair)(runtime.as_raw(), host_ptr, dev_ptr.as_ptr(), size) }
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
