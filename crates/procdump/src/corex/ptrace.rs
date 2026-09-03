#![allow(unsafe_code)]

use super::procfs;
use super::{CorexError, ThreadState};
use std::collections::HashSet;
use std::ffi::c_void;
use std::io;
use std::mem::size_of;
use std::time::{Duration, Instant};

const WAIT_ALL_THREADS: i32 = 0x4000_0000;
const NT_PRSTATUS: usize = 1;
const NT_PRFPREG: usize = 2;
#[cfg(target_arch = "aarch64")]
const NT_ARM_PAC_MASK: usize = 0x406;

pub(super) struct AttachedThreads {
    tids: Vec<i32>,
}

impl AttachedThreads {
    pub(super) fn attach_process(
        pid: i32,
        cancellation: Option<&crate::engine::CancellationToken>,
    ) -> Result<Self, CorexError> {
        let mut attached = Self { tids: Vec::new() };
        let mut known = HashSet::new();
        loop {
            let tids = procfs::enumerate_threads(pid)?;
            let additions: Vec<_> = tids.into_iter().filter(|tid| known.insert(*tid)).collect();
            if additions.is_empty() {
                break;
            }
            for tid in additions {
                check_cancelled(cancellation)?;
                if !attach(tid)? {
                    continue;
                }
                attached.tids.push(tid);
                wait_stopped(tid, cancellation)?;
            }
        }
        attached
            .tids
            .sort_unstable_by_key(|tid| (*tid != pid, *tid));
        Ok(attached)
    }

    pub(super) fn tids(&self) -> &[i32] {
        &self.tids
    }

    pub(super) fn finish(mut self) -> Result<(), CorexError> {
        self.detach_all()
    }

    fn detach_all(&mut self) -> Result<(), CorexError> {
        let mut failed = Vec::new();
        let mut first_error = None;
        while let Some(tid) = self.tids.pop() {
            let result = unsafe {
                libc::ptrace(
                    libc::PTRACE_DETACH,
                    tid,
                    std::ptr::null_mut::<c_void>(),
                    std::ptr::null_mut::<c_void>(),
                )
            };
            if result == -1 {
                let error = io::Error::last_os_error();
                first_error.get_or_insert_with(|| {
                    CorexError::Ptrace(format!("failed to detach from thread {tid}: {error}"))
                });
                failed.push(tid);
            }
        }
        self.tids = failed;
        first_error.map_or(Ok(()), Err)
    }
}

impl Drop for AttachedThreads {
    fn drop(&mut self) {
        if let Err(error) = self.detach_all() {
            eprintln!("warning: {error}");
        }
    }
}

pub(super) fn read_thread_states(
    tids: &[i32],
    cancellation: Option<&crate::engine::CancellationToken>,
) -> Result<Vec<ThreadState>, CorexError> {
    tids.iter()
        .map(|tid| {
            check_cancelled(cancellation)?;
            Ok(ThreadState {
                tid: *tid,
                gp_regs: read_regset(*tid, NT_PRSTATUS, gp_regset_size())?,
                fp_regs: read_regset(*tid, NT_PRFPREG, fp_regset_size())?,
                pac_mask: read_pac_mask(*tid),
            })
        })
        .collect()
}

fn check_cancelled(
    cancellation: Option<&crate::engine::CancellationToken>,
) -> Result<(), CorexError> {
    if cancellation.is_some_and(crate::engine::CancellationToken::is_cancelled) {
        Err(CorexError::Cancelled)
    } else {
        Ok(())
    }
}

fn attach(tid: i32) -> Result<bool, CorexError> {
    let result = unsafe {
        libc::ptrace(
            libc::PTRACE_ATTACH,
            tid,
            std::ptr::null_mut::<c_void>(),
            std::ptr::null_mut::<c_void>(),
        )
    };
    if result == -1 {
        let error = io::Error::last_os_error();
        if error.raw_os_error() == Some(libc::ESRCH) {
            return Ok(false);
        }
        return Err(CorexError::Ptrace(format!(
            "failed to attach to thread {tid}: {}",
            error
        )));
    }
    Ok(true)
}

