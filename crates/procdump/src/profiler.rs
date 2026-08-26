#![allow(unsafe_code)]

use crate::config::{Config, DotNetTrigger, GcHeap};
use crate::monitor::MonitorError;
use crate::process::ProcessIdentity;
use crate::sync::{MonitorControl, WaitOutcome};
use std::fs::{self, File, OpenOptions};
use std::io::{self, Read, Write};
use std::os::unix::fs::PermissionsExt;
use std::os::unix::net::{UnixListener, UnixStream};
use std::path::{Path, PathBuf};
use std::sync::Arc;
use std::thread::{self, JoinHandle};
use std::time::Duration;

const PROFILER_BYTES: &[u8] = include_bytes!(concat!(env!("OUT_DIR"), "/procdumpprofiler.so"));
const PROFILER_GUID: [u8; 16] = [
    0x1e, 0x82, 0x0d, 0xcf, 0x9b, 0x29, 0x07, 0x53, 0xa3, 0xd8, 0xb2, 0x83, 0xc0, 0x39, 0x16, 0xdd,
];
const ATTACH_TIMEOUT_MS: u32 = 5_000;
const PROFILER_COMMAND_SET: u8 = 0x03;
const PROFILER_COMMAND_ID: u8 = 0x01;
const MAX_STATUS_PAYLOAD: usize = 16 * 1024;

pub(crate) fn spawn_profiler_monitor(
    config: &Config,
    control: Arc<MonitorControl>,
    identity: ProcessIdentity,
) -> Result<JoinHandle<Result<(), MonitorError>>, MonitorError> {
    let trigger = config
        .dotnet_trigger
        .clone()
        .ok_or(MonitorError::UnsupportedTrigger)?;
    let output = config.output.clone();
    let dump_count = config.dump_count;
    let exception_filter = config.exception_filter.clone();
    thread::Builder::new()
        .name("dotnet profiler monitor".into())
        .spawn(move || {
            if control.wait_for_start() == WaitOutcome::Quit {
                return Ok(());
            }
            run_profiler(
                &control,
                identity,
                trigger,
                output,
                dump_count,
                exception_filter,
            )
            .map_err(|error| MonitorError::Profiler(error.to_string()))
        })
        .map_err(MonitorError::Spawn)
}

fn run_profiler(
    control: &MonitorControl,
    identity: ProcessIdentity,
    trigger: DotNetTrigger,
    output: crate::config::OutputSpec,
    dump_count: u32,
    exception_filter: Option<std::ffi::OsString>,
) -> Result<(), ProfilerError> {
    let procdump_pid = std::process::id();
    let directory = temporary_directory().join("procdump");
    fs::create_dir_all(&directory).map_err(ProfilerError::CreateDirectory)?;
    let profiler_path = directory.join("procdumpprofiler.so");
    extract_profiler(&profiler_path)?;
    prepare_profiler_log();

    let socket_path = directory.join(format!(
        "procdump-status-{procdump_pid}-{}",
        identity.pid.get()
    ));
    remove_socket(&socket_path);
    let listener = UnixListener::bind(&socket_path).map_err(ProfilerError::BindStatus)?;
    fs::set_permissions(&socket_path, fs::Permissions::from_mode(0o777))
        .map_err(ProfilerError::StatusPermissions)?;
    listener
        .set_nonblocking(true)
        .map_err(ProfilerError::ConfigureStatus)?;
    let _socket_guard = SocketGuard(socket_path.clone());

    let dump_path = if let Some(name) = output.file_name {
        output.directory.join(name).to_string_lossy().into_owned()
    } else {
        format!(
            "{}/",
            output.directory.to_string_lossy().trim_end_matches('/')
        )
    };
    let client_data = build_client_data(
        &trigger,
        &dump_path,
        procdump_pid,
        dump_count,
        exception_filter.as_deref(),
    )?;
    attach_profiler(identity, &profiler_path, client_data.as_bytes())?;

    let mut collected = 0_u32;
    while !control.is_quit_requested() {
        match listener.accept() {
            Ok((mut stream, _)) => match read_status(&mut stream)? {
                ProfilerStatus::Dump(path) => {
                    println!("Core dump generated: {}", path.display());
                    collected += 1;
                    if collected >= dump_count {
                        control.request_quit();
                        return Ok(());
                    }
                }
                ProfilerStatus::Health => {}
                ProfilerStatus::Failure(path) => {
                    return Err(ProfilerError::ProfilerFailure(path));
                }
            },
            Err(error) if error.kind() == io::ErrorKind::WouldBlock => {
                if unsafe { libc::kill(identity.pid.get(), 0) } == -1 {
                    return Err(ProfilerError::TargetExited);
                }
                thread::sleep(Duration::from_millis(25));
            }
            Err(error) => return Err(ProfilerError::AcceptStatus(error)),
        }
    }
    Ok(())
}

