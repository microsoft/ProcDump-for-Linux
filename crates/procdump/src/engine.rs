use std::ffi::{CStr, OsStr, OsString};
use std::fmt;
use std::fs::{self, File, OpenOptions};
use std::io::{self, Read};
use std::os::unix::fs::{MetadataExt, OpenOptionsExt};
use std::path::{Path, PathBuf};
use std::process::{Command, Stdio};
use std::sync::Arc;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Mutex, MutexGuard, OnceLock};
use std::thread;
use std::time::{Duration, Instant};

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
#[allow(dead_code)]
pub enum Platform {
    Linux,
    MacOs,
}

impl Platform {
    pub fn native() -> Result<Self, crate::WriteDumpError> {
        #[cfg(target_os = "linux")]
        return Ok(Self::Linux);
        #[cfg(target_os = "macos")]
        return Ok(Self::MacOs);
        #[cfg(not(any(target_os = "linux", target_os = "macos")))]
        return Err(crate::WriteDumpError::UnsupportedPlatform);
    }
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
#[allow(dead_code)]
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
pub struct OutputSpec {
    pub directory: PathBuf,
    pub file_name: Option<OsString>,
}

#[derive(Clone, Debug, Default)]
pub struct CancellationToken(Arc<AtomicBool>);

impl CancellationToken {
    #[allow(dead_code)]
    pub fn cancel(&self) {
        self.0.store(true, Ordering::Release);
    }

    pub fn is_cancelled(&self) -> bool {
        self.0.load(Ordering::Acquire)
    }
}

#[derive(Clone, Debug)]
pub struct DumpRequest {
    pub pid: i32,
    pub process_name: OsString,
    pub kind: DumpKind,
    pub output: OutputSpec,
    pub overwrite: bool,
    #[allow(dead_code)]
    pub use_gcore: bool,
    pub platform: Platform,
    pub cancellation: Option<CancellationToken>,
    pub core_dump_mask: Option<u32>,
}

pub trait DumpBackend: Send + Sync {
    fn write_dump(&self, request: &DumpRequest) -> Result<PathBuf, DumpError>;
}

#[derive(Clone, Debug, Default)]
pub struct GcoreBackend;

#[derive(Clone, Debug, Default)]
#[cfg(feature = "monitor")]
pub struct PlatformDumpBackend;

#[cfg(feature = "monitor")]
impl DumpBackend for PlatformDumpBackend {
    fn write_dump(&self, request: &DumpRequest) -> Result<PathBuf, DumpError> {
        write_dump(request)
    }
}

pub fn write_dump(request: &DumpRequest) -> Result<PathBuf, DumpError> {
    let _target_guard = target_lock(request.pid);
    let mut mask = crate::mask::CoreDumpMaskGuard::apply(request.pid, request.core_dump_mask)
        .map_err(|error| DumpError::Mask(error.to_string()))?;
    let result = write_dump_inner(request);
    let restore = mask
        .restore()
        .map_err(|error| DumpError::Mask(error.to_string()));
    match (result, restore) {
        (Ok(path), Ok(())) => Ok(path),
        (Err(error), Ok(())) | (Ok(_), Err(error)) => Err(error),
        (Err(error), Err(restore_error)) => Err(DumpError::Mask(format!(
            "{error}; additionally, mask restoration failed: {restore_error}"
        ))),
    }
}

fn write_dump_inner(request: &DumpRequest) -> Result<PathBuf, DumpError> {
    #[cfg(target_os = "linux")]
    if request.platform == Platform::Linux {
        let socket = crate::dotnet::find_diagnostics_socket(request.pid)
            .map_err(|error| DumpError::DotNet(error.to_string()))?;
        if let Some(socket) = socket {
            let paths = available_dump_paths(request, true)?;
            if paths.prefix.exists() && !request.overwrite {
                return Err(DumpError::AlreadyExists(paths.prefix));
            }
            ensure_writable_directory(&request.output.directory)?;
            reserve_output_file(&paths.prefix, request.overwrite)?;
            if let Err(error) = crate::dotnet::generate_dump(&socket, &paths.prefix) {
                remove_if_present(&paths.prefix);
                return Err(DumpError::DotNet(error.to_string()));
            }
            if !paths.prefix.is_file() {
                return Err(DumpError::DotNet(format!(
                    ".NET runtime reported success but did not create {}",
                    paths.prefix.display()
                )));
            }
            return Ok(paths.prefix);
        }
        #[cfg(any(target_arch = "x86_64", target_arch = "aarch64"))]
        if !request.use_gcore {
            return write_corex_dump(request);
        }
    }
    GcoreBackend.write_dump(request)
}

fn target_lock(pid: i32) -> MutexGuard<'static, ()> {
    const STRIPES: usize = 64;
    static LOCKS: OnceLock<Vec<Mutex<()>>> = OnceLock::new();
    let locks = LOCKS.get_or_init(|| (0..STRIPES).map(|_| Mutex::new(())).collect());
    let index = pid.unsigned_abs() as usize % STRIPES;
    locks[index]
        .lock()
        .unwrap_or_else(|poisoned| poisoned.into_inner())
}

