#![allow(unsafe_code)]

use super::{
    ProcessDiscovery, ProcessError, ProcessId, ProcessIdentity, ProcessMetrics, ProcessSnapshot,
    ProcessState,
};
use std::ffi::{OsStr, OsString, c_char, c_int, c_void};
use std::io;
use std::mem::{size_of, zeroed};
use std::os::unix::ffi::OsStrExt;
use std::path::Path;
use std::time::{SystemTime, UNIX_EPOCH};

const PROC_ALL_PIDS: u32 = 1;
const PROC_PIDLISTFDS: c_int = 1;
const PROC_PIDTASKALLINFO: c_int = 2;
const PROC_PIDPATHINFO_MAXSIZE: usize = 4096;

#[derive(Clone, Debug, Default)]
pub struct MacOsProcesses;

impl MacOsProcesses {
    pub fn new() -> Result<Self, ProcessError> {
        Ok(Self)
    }

    fn task_info(&self, pid: ProcessId) -> Result<ProcTaskAllInfo, ProcessError> {
        let mut info = unsafe { zeroed::<ProcTaskAllInfo>() };
        let bytes = unsafe {
            proc_pidinfo(
                pid.get(),
                PROC_PIDTASKALLINFO,
                0,
                (&raw mut info).cast(),
                size_of::<ProcTaskAllInfo>() as c_int,
            )
        };
        if bytes != size_of::<ProcTaskAllInfo>() as c_int {
            return Err(last_process_error(pid, "proc_pidinfo(PROC_PIDTASKALLINFO)"));
        }
        Ok(info)
    }

    fn file_descriptor_count(&self, pid: ProcessId) -> Result<u64, ProcessError> {
        let required =
            unsafe { proc_pidinfo(pid.get(), PROC_PIDLISTFDS, 0, std::ptr::null_mut(), 0) };
        if required <= 0 {
            return Err(last_process_error(
                pid,
                "proc_pidinfo(PROC_PIDLISTFDS size)",
            ));
        }
        let slots = required as usize / size_of::<ProcFdInfo>() + 1;
        let mut descriptors = vec![ProcFdInfo::default(); slots];
        let bytes = unsafe {
            proc_pidinfo(
                pid.get(),
                PROC_PIDLISTFDS,
                0,
                descriptors.as_mut_ptr().cast(),
                (descriptors.len() * size_of::<ProcFdInfo>()) as c_int,
            )
        };
        if bytes < 0 {
            return Err(last_process_error(pid, "proc_pidinfo(PROC_PIDLISTFDS)"));
        }
        Ok(bytes as u64 / size_of::<ProcFdInfo>() as u64)
    }
}

impl ProcessDiscovery for MacOsProcesses {
    fn list_processes(&self) -> Result<Vec<ProcessId>, ProcessError> {
        let required = unsafe { proc_listpids(PROC_ALL_PIDS, 0, std::ptr::null_mut(), 0) };
        if required <= 0 {
            return Err(system_error("proc_listpids size"));
        }
        let mut pids = vec![0_i32; required as usize / size_of::<i32>() + 1];
        let bytes = unsafe {
            proc_listpids(
                PROC_ALL_PIDS,
                0,
                pids.as_mut_ptr().cast(),
                (pids.len() * size_of::<i32>()) as c_int,
            )
        };
        if bytes < 0 {
            return Err(system_error("proc_listpids"));
        }
        pids.truncate(bytes as usize / size_of::<i32>());
        let mut processes: Vec<_> = pids
            .into_iter()
            .filter_map(|pid| ProcessId::new(pid).ok())
            .collect();
        processes.sort_unstable();
        Ok(processes)
    }

    fn identity(&self, pid: ProcessId) -> Result<ProcessIdentity, ProcessError> {
        let info = self.task_info(pid)?;
        Ok(ProcessIdentity {
            pid,
            start_time: info.pbsd.start_tvsec,
        })
    }

    fn name(&self, pid: ProcessId) -> Result<OsString, ProcessError> {
        let mut path = vec![0_u8; PROC_PIDPATHINFO_MAXSIZE];
        let bytes = unsafe { proc_pidpath(pid.get(), path.as_mut_ptr().cast(), path.len() as u32) };
        if bytes <= 0 {
            return Err(last_process_error(pid, "proc_pidpath"));
        }
        path.truncate(bytes as usize);
        let path = OsStr::from_bytes(&path);
        Ok(Path::new(path)
            .file_name()
            .map_or_else(|| path.to_owned(), OsStr::to_owned))
    }

    fn process_group(&self, pid: ProcessId) -> Result<i32, ProcessError> {
        Ok(self.task_info(pid)?.pbsd.pgid as i32)
    }

    fn is_alive(&self, identity: ProcessIdentity) -> Result<bool, ProcessError> {
        match self.identity(identity.pid) {
            Ok(current) => Ok(current == identity),
            Err(ProcessError::Disappeared(_)) => Ok(false),
            Err(error) => Err(error),
        }
    }
}

