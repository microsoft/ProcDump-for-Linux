use procdump::config::{self, Platform};
use procdump::dump::PlatformDumpBackend;
use procdump::orchestrator::monitor_processes;
use procdump::sync::MonitorControl;
use signal_hook::consts::{SIGINT, SIGTERM};
use signal_hook::iterator::Signals;
use std::ffi::OsString;
use std::fmt;
use std::path::Path;
use std::process::ExitCode;
use std::sync::Arc;
use std::thread;

#[cfg(target_os = "linux")]
use procdump::process::linux::LinuxProcfs as NativeProcesses;
#[cfg(target_os = "macos")]
use procdump::process::macos::MacOsProcesses as NativeProcesses;

fn main() -> ExitCode {
    let platform = match native_platform() {
        Ok(platform) => platform,
        Err(error) => {
            eprintln!("Error: {error}");
            return ExitCode::from(255);
        }
    };
    print_banner();
    let arguments: Vec<_> = std::env::args_os().skip(1).collect();
    if arguments.is_empty() {
        print_usage(platform);
        return ExitCode::from(255);
    }
    match run(arguments, platform) {
        Ok(()) => ExitCode::SUCCESS,
        Err(CliError::HelpRequested) => {
            print_usage(platform);
            ExitCode::from(255)
        }
        Err(CliError::Parse(error)) => {
            if let Some(message) = error.legacy_cli_message() {
                procdump::diagnostics::error(message);
            }
            print_usage(platform);
            ExitCode::from(255)
        }
        Err(CliError::Runtime(error)) => {
            procdump::diagnostics::error(error);
            ExitCode::from(255)
        }
    }
}

fn run(arguments: Vec<OsString>, platform: Platform) -> Result<(), CliError> {
    let config = match config::parse(arguments, platform) {
        Ok(config) => config,
        Err(config::ParseError::HelpRequested) => return Err(CliError::HelpRequested),
        Err(error) => return Err(CliError::Parse(error)),
    };
    if config.requires_gcore_preflight() && !command_on_path("gcore") {
        let message = "failed to locate gcore binary in $PATH. Check that gdb/gcore is installed and configured on your system.";
        return Err(CliError::Runtime(message.into()));
    }
    let processes =
        Arc::new(NativeProcesses::new().map_err(|error| CliError::Runtime(error.to_string()))?);
    let shutdown = Arc::new(MonitorControl::new());
    let signal_control = Arc::clone(&shutdown);
    let mut signals =
        Signals::new([SIGINT, SIGTERM]).map_err(|error| CliError::Runtime(error.to_string()))?;
    let handle = signals.handle();
    let signal_thread = thread::Builder::new()
        .name("shutdown signal monitor".into())
        .spawn(move || {
            if signals.forever().next().is_some() {
                signal_control.request_quit();
            }
        })
        .map_err(|error| CliError::Runtime(error.to_string()))?;

    let result = monitor_processes(
        &config,
        platform,
        processes,
        Arc::new(PlatformDumpBackend),
        shutdown,
    );
    handle.close();
    let _ = signal_thread.join();
    result.map_err(|error| CliError::Runtime(error.to_string()))?;
    Ok(())
}

#[derive(Debug)]
enum CliError {
    HelpRequested,
    Parse(config::ParseError),
    Runtime(String),
}

impl fmt::Display for CliError {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::HelpRequested => formatter.write_str("help requested"),
            Self::Parse(error) => error.fmt(formatter),
            Self::Runtime(error) => formatter.write_str(error),
        }
    }
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

fn print_banner() {
    print!("{}", banner());
}

fn banner() -> String {
    format!(
        concat!(
            "\nProcDump v{} - Sysinternals process dump utility\n",
            "Copyright (C) 2025 Microsoft Corporation. All rights reserved. Licensed under the MIT license.\n",
            "Mark Russinovich, Mario Hewardt, John Salem, Javid Habibi\n",
            "Sysinternals - www.sysinternals.com\n\n",
            "Monitors one or more processes and writes a core dump file when the processes exceeds the\n",
            "specified criteria.\n\n"
        ),
        product_version()
    )
}

fn product_version() -> &'static str {
    option_env!("PROCDUMP_VERSION").unwrap_or(env!("CARGO_PKG_VERSION"))
}

fn print_usage(platform: Platform) {
    print!("{}", usage(platform));
}