#[cfg(all(
    target_os = "linux",
    any(target_arch = "x86_64", target_arch = "aarch64")
))]
fn write_corex_dump(request: &DumpRequest) -> Result<PathBuf, DumpError> {
    let paths = available_dump_paths(request, false)?;
    if paths.final_path.exists() && !request.overwrite {
        return Err(DumpError::AlreadyExists(paths.final_path));
    }
    ensure_writable_directory(&request.output.directory)?;
    let output = open_output_file(&paths.final_path, request.overwrite)?;
    crate::corex::dump_pid(
        request.pid,
        &paths.final_path,
        output,
        request.cancellation.as_ref(),
    )
    .map_err(|error| DumpError::Corex(error.to_string()))?;
    if !paths.final_path.is_file() {
        return Err(DumpError::Corex(format!(
            "corex reported success but did not create {}",
            paths.final_path.display()
        )));
    }
    Ok(paths.final_path.clone())
}

impl DumpBackend for GcoreBackend {
    fn write_dump(&self, request: &DumpRequest) -> Result<PathBuf, DumpError> {
        let paths = available_dump_paths(request, false)?;
        if paths.final_path.exists() && !request.overwrite {
            return Err(DumpError::AlreadyExists(paths.final_path));
        }
        ensure_writable_directory(&request.output.directory)?;

        run_gcore(request, &paths, OsStr::new("gcore"))
    }
}

fn run_gcore(
    request: &DumpRequest,
    paths: &DumpPaths,
    command: &OsStr,
) -> Result<PathBuf, DumpError> {
    let output_argument = match request.platform {
        Platform::Linux => &paths.prefix,
        Platform::MacOs => &paths.final_path,
    };
    reserve_output_file(&paths.final_path, request.overwrite)?;
    let mut child = Command::new(command)
        .arg("-o")
        .arg(output_argument)
        .arg(request.pid.to_string())
        .stdout(Stdio::piped())
        .stderr(Stdio::piped())
        .spawn()
        .map_err(|source| DumpError::Start {
            program: "gcore",
            source,
        })?;
    let mut stdout = child.stdout.take().ok_or_else(|| DumpError::Io {
        operation: "capture gcore stdout",
        path: paths.final_path.clone(),
        source: io::Error::other("gcore stdout pipe is unavailable"),
    })?;
    let mut stderr = child.stderr.take().ok_or_else(|| DumpError::Io {
        operation: "capture gcore stderr",
        path: paths.final_path.clone(),
        source: io::Error::other("gcore stderr pipe is unavailable"),
    })?;
    let stdout_reader = thread::spawn(move || {
        let mut output = Vec::new();
        stdout.read_to_end(&mut output).map(|_| output)
    });
    let stderr_reader = thread::spawn(move || {
        let mut output = Vec::new();
        stderr.read_to_end(&mut output).map(|_| output)
    });
    let status = loop {
        if request
            .cancellation
            .as_ref()
            .is_some_and(CancellationToken::is_cancelled)
        {
            let _ = child.kill();
            let _ = child.wait();
            let _ = stdout_reader.join();
            let _ = stderr_reader.join();
            remove_if_present(&paths.final_path);
            return Err(DumpError::Cancelled);
        }
        match child.try_wait().map_err(|source| DumpError::Io {
            operation: "wait for gcore",
            path: paths.final_path.clone(),
            source,
        })? {
            Some(status) => break status,
            None => thread::sleep(Duration::from_millis(25)),
        }
    };
    let stdout = stdout_reader
        .join()
        .map_err(|_| DumpError::OutputThreadPanicked)?
        .map_err(|source| DumpError::Io {
            operation: "read gcore stdout",
            path: paths.final_path.clone(),
            source,
        })?;
    let stderr = stderr_reader
        .join()
        .map_err(|_| DumpError::OutputThreadPanicked)?
        .map_err(|source| DumpError::Io {
            operation: "read gcore stderr",
            path: paths.final_path.clone(),
            source,
        })?;
    if !status.success() || !paths.final_path.is_file() {
        remove_if_present(&paths.final_path);
        return Err(DumpError::Backend {
            status: status.code(),
            output: combined_output(&stdout, &stderr),
        });
    }
    Ok(paths.final_path.clone())
}