impl ProcessMetrics for MacOsProcesses {
    fn sample(&self, pid: ProcessId) -> Result<ProcessSnapshot, ProcessError> {
        let info = self.task_info(pid)?;
        Ok(ProcessSnapshot {
            identity: ProcessIdentity {
                pid,
                start_time: info.pbsd.start_tvsec,
            },
            name: self.name(pid)?,
            state: bsd_state(info.pbsd.status),
            parent_pid: info.pbsd.ppid as i32,
            process_group: info.pbsd.pgid as i32,
            cpu_time_ticks: info
                .ptinfo
                .total_user
                .saturating_add(info.ptinfo.total_system),
            rss_bytes: info.ptinfo.resident_size,
            swap_bytes: 0,
            thread_count: info.ptinfo.thread_num.max(0) as u64,
            file_descriptor_count: self.file_descriptor_count(pid)?,
        })
    }

    fn cpu_usage_percent(&self, snapshot: &ProcessSnapshot) -> Result<u32, ProcessError> {
        let mut timebase = unsafe { zeroed::<MachTimebaseInfo>() };
        let result = unsafe { mach_timebase_info(&raw mut timebase) };
        if result != 0 || timebase.denom == 0 {
            return Err(ProcessError::invalid_data(
                "mach_timebase_info",
                format!("kernel result {result}"),
            ));
        }
        let now = SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .map_err(|_| ProcessError::invalid_data("clock", "time before Unix epoch"))?
            .as_secs();
        Ok(calculate_cpu_usage(
            snapshot.cpu_time_ticks,
            timebase.numer,
            timebase.denom,
            snapshot.identity.start_time,
            now,
        ))
    }
}

fn calculate_cpu_usage(
    absolute_time: u64,
    numerator: u32,
    denominator: u32,
    start_seconds: u64,
    now_seconds: u64,
) -> u32 {
    if denominator == 0 {
        return 0;
    }
    let cpu_seconds = absolute_time
        .saturating_mul(numerator as u64)
        .saturating_div(denominator as u64)
        .saturating_div(1_000_000_000);
    let elapsed = now_seconds.saturating_sub(start_seconds);
    if elapsed == 0 {
        0
    } else {
        cpu_seconds.saturating_mul(100).saturating_div(elapsed) as u32
    }
}

fn bsd_state(status: u32) -> ProcessState {
    match status {
        2 => ProcessState::Running,
        3 => ProcessState::Sleeping,
        4 => ProcessState::Stopped,
        5 => ProcessState::Zombie,
        _ => ProcessState::Other('?'),
    }
}

fn last_process_error(pid: ProcessId, operation: &'static str) -> ProcessError {
    let error = io::Error::last_os_error();
    if matches!(error.raw_os_error(), Some(libc::ESRCH)) {
        ProcessError::Disappeared(pid)
    } else {
        ProcessError::io(operation, format!("pid {}", pid.get()), error)
    }
}

fn system_error(operation: &'static str) -> ProcessError {
    ProcessError::io(operation, "system process list", io::Error::last_os_error())
}

#[repr(C)]
#[derive(Clone, Copy)]
struct ProcBsdInfo {
    flags: u32,
    status: u32,
    xstatus: u32,
    pid: u32,
    ppid: u32,
    uid: u32,
    gid: u32,
    ruid: u32,
    rgid: u32,
    svuid: u32,
    svgid: u32,
    rfu_1: u32,
    comm: [c_char; 16],
    name: [c_char; 32],
    nfiles: u32,
    pgid: u32,
    pjobc: u32,
    e_tdev: u32,
    e_tpgid: u32,
    nice: i32,
    start_tvsec: u64,
    start_tvusec: u64,
}

#[repr(C)]
#[derive(Clone, Copy)]
struct ProcTaskInfo {
    virtual_size: u64,
    resident_size: u64,
    total_user: u64,
    total_system: u64,
    threads_user: u64,
    threads_system: u64,
    policy: i32,
    faults: i32,
    pageins: i32,
    cow_faults: i32,
    messages_sent: i32,
    messages_received: i32,
    syscalls_mach: i32,
    syscalls_unix: i32,
    csw: i32,
    thread_num: i32,
    num_running: i32,
    priority: i32,
}

#[repr(C)]
#[derive(Clone, Copy)]
struct ProcTaskAllInfo {
    pbsd: ProcBsdInfo,
    ptinfo: ProcTaskInfo,
}

#[repr(C)]
#[derive(Clone, Copy, Default)]
struct ProcFdInfo {
    fd: i32,
    fd_type: u32,
}

#[repr(C)]
#[derive(Clone, Copy)]
struct MachTimebaseInfo {
    numer: u32,
    denom: u32,
}

unsafe extern "C" {
    fn proc_listpids(
        process_type: u32,
        type_info: u32,
        buffer: *mut c_void,
        buffer_size: c_int,
    ) -> c_int;
    fn proc_pidinfo(
        pid: c_int,
        flavor: c_int,
        arg: u64,
        buffer: *mut c_void,
        buffer_size: c_int,
    ) -> c_int;
    fn proc_pidpath(pid: c_int, buffer: *mut c_void, buffer_size: u32) -> c_int;
    fn mach_timebase_info(info: *mut MachTimebaseInfo) -> c_int;
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn calculates_lifetime_cpu_percentage() {
        assert_eq!(calculate_cpu_usage(25_000_000_000, 1, 1, 100, 200), 25);
    }
}
