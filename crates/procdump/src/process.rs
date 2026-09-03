use std::ffi::OsString;
use std::fmt;
use std::io;
use std::path::PathBuf;

#[cfg(target_os = "linux")]
pub mod linux;
#[cfg(target_os = "macos")]
pub mod macos;

#[derive(Clone, Copy, Debug, Eq, Hash, Ord, PartialEq, PartialOrd)]
pub struct ProcessId(i32);

impl ProcessId {
    pub fn new(value: i32) -> Result<Self, ProcessError> {
        if value > 0 {
            Ok(Self(value))
        } else {
            Err(ProcessError::InvalidPid(value))
        }
    }

    pub const fn get(self) -> i32 {
        self.0
    }
}

#[derive(Clone, Copy, Debug, Eq, Hash, PartialEq)]
pub struct ProcessIdentity {
    pub pid: ProcessId,
    pub start_time: u64,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum ProcessState {
    Running,
    Sleeping,
    Waiting,
    Zombie,
    Stopped,
    Tracing,
    Dead,
    Other(char),
}

impl From<char> for ProcessState {
    fn from(value: char) -> Self {
        match value {
            'R' => Self::Running,
            'S' => Self::Sleeping,
            'D' => Self::Waiting,
            'Z' => Self::Zombie,
            'T' => Self::Stopped,
            't' => Self::Tracing,
            'X' | 'x' => Self::Dead,
            value => Self::Other(value),
        }
    }
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct ProcessSnapshot {
    pub identity: ProcessIdentity,
    pub name: OsString,
    pub state: ProcessState,
    pub parent_pid: i32,
    pub process_group: i32,
    pub cpu_time_ticks: u64,
    pub rss_bytes: u64,
    pub swap_bytes: u64,
    pub thread_count: u64,
    pub file_descriptor_count: u64,
}

impl ProcessSnapshot {
    pub fn commit_megabytes(&self) -> u64 {
        self.rss_bytes.saturating_add(self.swap_bytes) / (1024 * 1024)
    }
}

pub trait ProcessDiscovery: Send + Sync {
    fn list_processes(&self) -> Result<Vec<ProcessId>, ProcessError>;
    fn identity(&self, pid: ProcessId) -> Result<ProcessIdentity, ProcessError>;
    fn name(&self, pid: ProcessId) -> Result<OsString, ProcessError>;
    fn process_group(&self, pid: ProcessId) -> Result<i32, ProcessError>;
    fn is_alive(&self, identity: ProcessIdentity) -> Result<bool, ProcessError>;
}

pub trait ProcessMetrics: Send + Sync {
    fn sample(&self, pid: ProcessId) -> Result<ProcessSnapshot, ProcessError>;
    fn cpu_usage_percent(&self, snapshot: &ProcessSnapshot) -> Result<u32, ProcessError>;
}

#[derive(Debug)]
pub enum ProcessError {
    InvalidPid(i32),
    NameNotFound(OsString),
    GroupNotFound(i32),
    Io {
        operation: &'static str,
        path: PathBuf,
        source: io::Error,
    },
    InvalidData {
        path: PathBuf,
        detail: String,
    },
    Disappeared(ProcessId),
}

impl ProcessError {
    fn io(operation: &'static str, path: impl Into<PathBuf>, source: io::Error) -> Self {
        Self::Io {
            operation,
            path: path.into(),
            source,
        }
    }

    fn invalid_data(path: impl Into<PathBuf>, detail: impl Into<String>) -> Self {
        Self::InvalidData {
            path: path.into(),
            detail: detail.into(),
        }
    }
}

impl fmt::Display for ProcessError {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::InvalidPid(pid) => write!(formatter, "invalid process ID: {pid}"),
            Self::NameNotFound(name) => write!(
                formatter,
                "no process matching the specified name ({}) can be found",
                name.to_string_lossy()
            ),
            Self::GroupNotFound(group) => {
                write!(
                    formatter,
                    "no process matching process group {group} can be found"
                )
            }
            Self::Io {
                operation,
                path,
                source,
            } => write!(
                formatter,
                "failed to {operation} {}: {source}",
                path.display()
            ),
            Self::InvalidData { path, detail } => {
                write!(
                    formatter,
                    "invalid process data in {}: {detail}",
                    path.display()
                )
            }
            Self::Disappeared(pid) => write!(formatter, "process {} no longer exists", pid.get()),
        }
    }
}

impl std::error::Error for ProcessError {
    fn source(&self) -> Option<&(dyn std::error::Error + 'static)> {
        match self {
            Self::Io { source, .. } => Some(source),
            _ => None,
        }
    }
}