fn wait_stopped(
    tid: i32,
    cancellation: Option<&crate::engine::CancellationToken>,
) -> Result<(), CorexError> {
    let mut status = 0;
    let deadline = Instant::now() + Duration::from_secs(10);
    loop {
        check_cancelled(cancellation)?;
        let result = unsafe { libc::waitpid(tid, &mut status, WAIT_ALL_THREADS | libc::WNOHANG) };
        if result == tid {
            break;
        }
        if result == 0 {
            if Instant::now() >= deadline {
                return Err(CorexError::Ptrace(format!(
                    "timed out waiting for thread {tid} to stop"
                )));
            }
            std::thread::sleep(Duration::from_millis(10));
            continue;
        }
        let error = io::Error::last_os_error();
        if error.kind() != io::ErrorKind::Interrupted {
            return Err(CorexError::Ptrace(format!(
                "failed to wait for thread {tid}: {error}"
            )));
        }
    }
    if !libc::WIFSTOPPED(status) {
        return Err(CorexError::Ptrace(format!(
            "thread {tid} did not stop after ptrace attach"
        )));
    }
    Ok(())
}

fn read_regset(tid: i32, note: usize, expected_size: usize) -> Result<Vec<u8>, CorexError> {
    let mut data = vec![0_u8; expected_size];
    let mut iovec = libc::iovec {
        iov_base: data.as_mut_ptr().cast(),
        iov_len: data.len(),
    };
    let result = unsafe {
        libc::ptrace(
            libc::PTRACE_GETREGSET,
            tid,
            note as *mut c_void,
            (&raw mut iovec).cast::<c_void>(),
        )
    };
    if result == -1 {
        return Err(CorexError::Ptrace(format!(
            "failed to read register set {note:#x} for thread {tid}: {}",
            io::Error::last_os_error()
        )));
    }
    if iovec.iov_len != expected_size {
        return Err(CorexError::Ptrace(format!(
            "register set {note:#x} for thread {tid} had size {}, expected {expected_size}",
            iovec.iov_len
        )));
    }
    Ok(data)
}

#[cfg(target_arch = "aarch64")]
fn read_pac_mask(tid: i32) -> Option<[u64; 2]> {
    let data = read_regset(tid, NT_ARM_PAC_MASK, size_of::<[u64; 2]>()).ok()?;
    Some([
        u64::from_ne_bytes(data[0..8].try_into().unwrap()),
        u64::from_ne_bytes(data[8..16].try_into().unwrap()),
    ])
}

#[cfg(not(target_arch = "aarch64"))]
fn read_pac_mask(_tid: i32) -> Option<[u64; 2]> {
    None
}

#[cfg(target_arch = "aarch64")]
const fn gp_regset_size() -> usize {
    size_of::<libc::user_regs_struct>()
}

#[cfg(target_arch = "aarch64")]
const fn fp_regset_size() -> usize {
    size_of::<libc::user_fpsimd_struct>()
}

#[cfg(target_arch = "x86_64")]
const fn gp_regset_size() -> usize {
    size_of::<libc::user_regs_struct>()
}

#[cfg(target_arch = "x86_64")]
const fn fp_regset_size() -> usize {
    size_of::<libc::user_fpregs_struct>()
}

#[cfg(not(any(target_arch = "aarch64", target_arch = "x86_64")))]
compile_error!("Rust corex supports only aarch64 and x86_64");

#[cfg(test)]
mod tests {
    use super::*;

    #[cfg(target_arch = "aarch64")]
    #[test]
    fn aarch64_register_layouts_match_kernel_uapi() {
        assert_eq!(gp_regset_size(), 272);
        assert_eq!(fp_regset_size(), 528);
    }

    #[test]
    fn cancellation_stops_corex_work() {
        let cancellation = crate::engine::CancellationToken::default();
        cancellation.cancel();

        assert!(matches!(
            check_cancelled(Some(&cancellation)),
            Err(CorexError::Cancelled)
        ));
    }

    #[cfg(target_arch = "x86_64")]
    #[test]
    fn x86_64_register_layouts_match_kernel_uapi() {
        assert_eq!(gp_regset_size(), 216);
        assert_eq!(fp_regset_size(), 512);
    }
}
