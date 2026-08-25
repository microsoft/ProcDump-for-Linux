use procdump_core::config::{self, Config, Platform, TargetSpec};
use procdump_core::dump::PlatformDumpBackend;
use procdump_core::monitor::MonitorSet;
use procdump_core::process::{
    ProcessDiscovery, ProcessError, ProcessId, ProcessMetrics, ProcessSnapshot,
};
use std::ffi::OsStr;
use std::path::Path;
use std::process::ExitCode;
use std::sync::Arc;
use std::thread;
use std::time::Duration;

#[cfg(target_os = "linux")]
use procdump_core::process::linux::LinuxProcfs as NativeProcesses;
#[cfg(target_os = "macos")]
use procdump_core::process::macos::MacOsProcesses as NativeProcesses;

fn main() -> ExitCode {
    match run() {
        Ok(()) => ExitCode::SUCCESS,
        Err(error) => {
            eprintln!("Error: {error}");
            ExitCode::from(255)
        }
    }
}

fn run() -> Result<(), Box<dyn std::error::Error>> {
    let platform = native_platform()?;
    let config = config::parse(std::env::args_os().skip(1), platform).map_err(|error| {
        if matches!(error, config::ParseError::HelpRequested) {
            usage().to_owned()
        } else {
            format!("{error}\n{}", usage())
        }
    })?;
    let dump_uses_native_backend = config.dotnet_trigger.is_none()
        && config.perf_counters.is_empty()
        && !config
            .restrack
            .as_ref()
            .is_some_and(|restrack| !restrack.generate_dump);
    if dump_uses_native_backend && !command_on_path("gcore") {
        let message = "failed to locate gcore binary in $PATH. Check that gdb/gcore is installed and configured on your system.";
        println!("{message}");
        return Err(message.into());
    }
    let processes = Arc::new(NativeProcesses::new()?);
    let initial = resolve_target(&config, processes.as_ref())?;

    println!("ProcDump for Rust");
    println!(
        "Starting monitor for process {} ({})",
        initial.name.to_string_lossy(),
        initial.identity.pid.get()
    );

    let monitor = MonitorSet::start(
        &config,
        platform,
        initial,
        processes,
        Arc::new(PlatformDumpBackend),
    )?;
    monitor.wait()?;
    Ok(())
}

fn resolve_target<P>(config: &Config, processes: &P) -> Result<ProcessSnapshot, ProcessError>
where
    P: ProcessDiscovery + ProcessMetrics,
{
    match &config.target {
        TargetSpec::Pid(pid) => processes.sample(ProcessId::new(*pid)?),
        TargetSpec::Name(name) => loop {
            let mut newest = None;
            for pid in processes.list_processes()? {
                let candidate = match processes.name(pid) {
                    Ok(candidate) => candidate,
                    Err(ProcessError::Disappeared(_)) => continue,
                    Err(error) => return Err(error),
                };
                let matches = if config.wait_for_process {
                    candidate == *name
                } else {
                    os_eq_ignore_ascii_case(&candidate, name)
                };
                if matches {
                    let snapshot = processes.sample(pid)?;
                    if newest.as_ref().is_none_or(|current: &ProcessSnapshot| {
                        snapshot.identity.start_time > current.identity.start_time
                    }) {
                        newest = Some(snapshot);
                    }
                }
            }
            if let Some(snapshot) = newest {
                return Ok(snapshot);
            }
            if !config.wait_for_process {
                return Err(ProcessError::NameNotFound(name.clone()));
            }
            thread::sleep(Duration::from_millis(config.polling_interval_ms));
        },
        TargetSpec::ProcessGroup(group) => {
            let mut newest = None;
            for pid in processes.list_processes()? {
                let process_group = match processes.process_group(pid) {
                    Ok(process_group) => process_group,
                    Err(ProcessError::Disappeared(_)) => continue,
                    Err(ProcessError::Io { source, .. })
                        if source.kind() == std::io::ErrorKind::PermissionDenied =>
                    {
                        continue;
                    }
                    Err(error) => return Err(error),
                };
                if process_group == *group {
                    let snapshot = match processes.sample(pid) {
                        Ok(snapshot) => snapshot,
                        Err(ProcessError::Disappeared(_)) => continue,
                        Err(error) => return Err(error),
                    };
                    if newest.as_ref().is_none_or(|current: &ProcessSnapshot| {
                        snapshot.identity.start_time > current.identity.start_time
                    }) {
                        newest = Some(snapshot);
                    }
                }
            }
            newest.ok_or(ProcessError::GroupNotFound(*group))
        }
    }
}

fn os_eq_ignore_ascii_case(left: &OsStr, right: &OsStr) -> bool {
    left.to_string_lossy()
        .eq_ignore_ascii_case(&right.to_string_lossy())
}

fn command_on_path(command: &str) -> bool {
    std::env::var_os("PATH").is_some_and(|path| {
        std::env::split_paths(&path).any(|directory| {
            let candidate = directory.join(command);
            candidate.is_file() && is_executable(&candidate)
        })
    })
}

#[cfg(unix)]
fn is_executable(path: &Path) -> bool {
    use std::os::unix::fs::PermissionsExt;
    std::fs::metadata(path).is_ok_and(|metadata| metadata.permissions().mode() & 0o111 != 0)
}

#[cfg(target_os = "linux")]
fn native_platform() -> Result<Platform, &'static str> {
    Ok(Platform::Linux)
}

#[cfg(target_os = "macos")]
fn native_platform() -> Result<Platform, &'static str> {
    Ok(Platform::MacOs)
}

#[cfg(not(any(target_os = "linux", target_os = "macos")))]
fn native_platform() -> Result<Platform, &'static str> {
    Err("this platform is not supported")
}

fn usage() -> &'static str {
    "Usage: procdump [-n Count] [-s Seconds] [-c|-cl CPU] [-m|-ml Memory] \
[-tc Threads] [-fc FileDescriptors] [-pf PollingMs] [-o] [-w] \
{Process_Name|PID} [DumpFile|DumpFolder]"
}
