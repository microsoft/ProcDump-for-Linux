#![allow(unsafe_code)]

use procdump_core::config::{OutputSpec, Platform};
use procdump_core::dump::{DumpBackend, DumpKind, DumpRequest, PlatformDumpBackend};
use procdump_core::process::{ProcessDiscovery, ProcessId};
use std::ffi::{CStr, CString, c_char, c_int};
#[cfg(target_os = "linux")]
use std::fs;
use std::os::unix::ffi::OsStrExt;
use std::panic::{AssertUnwindSafe, catch_unwind};
use std::path::{Path, PathBuf};
use std::ptr;

#[cfg(target_os = "linux")]
use procdump_core::process::linux::LinuxProcfs as NativeProcesses;
#[cfg(target_os = "macos")]
use procdump_core::process::macos::MacOsProcesses as NativeProcesses;

const PD_DUMP_MASK_DEFAULT: c_int = -1;

/// Writes an on-demand process dump through the ProcDump C ABI.
///
/// # Safety
///
/// `dump_path` must be null or point to a readable NUL-terminated string. When
/// non-null, `error` must point to writable storage for a `char *`. Any error
/// returned through that storage must be released with [`pdFreeError`].
#[unsafe(no_mangle)]
pub unsafe extern "C" fn pdWriteDump(
    process_id: libc::pid_t,
    dump_path: *const c_char,
    dump_mask: c_int,
    overwrite: bool,
    error: *mut *mut c_char,
) -> c_int {
    set_error_pointer(error, ptr::null_mut());
    let result = catch_unwind(AssertUnwindSafe(|| {
        write_dump(process_id, dump_path, dump_mask, overwrite)
    }));
    match result {
        Ok(Ok(())) => 0,
        Ok(Err(message)) => {
            set_error(error, &message);
            -1
        }
        Err(_) => {
            set_error(error, "Failed to generate core dump: internal panic.");
            -1
        }
    }
}

/// Releases an error returned by [`pdWriteDump`].
///
/// # Safety
///
/// `error` must be null or a pointer returned by this library that has not
/// already been freed.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn pdFreeError(error: *mut c_char) {
    if !error.is_null() {
        drop(unsafe { CString::from_raw(error) });
    }
}

fn write_dump(
    process_id: libc::pid_t,
    dump_path: *const c_char,
    dump_mask: c_int,
    overwrite: bool,
) -> Result<(), String> {
    if process_id <= 0 || dump_path.is_null() {
        return Err("Invalid argument: a valid processId and dumpPath are required.".into());
    }
    let path_bytes = unsafe { CStr::from_ptr(dump_path) }.to_bytes();
    if path_bytes.is_empty() {
        return Err("Invalid argument: a valid processId and dumpPath are required.".into());
    }
    let path = PathBuf::from(std::ffi::OsStr::from_bytes(path_bytes));
    let directory = path.parent().unwrap_or_else(|| Path::new("."));
    if !directory.is_dir() {
        return Err(format!(
            "Invalid directory (\"{}\") provided for core dump output.",
            directory.display()
        ));
    }

    let pid = ProcessId::new(process_id).map_err(|error| error.to_string())?;
    let processes = NativeProcesses::new().map_err(|error| error.to_string())?;
    let process_name = processes.name(pid).map_err(|error| error.to_string())?;
    let _mask = CoreDumpMaskGuard::apply(pid, dump_mask)?;
    PlatformDumpBackend
        .write_dump(&DumpRequest {
            pid,
            process_name,
            kind: DumpKind::Manual,
            output: OutputSpec {
                directory: if directory.as_os_str().is_empty() {
                    PathBuf::from(".")
                } else {
                    directory.to_path_buf()
                },
                file_name: path.file_name().map(std::ffi::OsStr::to_owned),
            },
            overwrite,
            platform: native_platform(),
        })
        .map(|_| ())
        .map_err(|error| error.to_string())
}

#[cfg(target_os = "linux")]
fn native_platform() -> Platform {
    Platform::Linux
}

#[cfg(target_os = "macos")]
fn native_platform() -> Platform {
    Platform::MacOs
}

struct CoreDumpMaskGuard {
    #[cfg(target_os = "linux")]
    path: Option<PathBuf>,
    #[cfg(target_os = "linux")]
    previous: Option<u32>,
}

impl CoreDumpMaskGuard {
    #[cfg(target_os = "linux")]
    fn apply(pid: ProcessId, mask: c_int) -> Result<Self, String> {
        if mask == PD_DUMP_MASK_DEFAULT {
            return Ok(Self {
                path: None,
                previous: None,
            });
        }
        if mask < 0 {
            return Err("Invalid core dump mask specified.".into());
        }
        let path = PathBuf::from(format!("/proc/{}/coredump_filter", pid.get()));
        let previous_text = fs::read_to_string(&path)
            .map_err(|error| format!("Failed to read {}: {error}", path.display()))?;
        let previous = u32::from_str_radix(previous_text.trim(), 16).map_err(|_| {
            format!(
                "Failed to parse core dump mask from {}: {}",
                path.display(),
                previous_text.trim()
            )
        })?;
        fs::write(&path, mask.to_string())
            .map_err(|error| format!("Failed to write {}: {error}", path.display()))?;
        Ok(Self {
            path: Some(path),
            previous: Some(previous),
        })
    }

    #[cfg(target_os = "macos")]
    fn apply(_pid: ProcessId, mask: c_int) -> Result<Self, String> {
        if mask == PD_DUMP_MASK_DEFAULT {
            Ok(Self {})
        } else {
            Err("Custom core dump masks are not supported on macOS.".into())
        }
    }
}

impl Drop for CoreDumpMaskGuard {
    fn drop(&mut self) {
        #[cfg(target_os = "linux")]
        if let (Some(path), Some(previous)) = (&self.path, &self.previous) {
            let _ = fs::write(path, previous.to_string());
        }
    }
}

fn set_error(error: *mut *mut c_char, message: &str) {
    if error.is_null() {
        return;
    }
    let sanitized = message.replace('\0', " ");
    if let Ok(message) = CString::new(sanitized) {
        set_error_pointer(error, message.into_raw());
    }
}

fn set_error_pointer(error: *mut *mut c_char, value: *mut c_char) {
    if !error.is_null() {
        unsafe { error.write(value) };
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn rejects_invalid_arguments_and_allocates_error() {
        let mut error = ptr::null_mut();
        let result = unsafe { pdWriteDump(0, ptr::null(), -1, false, &raw mut error) };

        assert_eq!(result, -1);
        assert!(!error.is_null());
        let message = unsafe { CStr::from_ptr(error) }.to_string_lossy();
        assert!(message.contains("Invalid argument"));
        unsafe { pdFreeError(error) };
    }

    #[test]
    fn free_error_accepts_null() {
        unsafe { pdFreeError(ptr::null_mut()) };
    }
}
