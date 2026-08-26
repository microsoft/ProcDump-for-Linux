#![allow(unsafe_code)]

#[cfg(target_os = "linux")]
mod corex;
#[cfg(target_os = "linux")]
mod dotnet;
mod engine;
mod mask;
mod process_name;

#[cfg(feature = "monitor")]
pub mod config;
#[cfg(feature = "monitor")]
pub mod dump;
#[cfg(all(target_os = "linux", feature = "dotnet-triggers"))]
mod eventpipe;
#[cfg(feature = "monitor")]
pub mod monitor;
#[cfg(feature = "monitor")]
pub mod orchestrator;
#[cfg(feature = "monitor")]
pub mod process;
#[cfg(all(target_os = "linux", feature = "dotnet-triggers"))]
mod profiler;
#[cfg(all(target_os = "linux", feature = "restrack"))]
mod restrack;
#[cfg(all(target_os = "linux", feature = "monitor"))]
mod signal;
#[cfg(feature = "monitor")]
pub mod sync;

use std::fmt;
use std::path::{Path, PathBuf};

#[derive(Clone, Debug, Default, Eq, PartialEq)]
#[non_exhaustive]
pub struct WriteDumpOptions {
    overwrite: bool,
    core_dump_mask: Option<u32>,
    use_gcore: bool,
}

impl WriteDumpOptions {
    pub fn overwrite(mut self, overwrite: bool) -> Self {
        self.overwrite = overwrite;
        self
    }

    pub fn core_dump_mask(mut self, core_dump_mask: u32) -> Self {
        self.core_dump_mask = Some(core_dump_mask);
        self
    }

    pub fn use_gcore(mut self, use_gcore: bool) -> Self {
        self.use_gcore = use_gcore;
        self
    }
}

pub fn write_dump(
    process_id: i32,
    dump_path: impl AsRef<Path>,
    options: WriteDumpOptions,
) -> Result<PathBuf, WriteDumpError> {
    if process_id <= 0 {
        return Err(WriteDumpError::InvalidArgument);
    }
    let dump_path = dump_path.as_ref();
    if dump_path.as_os_str().is_empty() {
        return Err(WriteDumpError::InvalidArgument);
    }
    let file_name = dump_path
        .file_name()
        .ok_or(WriteDumpError::InvalidArgument)?
        .to_owned();
    let directory = dump_path
        .parent()
        .filter(|path| !path.as_os_str().is_empty())
        .unwrap_or_else(|| Path::new("."));
    if !directory.is_dir() {
        return Err(WriteDumpError::InvalidDirectory(directory.to_path_buf()));
    }

    let process_name = process_name::name(process_id).map_err(WriteDumpError::Process)?;
    let _mask = mask::CoreDumpMaskGuard::apply(process_id, options.core_dump_mask)?;
    engine::write_dump(&engine::DumpRequest {
        pid: process_id,
        process_name,
        kind: engine::DumpKind::Manual,
        output: engine::OutputSpec {
            directory: directory.to_path_buf(),
            file_name: Some(file_name),
        },
        overwrite: options.overwrite,
        use_gcore: options.use_gcore,
        platform: engine::Platform::native()?,
    })
    .map_err(WriteDumpError::Dump)
}

#[derive(Debug)]
pub enum WriteDumpError {
    InvalidArgument,
    InvalidDirectory(PathBuf),
    InvalidCoreDumpMask,
    UnsupportedCoreDumpMask,
    UnsupportedPlatform,
    Process(String),
    Dump(engine::DumpError),
    Io {
        operation: &'static str,
        path: PathBuf,
        source: std::io::Error,
    },
}

impl fmt::Display for WriteDumpError {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::InvalidArgument => formatter
                .write_str("Invalid argument: a valid processId and dumpPath are required."),
            Self::InvalidDirectory(path) => write!(
                formatter,
                "Invalid directory (\"{}\") provided for core dump output.",
                path.display()
            ),
            Self::InvalidCoreDumpMask => formatter.write_str("Invalid core dump mask specified."),
            Self::UnsupportedCoreDumpMask => {
                formatter.write_str("Custom core dump masks are not supported on macOS.")
            }
            Self::UnsupportedPlatform => formatter.write_str("this platform is not supported"),
            Self::Process(message) => formatter.write_str(message),
            Self::Dump(error) => error.fmt(formatter),
            Self::Io {
                operation,
                path,
                source,
            } => write!(
                formatter,
                "Failed to {operation} {}: {source}",
                path.display()
            ),
        }
    }
}

impl std::error::Error for WriteDumpError {
    fn source(&self) -> Option<&(dyn std::error::Error + 'static)> {
        match self {
            Self::Dump(error) => Some(error),
            Self::Io { source, .. } => Some(source),
            _ => None,
        }
    }
}

#[doc(hidden)]
pub mod internal {
    #[cfg(target_os = "linux")]
    pub use crate::dotnet::find_diagnostics_socket;
    pub use crate::engine::{
        DumpBackend, DumpError, DumpKind, DumpRequest, GcoreBackend, OutputSpec, Platform,
        PlatformDumpBackend, sidecar_path,
    };
}