fn extract_profiler(path: &Path) -> Result<(), ProfilerError> {
    let mut file = File::create(path).map_err(ProfilerError::Extract)?;
    file.write_all(PROFILER_BYTES)
        .map_err(ProfilerError::Extract)?;
    file.flush().map_err(ProfilerError::Extract)?;
    fs::set_permissions(path, fs::Permissions::from_mode(0o744)).map_err(ProfilerError::Extract)
}

fn prepare_profiler_log() {
    let path = Path::new("/var/tmp/procdumpprofiler.log");
    if OpenOptions::new()
        .create(true)
        .append(true)
        .open(path)
        .is_ok()
    {
        let _ = fs::set_permissions(path, fs::Permissions::from_mode(0o666));
    }
}

fn attach_profiler(
    identity: ProcessIdentity,
    profiler_path: &Path,
    client_data: &[u8],
) -> Result<(), ProfilerError> {
    let socket = crate::internal::find_diagnostics_socket(identity.pid.get())
        .map_err(|error| ProfilerError::Diagnostics(error.to_string()))?
        .ok_or(ProfilerError::NotManaged)?;
    let packet = build_attach_packet(profiler_path, client_data)?;
    let mut stream = UnixStream::connect(&socket).map_err(ProfilerError::ConnectDiagnostics)?;
    stream
        .write_all(&packet)
        .map_err(ProfilerError::SendAttach)?;
    let mut header = [0_u8; 20];
    stream
        .read_exact(&mut header)
        .map_err(ProfilerError::ReceiveAttach)?;
    let size = u16::from_le_bytes([header[14], header[15]]);
    if size != 24 {
        return Err(ProfilerError::InvalidAttachResponse(size));
    }
    let mut result = [0_u8; 4];
    stream
        .read_exact(&mut result)
        .map_err(ProfilerError::ReceiveAttach)?;
    let result = i32::from_le_bytes(result);
    if result != 0 {
        return Err(ProfilerError::AttachRuntime(result));
    }
    Ok(())
}

fn build_attach_packet(path: &Path, client_data: &[u8]) -> Result<Vec<u8>, ProfilerError> {
    let path = path
        .to_str()
        .ok_or_else(|| ProfilerError::InvalidProfilerPath(path.to_path_buf()))?;
    let mut path_utf16: Vec<u16> = path.encode_utf16().collect();
    path_utf16.push(0);
    let path_len = u32::try_from(path_utf16.len()).map_err(|_| ProfilerError::PacketTooLarge)?;
    let client_len =
        u32::try_from(client_data.len() + 1).map_err(|_| ProfilerError::PacketTooLarge)?;
    let payload_size = 4 + 16 + 4 + path_utf16.len() * 2 + 4 + client_data.len() + 1;
    let packet_size =
        u16::try_from(20 + payload_size).map_err(|_| ProfilerError::PacketTooLarge)?;
    let mut packet = Vec::with_capacity(packet_size as usize);
    packet.extend_from_slice(b"DOTNET_IPC_V1\0");
    packet.extend_from_slice(&packet_size.to_le_bytes());
    packet.push(PROFILER_COMMAND_SET);
    packet.push(PROFILER_COMMAND_ID);
    packet.extend_from_slice(&0_u16.to_le_bytes());
    packet.extend_from_slice(&ATTACH_TIMEOUT_MS.to_le_bytes());
    packet.extend_from_slice(&PROFILER_GUID);
    packet.extend_from_slice(&path_len.to_le_bytes());
    for character in path_utf16 {
        packet.extend_from_slice(&character.to_le_bytes());
    }
    packet.extend_from_slice(&client_len.to_le_bytes());
    packet.extend_from_slice(client_data);
    packet.push(0);
    Ok(packet)
}

