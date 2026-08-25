#![allow(unsafe_code)]

use crate::config::{OutputSpec, Platform};
use crate::process::ProcessId;
use std::ffi::{CStr, OsStr, OsString};
use std::fmt;
use std::fs;
use std::io;
use std::path::{Path, PathBuf};
use std::process::Command;

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum DumpKind {
    Commit,
    Cpu,
    Thread,
    FileDescriptor,
    Signal,
    Timer,
    Exception,
    Manual,
    PerformanceCounter,
}

impl DumpKind {
    pub const fn descriptor(self) -> &'static str {
        match self {
            Self::Commit => "commit",
            Self::Cpu => "cpu",
            Self::Thread => "thread",
            Self::FileDescriptor => "filedesc",
            Self::Signal => "signal",
            Self::Timer => "time",
            Self::Exception => "exception",
            Self::Manual => "manual",
            Self::PerformanceCounter => "perfcounter",
        }
    }
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct DumpRequest {
    pub pid: ProcessId,
    pub process_name: OsString,
    pub kind: DumpKind,
    pub output: OutputSpec,
    pub overwrite: bool,
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
        #[cfg(target_os = "linux")]
        if request.platform == Platform::Linux {
            let socket = crate::dotnet::find_diagnostics_socket(request.pid)
                .map_err(|error| DumpError::DotNet(error.to_string()))?;
            if let Some(socket) = socket {
                let timestamp = local_timestamp()?;
                let paths = dump_paths(request, &timestamp)?;
                if paths.prefix.exists() && !request.overwrite {
                    return Err(DumpError::AlreadyExists(paths.prefix));
                }
                ensure_writable_directory(&request.output.directory)?;
                crate::dotnet::generate_dump(&socket, &paths.prefix)
                    .map_err(|error| DumpError::DotNet(error.to_string()))?;
                if !paths.prefix.is_file() {
                    return Err(DumpError::DotNet(format!(
                        ".NET runtime reported success but did not create {}",
                        paths.prefix.display()
                    )));
                }
                return Ok(paths.prefix);
            }
        }
        GcoreBackend.write_dump(request)
    }
}

impl DumpBackend for GcoreBackend {
    fn write_dump(&self, request: &DumpRequest) -> Result<PathBuf, DumpError> {
        let timestamp = local_timestamp()?;
        let paths = dump_paths(request, &timestamp)?;

        if paths.final_path.exists() && !request.overwrite {
            return Err(DumpError::AlreadyExists(paths.final_path));
        }
        ensure_writable_directory(&request.output.directory)?;

        let output_argument = match request.platform {
            Platform::Linux => &paths.prefix,
            Platform::MacOs => &paths.final_path,
        };
        let output = Command::new("gcore")
            .arg("-o")
            .arg(output_argument)
            .arg(request.pid.get().to_string())
            .output()
            .map_err(|source| DumpError::Start {
                program: "gcore",
                source,
            })?;

        if !output.status.success() || !paths.final_path.is_file() {
            remove_if_present(&paths.final_path);
            return Err(DumpError::Backend {
                status: output.status.code(),
                output: combined_output(&output.stdout, &output.stderr),
            });
        }

        Ok(paths.final_path)
    }
}

#[derive(Clone, Debug, Eq, PartialEq)]
struct DumpPaths {
    prefix: PathBuf,
    final_path: PathBuf,
}

fn dump_paths(request: &DumpRequest, timestamp: &str) -> Result<DumpPaths, DumpError> {
    let prefix = if let Some(file_name) = &request.output.file_name {
        request.output.directory.join(file_name)
    } else {
        let process_name = sanitize_process_name(&request.process_name);
        request.output.directory.join(format!(
            "{process_name}_{}_{timestamp}",
            request.kind.descriptor()
        ))
    };
    let final_path = prefix.with_file_name(format!(
        "{}.{}",
        prefix
            .file_name()
            .and_then(OsStr::to_str)
            .ok_or_else(|| DumpError::InvalidPath(prefix.clone()))?,
        request.pid.get()
    ));

    if !is_legacy_safe_path(&prefix) || !is_legacy_safe_path(&final_path) {
        return Err(DumpError::InvalidPath(final_path));
    }
    Ok(DumpPaths { prefix, final_path })
}

pub(crate) fn sidecar_path(request: &DumpRequest, extension: &str) -> Result<PathBuf, DumpError> {
    let paths = dump_paths(request, &local_timestamp()?)?;
    let file_name = paths
        .final_path
        .file_name()
        .and_then(OsStr::to_str)
        .ok_or_else(|| DumpError::InvalidPath(paths.final_path.clone()))?;
    Ok(paths
        .final_path
        .with_file_name(format!("{file_name}.{extension}")))
}

fn sanitize_process_name(name: &OsStr) -> String {
    name.to_string_lossy()
        .chars()
        .map(|character| {
            if character.is_alphanumeric() {
                character
            } else {
                '_'
            }
        })
        .collect()
}

fn is_legacy_safe_path(path: &Path) -> bool {
    path.to_string_lossy().chars().all(|character| {
        character.is_alphanumeric() || matches!(character, '/' | '.' | '_' | '-' | ' ')
    })
}