fn usage(platform: Platform) -> String {
    let mut output = String::from(concat!(
        "\nCapture Usage: \n",
        "   procdump [-n Count]\n",
        "            [-s Seconds]\n",
        "            [-c|-cl CPU_Usage]\n",
        "            [-m|-ml Commit_Usage1[,Commit_Usage2...]]\n",
        "            [-tc Thread_Threshold]\n",
        "            [-fc FileDescriptor_Threshold]\n"
    ));
    if platform == Platform::Linux {
        output.push_str(concat!(
            "            [-gcm [<GCGeneration>: | LOH: | POH:]Memory_Usage1[,Memory_Usage2...]]\n",
            "            [-gcgen Generation]\n",
            "            [-restrack [nodump]]\n",
            "            [-sr Sample_Rate]\n",
            "            [-sig Signal_Number1[,Signal_Number2...]]\n",
            "            [-pc|-pcl Provider:Counter[pN] Threshold]\n",
            "            [-e]\n",
            "            [-f Include_Filter,...]\n",
            "            [-fx Exclude_Filter]\n",
            "            [-mc Custom_Dump_Mask]\n"
        ));
    }
    output.push_str(concat!(
        "            [-pf Polling_Frequency]\n",
        "            [-o]\n",
        "            [-log syslog|stdout]\n",
        "            {\n"
    ));
    if platform == Platform::Linux {
        output.push_str(
            "             {{[-w] Process_Name | [-pgid] PID} [Dump_File | Dump_Folder]}\n",
        );
    } else {
        output.push_str("             {{[-w] Process_Name | PID} [Dump_File | Dump_Folder]}\n");
    }
    output.push_str(concat!(
        "            }\n\n",
        "Options:\n",
        "   -n      Number of dumps to write before exiting.\n",
        "   -s      Consecutive seconds before dump is written (default is 10).\n",
        "   -c      CPU threshold above which to create a dump of the process.\n",
        "   -cl     CPU threshold below which to create a dump of the process.\n",
        "   -tc     Thread count threshold above which to create a dump of the process.\n",
        "   -fc     File descriptor count threshold above which to create a dump of the process.\n"
    ));
    if platform == Platform::Linux {
        output.push_str(concat!(
            "   -m      Memory commit threshold(s) (MB) above which to create dumps.\n",
            "   -ml     Memory commit threshold(s) (MB) below which to create dumps.\n",
            "   -gcm    [.NET] GC memory threshold(s) (MB) above which to create dumps for the specified generation or heap (default is total .NET memory usage).\n",
            "   -gcgen  [.NET] Create dump when the garbage collection of the specified generation starts and finishes.\n",
            "   -restrack Enable memory leak tracking (malloc family of APIs). If used without other triggers, use 't' to manually capture a restrack report. When used with other triggers, the 'nodump' option can be used to prevent dump generation and only produce restrack report(s).\n",
            "   -sr     Sample rate when using -restrack.\n",
            "   -sig    Comma separated list of signal number(s) during which any signal results in a dump of the process.\n",
            "   -pc     [.NET] Trigger when performance counter is at or exceeds the threshold. Format: provider_name:counter_name[pN] threshold.\n",
            "           Supports both EventCounters and System.Diagnostics.Metrics. For histogram instruments, append [pN] to select\n",
            "           a percentile (e.g., [p50], [p95], [p99]). Default is p50 if omitted.\n",
            "   -pcl    [.NET] Trigger when performance counter falls below the threshold. Format: provider_name:counter_name[pN] threshold.\n",
            "   -e      [.NET] Create dump when the process encounters an exception.\n",
            "   -f      Filter (include) on the content of .NET exceptions (comma separated). Wildcards (*) are supported.\n",
            "   -fx     Filter (exclude) on the content of -restrack call stacks. Wildcards (*) are supported.\n",
            "   -mc     Custom core dump mask (in hex) indicating what memory should be included in the core dump. Please see 'man core' (/proc/[pid]/coredump_filter) for available options.\n",
            "   -pgid   Process ID specified refers to a process group ID.\n"
        ));
    }
    output.push_str(concat!(
        "   -pf     Polling frequency.\n",
        "   -o      Overwrite existing dump file.\n",
        "   -log    Writes extended ProcDump tracing to the specified output stream (syslog or stdout).\n",
        "   -w      Wait for the specified process to launch if it's not running.\n"
    ));
    output
}

#[cfg(test)]
mod tests {
    use super::*;

    fn expected_help(platform: Platform) -> String {
        let fixture = match platform {
            Platform::Linux => {
                include_str!("../../../tests/cli-compat/legacy-linux-help.txt")
            }
            Platform::MacOs => {
                include_str!("../../../tests/cli-compat/legacy-macos-help.txt")
            }
        };
        fixture
            .replace("@VERSION@", product_version())
            .replace("@OPTION_INDENT@", "   ")
            .replace("@EOL@", "\n")
    }

    fn has_option(usage: &str, option: &str) -> bool {
        let prefix = format!("{option} ");
        usage
            .lines()
            .any(|line| line.trim_start().starts_with(&prefix))
    }

    #[test]
    fn banner_identifies_version_and_sysinternals() {
        let banner = banner();

        assert!(banner.contains(&format!("ProcDump v{}", product_version())));
        assert!(banner.contains("Sysinternals process dump utility"));
        assert!(banner.contains("Mark Russinovich, Mario Hewardt, John Salem, Javid Habibi"));
    }

    #[test]
    fn platform_help_matches_legacy_output_byte_for_byte() {
        for platform in [Platform::Linux, Platform::MacOs] {
            assert_eq!(
                format!("{}{}", banner(), usage(platform)),
                expected_help(platform)
            );
        }
    }

    #[test]
    fn linux_usage_lists_every_supported_option() {
        let usage = usage(Platform::Linux);

        assert!(usage.starts_with("\nCapture Usage: \n   procdump [-n Count]\n"));
        assert!(usage.contains("\n            [-s Seconds]\n"));
        assert!(usage.contains("\nOptions:\n   -n      Number of dumps"));

        for option in [
            "-n",
            "-s",
            "-c",
            "-cl",
            "-m",
            "-ml",
            "-tc",
            "-fc",
            "-gcm",
            "-gcgen",
            "-restrack",
            "-sr",
            "-sig",
            "-pc",
            "-pcl",
            "-e",
            "-f",
            "-fx",
            "-mc",
            "-pgid",
            "-pf",
            "-o",
            "-log",
            "-w",
        ] {
            assert!(has_option(&usage, option), "usage is missing {option}");
        }
    }

    #[test]
    fn macos_usage_omits_linux_only_options() {
        let usage = usage(Platform::MacOs);

        for option in [
            "-gcm",
            "-gcgen",
            "-restrack",
            "-sr",
            "-sig",
            "-pc",
            "-pcl",
            "-e",
            "-f",
            "-fx",
            "-mc",
            "-pgid",
        ] {
            assert!(!has_option(&usage, option), "macOS usage contains {option}");
        }
        assert!(usage.contains("{{[-w] Process_Name | PID}"));
    }
}