#[derive(Clone, Debug, Eq, PartialEq)]
struct DumpPaths {
    prefix: PathBuf,
    final_path: PathBuf,
}

fn available_dump_paths(request: &DumpRequest, use_prefix: bool) -> Result<DumpPaths, DumpError> {
    let deadline = Instant::now() + Duration::from_secs(2);
    loop {
        let paths = dump_paths(request, &local_timestamp()?)?;
        let occupied = if use_prefix {
            paths.prefix.exists()
        } else {
            paths.final_path.exists()
        };
        if request.output.file_name.is_some() || !occupied {
            return Ok(paths);
        }
        if Instant::now() >= deadline {
            return Err(DumpError::AlreadyExists(if use_prefix {
                paths.prefix
            } else {
                paths.final_path
            }));
        }
        thread::sleep(Duration::from_millis(25));
    }
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
        request.pid
    ));
    if !is_legacy_safe_path(&prefix) || !is_legacy_safe_path(&final_path) {
        return Err(DumpError::InvalidPath(final_path));
    }
    Ok(DumpPaths { prefix, final_path })
}

#[cfg(all(target_os = "linux", feature = "restrack"))]
pub fn sidecar_path(request: &DumpRequest, extension: &str) -> Result<PathBuf, DumpError> {
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

pub(crate) fn ensure_writable_directory(path: &Path) -> Result<(), DumpError> {
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
    let mode = metadata.mode();
    let writable_by_others = mode & 0o022 != 0;
    let sticky = mode & sticky_bit() != 0;
    if writable_by_others && !sticky {
        return Err(DumpError::UnsafeDirectory(path.to_path_buf()));
    }
    Ok(())
}

#[cfg(target_os = "linux")]
const fn sticky_bit() -> u32 {
    libc::S_ISVTX
}

#[cfg(target_os = "macos")]
const fn sticky_bit() -> u32 {
    libc::S_ISVTX as u32
}

pub(crate) fn open_output_file(path: &Path, overwrite: bool) -> Result<File, DumpError> {
    let directory = path.parent().unwrap_or_else(|| Path::new("."));
    ensure_writable_directory(directory)?;
    let mut options = OpenOptions::new();
    options
        .write(true)
        .mode(0o600)
        .custom_flags(libc::O_NOFOLLOW | libc::O_CLOEXEC);
    if overwrite {
        options.create(true);
    } else {
        options.create_new(true);
    }
    let file = options.open(path).map_err(|source| {
        if source.kind() == io::ErrorKind::AlreadyExists {
            DumpError::AlreadyExists(path.to_path_buf())
        } else {
            DumpError::Io {
                operation: "securely create dump file",
                path: path.to_path_buf(),
                source,
            }
        }
    })?;
    let metadata = file.metadata().map_err(|source| DumpError::Io {
        operation: "inspect dump file",
        path: path.to_path_buf(),
        source,
    })?;
    if !metadata.file_type().is_file()
        || metadata.uid() != unsafe { libc::geteuid() }
        || metadata.nlink() != 1
    {
        return Err(DumpError::UnsafeOutput(path.to_path_buf()));
    }
    if overwrite {
        file.set_len(0).map_err(|source| DumpError::Io {
            operation: "truncate validated dump file",
            path: path.to_path_buf(),
            source,
        })?;
    }
    Ok(file)
}

fn reserve_output_file(path: &Path, overwrite: bool) -> Result<(), DumpError> {
    drop(open_output_file(path, overwrite)?);
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
    UnsafeDirectory(PathBuf),
    UnsafeOutput(PathBuf),
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
    Mask(String),
    #[cfg(target_os = "linux")]
    Corex(String),
    Io {
        operation: &'static str,
        path: PathBuf,
        source: io::Error,
    },
    Clock,
    Cancelled,
    OutputThreadPanicked,
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
            Self::UnsafeDirectory(path) => write!(
                formatter,
                "Core dump directory is writable by other users without sticky protection: {}",
                path.display()
            ),
            Self::UnsafeOutput(path) => write!(
                formatter,
                "Core dump target is not a trusted regular file: {}",
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
            Self::Mask(error) => formatter.write_str(error),
            #[cfg(target_os = "linux")]
            Self::Corex(error) => write!(formatter, "corex failed to generate core dump: {error}"),
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
            Self::Cancelled => write!(formatter, "dump generation was cancelled"),
            Self::OutputThreadPanicked => write!(formatter, "gcore output reader panicked"),
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
    use std::os::unix::fs::{PermissionsExt, symlink};
    use std::sync::mpsc::sync_channel;

    fn request(output: OutputSpec) -> DumpRequest {
        DumpRequest {
            pid: 42,
            process_name: OsString::from("worker pool[1]"),
            kind: DumpKind::Cpu,
            output,
            overwrite: false,
            use_gcore: false,
            platform: Platform::Linux,
            cancellation: None,
            core_dump_mask: None,
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

    #[test]
    fn secure_output_refuses_symlink_targets() {
        let root =
            std::env::temp_dir().join(format!("procdump-output-security-{}", std::process::id()));
        let _ = fs::remove_dir_all(&root);
        fs::create_dir(&root).unwrap();
        let victim = root.join("victim");
        let link = root.join("dump");
        fs::write(&victim, b"unchanged").unwrap();
        symlink(&victim, &link).unwrap();

        assert!(open_output_file(&link, true).is_err());
        assert_eq!(fs::read(&victim).unwrap(), b"unchanged");
        fs::remove_dir_all(root).unwrap();
    }

    #[test]
    fn rejects_non_sticky_world_writable_directory() {
        let root =
            std::env::temp_dir().join(format!("procdump-unsafe-directory-{}", std::process::id()));
        let _ = fs::remove_dir_all(&root);
        fs::create_dir(&root).unwrap();
        fs::set_permissions(&root, fs::Permissions::from_mode(0o777)).unwrap();

        let error = ensure_writable_directory(&root).unwrap_err();
        assert!(matches!(error, DumpError::UnsafeDirectory(path) if path == root));
        fs::remove_dir_all(root).unwrap();
    }

    #[test]
    fn secure_output_preserves_existing_file_without_overwrite() {
        let root =
            std::env::temp_dir().join(format!("procdump-no-overwrite-{}", std::process::id()));
        let _ = fs::remove_dir_all(&root);
        fs::create_dir(&root).unwrap();
        fs::set_permissions(&root, fs::Permissions::from_mode(0o700)).unwrap();
        let path = root.join("dump");
        fs::write(&path, b"existing").unwrap();

        let error = open_output_file(&path, false).unwrap_err();

        assert!(matches!(error, DumpError::AlreadyExists(value) if value == path));
        assert_eq!(fs::read(&path).unwrap(), b"existing");
        fs::remove_dir_all(root).unwrap();
    }

    #[test]
    fn target_lock_serializes_same_pid() {
        let guard = target_lock(42);
        let (started_tx, started_rx) = sync_channel(1);
        let (acquired_tx, acquired_rx) = sync_channel(1);
        let waiter = std::thread::spawn(move || {
            started_tx.send(()).unwrap();
            let _guard = target_lock(42);
            acquired_tx.send(()).unwrap();
        });
        started_rx.recv().unwrap();
        assert!(acquired_rx.recv_timeout(Duration::from_millis(50)).is_err());

        drop(guard);

        acquired_rx.recv_timeout(Duration::from_secs(1)).unwrap();
        waiter.join().unwrap();
    }

    #[test]
    fn gcore_child_is_cancelled_and_reaped() {
        let root =
            std::env::temp_dir().join(format!("procdump-gcore-cancel-{}", std::process::id()));
        let _ = fs::remove_dir_all(&root);
        fs::create_dir(&root).unwrap();
        fs::set_permissions(&root, fs::Permissions::from_mode(0o700)).unwrap();
        let command = root.join("gcore-test");
        fs::write(&command, "#!/bin/sh\nexec sleep 30\n").unwrap();
        fs::set_permissions(&command, fs::Permissions::from_mode(0o755)).unwrap();
        let cancellation = CancellationToken::default();
        let mut request = request(OutputSpec {
            directory: root.clone(),
            file_name: Some(OsString::from("cancel.core")),
        });
        request.use_gcore = true;
        request.cancellation = Some(cancellation.clone());
        let paths = dump_paths(&request, "ignored").unwrap();
        let started = Instant::now();
        let worker = std::thread::spawn(move || run_gcore(&request, &paths, command.as_os_str()));
        std::thread::sleep(Duration::from_millis(100));
        cancellation.cancel();

        let result = worker.join().unwrap();
        assert!(matches!(result, Err(DumpError::Cancelled)), "{result:?}");
        assert!(started.elapsed() < Duration::from_secs(2));
        fs::remove_dir_all(root).unwrap();
    }
}
