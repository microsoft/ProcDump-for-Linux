use crate::config::{OutputSpec, Platform};
use crate::process::ProcessId;
use std::ffi::OsString;
use std::path::PathBuf;

pub use crate::engine::{DumpError, DumpKind};

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct DumpRequest {
    pub pid: ProcessId,
    pub process_name: OsString,
    pub kind: DumpKind,
    pub output: OutputSpec,
    pub overwrite: bool,
    pub use_gcore: bool,
    pub platform: Platform,
}

pub trait DumpBackend: Send + Sync {
    fn write_dump(&self, request: &DumpRequest) -> Result<PathBuf, DumpError>;
}

#[derive(Clone, Debug, Default)]
pub struct GcoreBackend;

#[derive(Clone, Debug, Default)]
pub struct PlatformDumpBackend;

impl DumpBackend for PlatformDumpBackend {
    fn write_dump(&self, request: &DumpRequest) -> Result<PathBuf, DumpError> {
        crate::engine::DumpBackend::write_dump(
            &crate::engine::PlatformDumpBackend,
            &to_internal_request(request),
        )
    }
}

impl DumpBackend for GcoreBackend {
    fn write_dump(&self, request: &DumpRequest) -> Result<PathBuf, DumpError> {
        crate::engine::DumpBackend::write_dump(
            &crate::engine::GcoreBackend,
            &to_internal_request(request),
        )
    }
}

#[cfg(all(target_os = "linux", feature = "restrack"))]
pub(crate) fn sidecar_path(request: &DumpRequest, extension: &str) -> Result<PathBuf, DumpError> {
    crate::engine::sidecar_path(&to_internal_request(request), extension)
}

fn to_internal_request(request: &DumpRequest) -> crate::engine::DumpRequest {
    crate::engine::DumpRequest {
        pid: request.pid.get(),
        process_name: request.process_name.clone(),
        kind: request.kind,
        output: crate::engine::OutputSpec {
            directory: request.output.directory.clone(),
            file_name: request.output.file_name.clone(),
        },
        overwrite: request.overwrite,
        use_gcore: request.use_gcore,
        platform: match request.platform {
            Platform::Linux => crate::engine::Platform::Linux,
            Platform::MacOs => crate::engine::Platform::MacOs,
        },
    }
}
