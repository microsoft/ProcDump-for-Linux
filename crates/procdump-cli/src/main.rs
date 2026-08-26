use procdump::config::{self, Platform};
use procdump::dump::PlatformDumpBackend;
use procdump::orchestrator::monitor_processes;
use procdump::sync::MonitorControl;
use signal_hook::consts::{SIGINT, SIGTERM};
use signal_hook::iterator::Signals;
use std::path::Path;
use std::process::ExitCode;
use std::sync::Arc;
use std::thread;

#[cfg(target_os = "linux")]
use procdump::process::linux::LinuxProcfs as NativeProcesses;
#[cfg(target_os = "macos")]
use procdump::process::macos::MacOsProcesses as NativeProcesses;

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
    let dump_uses_native_backend = config.requires_gcore_preflight();
    if dump_uses_native_backend && !command_on_path("gcore") {
        let message = "failed to locate gcore binary in $PATH. Check that gdb/gcore is installed and configured on your system.";
        println!("{message}");
        return Err(message.into());
    }
    let processes = Arc::new(NativeProcesses::new()?);
    let shutdown = Arc::new(MonitorControl::new());
    let signal_control = Arc::clone(&shutdown);
    let mut signals = Signals::new([SIGINT, SIGTERM])?;
    let handle = signals.handle();
    let signal_thread = thread::Builder::new()
        .name("shutdown signal monitor".into())
        .spawn(move || {
            if signals.forever().next().is_some() {
                signal_control.request_quit();
            }
        })?;

    println!("ProcDump for Rust");
    let result = monitor_processes(
        &config,
        platform,
        processes,
        Arc::new(PlatformDumpBackend),
        shutdown,
    );
    handle.close();
    let _ = signal_thread.join();
    result?;
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
