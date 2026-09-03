#![allow(unsafe_code)]

use procdump::{WriteDumpError, WriteDumpOptions};
use std::ffi::{CString, c_char, c_int};
use std::os::unix::ffi::OsStrExt;
use std::panic::{AssertUnwindSafe, catch_unwind};
use std::path::Path;
use std::ptr;

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
        return Err(WriteDumpError::InvalidArgument.to_string());
    }
    let path_bytes = unsafe { std::ffi::CStr::from_ptr(dump_path) }.to_bytes();
    if path_bytes.is_empty() {
        return Err(WriteDumpError::InvalidArgument.to_string());
    }
    let core_dump_mask = match dump_mask {
        PD_DUMP_MASK_DEFAULT => None,
        value if value < 0 => return Err(WriteDumpError::InvalidCoreDumpMask.to_string()),
        value => Some(value as u32),
    };
    let path = Path::new(std::ffi::OsStr::from_bytes(path_bytes));
    procdump::write_dump(
        process_id,
        path,
        core_dump_mask
            .map_or_else(WriteDumpOptions::default, |mask| {
                WriteDumpOptions::default().core_dump_mask(mask)
            })
            .overwrite(overwrite),
    )
    .map(|_| ())
    .map_err(|error| error.to_string())
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
    use std::ffi::CStr;

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
    fn rejects_negative_non_default_mask() {
        let path = CString::new("/tmp/core").unwrap();
        let mut error = ptr::null_mut();
        let result = unsafe { pdWriteDump(1, path.as_ptr(), -2, false, &raw mut error) };

        assert_eq!(result, -1);
        assert_eq!(
            unsafe { CStr::from_ptr(error) }.to_string_lossy(),
            "Invalid core dump mask specified."
        );
        unsafe { pdFreeError(error) };
    }

    #[test]
    fn free_error_accepts_null() {
        unsafe { pdFreeError(ptr::null_mut()) };
    }
}
