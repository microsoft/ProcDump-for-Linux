mod elf;
mod notes;
mod procfs;
mod ptrace;

use std::fmt;
use std::fs::File;
use std::path::Path;

#[derive(Clone, Debug)]
pub(super) struct Mapping {
    pub start: u64,
    pub end: u64,
    pub flags: u32,
    pub offset: u64,
    pub is_shared: bool,
    pub is_file_backed: bool,
    pub should_dump: bool,
    pub path: String,
}

#[derive(Clone, Debug)]
pub(super) struct ProcessInfo {
    pub pid: i32,
    pub ppid: i32,
    pub pgrp: i32,
    pub sid: i32,
    pub uid: u32,
    pub gid: u32,
    pub comm: Vec<u8>,
    pub exe: String,
    pub cmdline: Vec<u8>,
    pub mappings: Vec<Mapping>,
    pub auxv: Vec<u8>,
    pub coredump_filter: u32,
    pub page_size: u64,
    pub tids: Vec<i32>,
}

#[derive(Clone, Debug)]
pub(super) struct ThreadState {
    pub tid: i32,
    pub gp_regs: Vec<u8>,
    pub fp_regs: Vec<u8>,
    pub pac_mask: Option<[u64; 2]>,
}

pub(crate) fn dump_pid(
    pid: i32,
    path: &Path,
    output: File,
    cancellation: Option<&crate::engine::CancellationToken>,
) -> Result<(), CorexError> {
    let attached = ptrace::AttachedThreads::attach_process(pid, cancellation)?;
    let mut process = procfs::read_process(pid)?;
    process.tids = attached.tids().to_vec();
    procfs::apply_coredump_filter(&mut process)?;
    let threads = ptrace::read_thread_states(attached.tids(), cancellation)?;
    let notes = notes::build(&process, &threads)?;
    let write_result = elf::write(path, output, &process, &notes, cancellation);
    let detach_result = attached.finish();
    match (write_result, detach_result) {
        (Ok(()), Ok(())) => Ok(()),
        (Err(error), Ok(())) | (Ok(()), Err(error)) => Err(error),
        (Err(write_error), Err(detach_error)) => Err(CorexError::InvalidData(format!(
            "{write_error}; additionally, cleanup failed: {detach_error}"
        ))),
    }
}

#[derive(Debug)]
pub(crate) enum CorexError {
    Io {
        operation: &'static str,
        path: String,
        source: std::io::Error,
    },
    InvalidData(String),
    Ptrace(String),
    Cancelled,
}

impl CorexError {
    pub(super) fn io(
        operation: &'static str,
        path: impl Into<String>,
        source: std::io::Error,
    ) -> Self {
        Self::Io {
            operation,
            path: path.into(),
            source,
        }
    }
}

impl fmt::Display for CorexError {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::Io {
                operation,
                path,
                source,
            } => write!(formatter, "failed to {operation} {path}: {source}"),
            Self::InvalidData(message) | Self::Ptrace(message) => formatter.write_str(message),
            Self::Cancelled => formatter.write_str("core dump generation was cancelled"),
        }
    }
}

impl std::error::Error for CorexError {
    fn source(&self) -> Option<&(dyn std::error::Error + 'static)> {
        match self {
            Self::Io { source, .. } => Some(source),
            _ => None,
        }
    }
}
