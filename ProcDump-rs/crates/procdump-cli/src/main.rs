use procdump_core::config::{self, Platform};
use procdump_core::dump::PlatformDumpBackend;
use procdump_core::orchestrator::monitor_processes;
use std::path::Path;
use std::process::ExitCode;
use std::sync::Arc;

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

    println!("ProcDump for Rust");
    monitor_processes(&config, platform, processes, Arc::new(PlatformDumpBackend))?;
    Ok(())
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