fn build_client_data(
    trigger: &DotNetTrigger,
    dump_path: &str,
    procdump_pid: u32,
    dump_count: u32,
    exception_filter: Option<&std::ffi::OsStr>,
) -> Result<String, ProfilerError> {
    let suffix = match trigger {
        DotNetTrigger::Exception => encode_exception_filter(exception_filter, dump_count)?,
        DotNetTrigger::GcMemory {
            heap,
            thresholds_mb,
        } => {
            let generation = match heap {
                GcHeap::Cumulative => 2008,
                GcHeap::Generation(generation) => i32::from(*generation),
                GcHeap::LargeObject => 3,
                GcHeap::PinnedObject => 4,
            };
            format!(
                "{generation};{}",
                thresholds_mb
                    .iter()
                    .map(u64::to_string)
                    .collect::<Vec<_>>()
                    .join(";")
            )
        }
        DotNetTrigger::GcGeneration(generation) => generation.to_string(),
    };
    let trigger_type = match trigger {
        DotNetTrigger::Exception => 6,
        DotNetTrigger::GcMemory { .. } => 7,
        DotNetTrigger::GcGeneration(_) => 8,
    };
    Ok(format!(
        "{trigger_type};{dump_path};{procdump_pid};{suffix}"
    ))
}

fn encode_exception_filter(
    filter: Option<&std::ffi::OsStr>,
    dump_count: u32,
) -> Result<String, ProfilerError> {
    let filter = filter.and_then(std::ffi::OsStr::to_str).unwrap_or("*");
    let mut encoded = String::new();
    for value in filter.split(',') {
        if value.is_empty() {
            return Err(ProfilerError::InvalidExceptionFilter);
        }
        let starts = value.starts_with('*');
        let ends = value.ends_with('*');
        match (starts, ends) {
            (false, false) => encoded.push_str(&format!("*{value}*:{dump_count};")),
            (false, true) => encoded.push_str(&format!("*{value}:{dump_count};")),
            (true, false) => encoded.push_str(&format!("{value}*:{dump_count};")),
            (true, true) => encoded.push_str(&format!("{value}:{dump_count};")),
        }
    }
    Ok(encoded)
}

fn read_status(stream: &mut UnixStream) -> Result<ProfilerStatus, ProfilerError> {
    stream
        .set_read_timeout(Some(Duration::from_secs(5)))
        .map_err(ProfilerError::ConfigureStatus)?;
    let mut length = [0_u8; 4];
    stream
        .read_exact(&mut length)
        .map_err(ProfilerError::ReadStatus)?;
    let length = u32::from_le_bytes(length) as usize;
    if !(5..=MAX_STATUS_PAYLOAD).contains(&length) {
        return Err(ProfilerError::InvalidStatusLength(length));
    }
    let mut payload = vec![0_u8; length];
    stream
        .read_exact(&mut payload)
        .map_err(ProfilerError::ReadStatus)?;
    let status = payload[0];
    let path_length = u32::from_le_bytes(payload[1..5].try_into().unwrap()) as usize;
    if path_length > payload.len() - 5 {
        return Err(ProfilerError::InvalidStatusLength(path_length));
    }
    let path = PathBuf::from(String::from_utf8_lossy(&payload[5..5 + path_length]).into_owned());
    match status {
        b'1' => Ok(ProfilerStatus::Dump(path)),
        b'H' => Ok(ProfilerStatus::Health),
        b'2' | b'F' => Ok(ProfilerStatus::Failure(path)),
        value => Err(ProfilerError::InvalidStatus(value)),
    }
}

fn temporary_directory() -> PathBuf {
    std::env::var_os("TMPDIR")
        .map(PathBuf::from)
        .unwrap_or_else(|| PathBuf::from("/tmp"))
}

fn remove_socket(path: &Path) {
    match fs::remove_file(path) {
        Ok(()) => {}
        Err(error) if error.kind() == io::ErrorKind::NotFound => {}
        Err(_) => {}
    }
}

struct SocketGuard(PathBuf);

impl Drop for SocketGuard {
    fn drop(&mut self) {
        remove_socket(&self.0);
    }
}