fn ensure_writable_directory(path: &Path) -> Result<(), DumpError> {
    if !path.is_dir() {
        return Err(DumpError::InvalidDirectory(path.to_path_buf()));
    }
    let metadata = fs::metadata(path).map_err(|source| DumpError::Io {
        operation: "inspect dump directory",
        path: path.to_path_buf(),
        source,
    })?;
    if metadata.permissions().readonly() {
        return Err(DumpError::InvalidDirectory(path.to_path_buf()));
    }
    Ok(())
}

fn combined_output(stdout: &[u8], stderr: &[u8]) -> String {
    let mut output = String::from_utf8_lossy(stdout).into_owned();
    output.push_str(&String::from_utf8_lossy(stderr));
    output
}

fn remove_if_present(path: &Path) {
    match fs::remove_file(path) {
        Ok(()) => {}
        Err(error) if error.kind() == io::ErrorKind::NotFound => {}
        Err(_) => {}
    }
}

fn local_timestamp() -> Result<String, DumpError> {
    let mut now = unsafe { std::mem::zeroed::<libc::time_t>() };
    if unsafe { libc::time(&mut now) } == -1 {
        return Err(DumpError::Clock);
    }
    let mut local = unsafe { std::mem::zeroed::<libc::tm>() };
    if unsafe { libc::localtime_r(&now, &mut local) }.is_null() {
        return Err(DumpError::Clock);
    }
    let format = c"%y%m%d_%H%M%S";
    let mut buffer = [0 as libc::c_char; 32];
    let length =
        unsafe { libc::strftime(buffer.as_mut_ptr(), buffer.len(), format.as_ptr(), &local) };
    if length == 0 {
        return Err(DumpError::Clock);
    }
    let value = unsafe { CStr::from_ptr(buffer.as_ptr()) };
    value
        .to_str()
        .map(str::to_owned)
        .map_err(|_| DumpError::Clock)
}

#[derive(Debug)]
pub enum DumpError {
    AlreadyExists(PathBuf),
    InvalidDirectory(PathBuf),
    InvalidPath(PathBuf),
    Start {
        program: &'static str,
        source: io::Error,
    },
    Backend {
        status: Option<i32>,
        output: String,
    },
    DotNet(String),
    Io {
        operation: &'static str,
        path: PathBuf,
        source: io::Error,
    },
    Clock,
}

impl fmt::Display for DumpError {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::AlreadyExists(path) => write!(
                formatter,
                "Dump file {} already exists and was not overwritten (use -o to overwrite)",
                path.display()
            ),
            Self::InvalidDirectory(path) => write!(
                formatter,
                "No write permission to core dump target directory: {}",
                path.display()
            ),
            Self::InvalidPath(path) => write!(
                formatter,
                "Invalid characters in core dump file path: {}",
                path.display()
            ),
            Self::Start { program, source } => {
                write!(formatter, "Failed to start {program}: {source}")
            }
            Self::Backend { status, output } => write!(
                formatter,
                "gcore failed to generate core dump (exit status {}): {}",
                status.map_or_else(|| "unknown".into(), |status| status.to_string()),
                output.trim()
            ),
            Self::DotNet(error) => formatter.write_str(error),
            Self::Io {
                operation,
                path,
                source,
            } => write!(
                formatter,
                "failed to {operation} {}: {source}",
                path.display()
            ),
            Self::Clock => write!(formatter, "failed to generate dump timestamp"),
        }
    }
}

impl std::error::Error for DumpError {
    fn source(&self) -> Option<&(dyn std::error::Error + 'static)> {
        match self {
            Self::Start { source, .. } | Self::Io { source, .. } => Some(source),
            _ => None,
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn request(output: OutputSpec) -> DumpRequest {
        DumpRequest {
            pid: ProcessId::new(42).unwrap(),
            process_name: OsString::from("worker pool[1]"),
            kind: DumpKind::Cpu,
            output,
            overwrite: false,
            platform: Platform::Linux,
        }
    }

    #[test]
    fn default_name_matches_legacy_pattern() {
        let paths = dump_paths(
            &request(OutputSpec {
                directory: PathBuf::from("/tmp"),
                file_name: None,
            }),
            "260825_101112",
        )
        .unwrap();

        assert_eq!(
            paths.prefix,
            PathBuf::from("/tmp/worker_pool_1__cpu_260825_101112")
        );
        assert_eq!(
            paths.final_path,
            PathBuf::from("/tmp/worker_pool_1__cpu_260825_101112.42")
        );
    }

    #[test]
    fn custom_name_only_appends_pid() {
        let paths = dump_paths(
            &request(OutputSpec {
                directory: PathBuf::from("/tmp"),
                file_name: Some(OsString::from("custom.core")),
            }),
            "ignored",
        )
        .unwrap();

        assert_eq!(paths.prefix, PathBuf::from("/tmp/custom.core"));
        assert_eq!(paths.final_path, PathBuf::from("/tmp/custom.core.42"));
    }

    #[test]
    fn unsafe_output_path_is_rejected() {
        let error = dump_paths(
            &request(OutputSpec {
                directory: PathBuf::from("/tmp/bad;$dir"),
                file_name: None,
            }),
            "260825_101112",
        )
        .unwrap_err();

        assert!(matches!(error, DumpError::InvalidPath(_)));
    }
}