enum ProfilerStatus {
    Dump(PathBuf),
    Health,
    Failure(PathBuf),
}

#[derive(Debug)]
enum ProfilerError {
    CreateDirectory(io::Error),
    Extract(io::Error),
    BindStatus(io::Error),
    StatusPermissions(io::Error),
    ConfigureStatus(io::Error),
    AcceptStatus(io::Error),
    ReadStatus(io::Error),
    Diagnostics(String),
    ConnectDiagnostics(io::Error),
    SendAttach(io::Error),
    ReceiveAttach(io::Error),
    InvalidProfilerPath(PathBuf),
    InvalidAttachResponse(u16),
    AttachRuntime(i32),
    PacketTooLarge,
    InvalidExceptionFilter,
    InvalidStatusLength(usize),
    InvalidStatus(u8),
    ProfilerFailure(PathBuf),
    NotManaged,
    TargetExited,
}

impl std::fmt::Display for ProfilerError {
    fn fmt(&self, formatter: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            Self::CreateDirectory(error) => {
                write!(formatter, "failed to create profiler directory: {error}")
            }
            Self::Extract(error) => write!(formatter, "failed to extract profiler: {error}"),
            Self::BindStatus(error) => {
                write!(formatter, "failed to bind profiler status socket: {error}")
            }
            Self::StatusPermissions(error) => write!(
                formatter,
                "failed to set profiler socket permissions: {error}"
            ),
            Self::ConfigureStatus(error) => write!(
                formatter,
                "failed to configure profiler status socket: {error}"
            ),
            Self::AcceptStatus(error) => {
                write!(formatter, "failed to accept profiler status: {error}")
            }
            Self::ReadStatus(error) => write!(formatter, "failed to read profiler status: {error}"),
            Self::Diagnostics(error) => formatter.write_str(error),
            Self::ConnectDiagnostics(error) => write!(
                formatter,
                "failed to connect to .NET diagnostics socket: {error}"
            ),
            Self::SendAttach(error) => {
                write!(formatter, "failed to send profiler attach request: {error}")
            }
            Self::ReceiveAttach(error) => write!(
                formatter,
                "failed to receive profiler attach response: {error}"
            ),
            Self::InvalidProfilerPath(path) => {
                write!(formatter, "invalid profiler path: {}", path.display())
            }
            Self::InvalidAttachResponse(size) => {
                write!(formatter, "invalid profiler attach response size: {size}")
            }
            Self::AttachRuntime(result) => write!(
                formatter,
                ".NET profiler attach failed with HRESULT 0x{result:08x}"
            ),
            Self::PacketTooLarge => write!(formatter, "profiler attach packet is too large"),
            Self::InvalidExceptionFilter => write!(formatter, "invalid exception filter"),
            Self::InvalidStatusLength(length) => {
                write!(formatter, "invalid profiler status length: {length}")
            }
            Self::InvalidStatus(status) => {
                write!(formatter, "invalid profiler status byte: {status}")
            }
            Self::ProfilerFailure(path) => write!(
                formatter,
                "profiler failed to generate dump: {}",
                path.display()
            ),
            Self::NotManaged => write!(formatter, "target process has no .NET diagnostics socket"),
            Self::TargetExited => write!(
                formatter,
                "target process exited during profiler monitoring"
            ),
        }
    }
}

impl std::error::Error for ProfilerError {}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn exception_client_data_matches_profiler_grammar() {
        assert_eq!(
            build_client_data(
                &DotNetTrigger::Exception,
                "/tmp/dumps/",
                42,
                2,
                Some(std::ffi::OsStr::new("invalid,*Exact")),
            )
            .unwrap(),
            "6;/tmp/dumps/;42;*invalid*:2;*Exact*:2;"
        );
    }

    #[test]
    fn gc_client_data_matches_profiler_grammar() {
        assert_eq!(
            build_client_data(
                &DotNetTrigger::GcMemory {
                    heap: GcHeap::LargeObject,
                    thresholds_mb: vec![10, 20, 30],
                },
                "/tmp/dumps/",
                42,
                3,
                None,
            )
            .unwrap(),
            "7;/tmp/dumps/;42;3;10;20;30"
        );
    }
}
