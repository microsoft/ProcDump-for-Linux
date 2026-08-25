use std::env;
use std::ffi::OsStr;
use std::fmt;
use std::fs;
use std::io;
use std::path::{Path, PathBuf};
use std::process::{Command, ExitStatus};

fn main() {
    if let Err(error) = run() {
        eprintln!("xtask: {error}");
        std::process::exit(1);
    }
}

fn run() -> Result<(), XtaskError> {
    let mut arguments = env::args_os().skip(1);
    let command = arguments.next().unwrap_or_else(|| "help".into());
    match command.to_str() {
        Some("stage-tests") => {
            reject_extra(arguments)?;
            stage_tests()
        }
        Some("test-scenario") => {
            let filter = arguments
                .next()
                .ok_or_else(|| XtaskError::Usage(usage().into()))?;
            reject_extra(arguments)?;
            stage_tests()?;
            run_scenario(&filter)
        }
        Some("test-integration") => {
            let filter = arguments.next();
            reject_extra(arguments)?;
            stage_tests()?;
            run_integration(filter.as_deref())
        }
        Some("help") | Some("-h") | Some("--help") => {
            println!("{}", usage());
            Ok(())
        }
        _ => Err(XtaskError::Usage(usage().into())),
    }
}

fn usage() -> &'static str {
    "Usage: cargo xtask <command>\n\
     Commands:\n\
       stage-tests                 Build and stage integration artifacts\n\
       test-scenario <name>        Run one staged scenario without forcing elevation\n\
       test-integration [filter]   Run the existing root-required integration runner"
}

fn stage_tests() -> Result<(), XtaskError> {
    let paths = Paths::discover()?;
    run_checked(
        Command::new("cargo").current_dir(&paths.workspace).args([
            "build",
            "--release",
            "--bin",
            "procdump",
        ]),
        "build release procdump",
    )?;
    run_checked(
        Command::new("cargo").current_dir(&paths.workspace).args([
            "build",
            "--release",
            "-p",
            "procdump-capi",
        ]),
        "build release libprocdump.a",
    )?;

    remove_old_stage(&paths.stage)?;
    fs::create_dir_all(&paths.stage).map_err(|source| XtaskError::Io {
        operation: "create integration stage",
        path: paths.stage.clone(),
        source,
    })?;

    copy_file(
        &paths.workspace.join("target/release/procdump"),
        &paths.stage.join("procdump"),
    )?;
    copy_tree(
        &paths.legacy.join("tests/integration"),
        &paths.stage.join("tests/integration"),
    )?;
    add_staged_fail_fast_check(&paths.stage.join("tests/integration/helpers.sh"))?;
    copy_file(
        &paths.legacy.join("nuget.config"),
        &paths.workspace.join("target/nuget.config"),
    )?;
    build_test_application(&paths)?;

    let static_library = paths.workspace.join("target/release/libprocdump.a");
    build_library_driver(&paths, &static_library)?;
    write_elevated_runner(&paths)?;
    println!("integration stage: {}", paths.stage.display());
    Ok(())
}

fn add_staged_fail_fast_check(helper: &Path) -> Result<(), XtaskError> {
    let contents = fs::read_to_string(helper).map_err(|source| XtaskError::Io {
        operation: "read staged integration helper",
        path: helper.to_path_buf(),
        source,
    })?;
    let marker = "  while [ ! -S $socketpath ]\n  do\n";
    let replacement = "  while [ ! -S $socketpath ]\n  do\n      if ! ps -p \"$procdumpchildpid\" >/dev/null 2>&1; then\n        echo \"ProcDump exited before creating the .NET status socket\"\n        result=-1\n        return\n      fi\n";
    let updated = contents.replacen(marker, replacement, 1);
    if updated == contents {
        return Err(XtaskError::StagingPatch(
            "could not locate profiler socket wait loop".into(),
        ));
    }
    fs::write(helper, updated).map_err(|source| XtaskError::Io {
        operation: "write staged integration helper",
        path: helper.to_path_buf(),
        source,
    })
}

fn remove_old_stage(stage: &Path) -> Result<(), XtaskError> {
    if !stage.exists() {
        return Ok(());
    }
    match fs::remove_dir_all(stage) {
        Ok(()) => Ok(()),
        Err(_) if effective_uid()? != 0 => {
            let owner = format!("{}:{}", effective_uid()?, effective_gid()?);
            run_checked(
                Command::new("sudo")
                    .args([OsStr::new("chown"), OsStr::new("-R")])
                    .arg(owner)
                    .arg(stage),
                "restore integration stage ownership",
            )?;
            fs::remove_dir_all(stage).map_err(|source| XtaskError::Io {
                operation: "remove old integration stage",
                path: stage.to_path_buf(),
                source,
            })
        }
        Err(source) => Err(XtaskError::Io {
            operation: "remove old integration stage",
            path: stage.to_path_buf(),
            source,
        }),
    }
}

fn write_elevated_runner(paths: &Paths) -> Result<(), XtaskError> {
    let owner = format!("{}:{}", effective_uid()?, effective_gid()?);
    let runner = paths.stage.join("tests/integration/run.sh");
    let wrapper = paths.stage.join("run-elevated.sh");
    let contents = format!(
        "#!/bin/bash\n\
         cleanup_processes() {{\n\
           pkill -9 TestWebApi >/dev/null 2>&1 || true\n\
           pkill -9 procdump >/dev/null 2>&1 || true\n\
           pkill -9 gcore >/dev/null 2>&1 || true\n\
                     pkill -9 -f '^cat /dev/urandom$' >/dev/null 2>&1 || true\n\
                     rm -f /tmp/procdump/procdump-status-* >/dev/null 2>&1 || true\n\
         }}\n\
         cleanup() {{\n\
           cleanup_processes\n\
           chown -R {owner} {} >/dev/null 2>&1 || true\n\
         }}\n\
         trap cleanup EXIT INT TERM\n\
         cleanup_processes\n\
         {} \"$@\"\n\
         exit $?\n",
        shell_quote(&paths.stage),
        shell_quote(&runner),
    );
    fs::write(&wrapper, contents).map_err(|source| XtaskError::Io {
        operation: "write elevated integration runner",
        path: wrapper.clone(),
        source,
    })?;
    #[cfg(unix)]
    {
        use std::os::unix::fs::PermissionsExt;
        fs::set_permissions(&wrapper, fs::Permissions::from_mode(0o755)).map_err(|source| {
            XtaskError::Io {
                operation: "make elevated integration runner executable",
                path: wrapper,
                source,
            }
        })?;
    }
    Ok(())
}

fn shell_quote(path: &Path) -> String {
    format!("'{}'", path.to_string_lossy().replace('\'', "'\\''"))
}

fn build_test_application(paths: &Paths) -> Result<(), XtaskError> {
    let compiler = env::var_os("CC").unwrap_or_else(|| "cc".into());
    let mut command = Command::new(compiler);
    command
        .arg("-g")
        .arg("-pthread")
        .arg("-std=gnu99")
        .arg("-D_GNU_SOURCE")
        .arg(
            paths
                .legacy
                .join("tests/integration/ProcDumpTestApplication.c"),
        )
        .arg("-o")
        .arg(paths.stage.join("ProcDumpTestApplication"));
    run_checked(&mut command, "build ProcDumpTestApplication")
}

fn build_library_driver(paths: &Paths, static_library: &Path) -> Result<(), XtaskError> {
    let compiler = env::var_os("CXX").unwrap_or_else(|| "c++".into());
    let mut command = Command::new(compiler);
    command
        .arg("-g")
        .arg("-pthread")
        .arg("-std=c++11")
        .arg("-I")
        .arg(paths.legacy.join("lib"))
        .arg(
            paths
                .legacy
                .join("tests/integration/ProcDumpLibTestDriver.cpp"),
        )
        .arg(static_library)
        .arg("-ldl")
        .arg("-o")
        .arg(paths.stage.join("ProcDumpLibTestDriver"));
    run_checked(&mut command, "build ProcDumpLibTestDriver")
}

fn run_scenario(filter: &OsStr) -> Result<(), XtaskError> {
    let paths = Paths::discover()?;
    let directory = if cfg!(target_os = "macos") {
        "scenarios_mac"
    } else {
        "scenarios"
    };
    let mut name = PathBuf::from(filter);
    if name.extension().is_none() {
        name.set_extension("sh");
    }
    let scenario = paths
        .stage
        .join("tests/integration")
        .join(directory)
        .join(name);
    if !scenario.is_file() {
        return Err(XtaskError::MissingScenario(scenario));
    }
    let mut command = Command::new(&scenario);
    command.current_dir(scenario.parent().unwrap_or(&paths.stage));
    run_checked(&mut command, "run integration scenario")
}

fn run_integration(filter: Option<&OsStr>) -> Result<(), XtaskError> {
    let paths = Paths::discover()?;
    let runner = paths.stage.join("tests/integration/run.sh");
    let elevated_runner = paths.stage.join("run-elevated.sh");
    let mut command = integration_command(&runner, &elevated_runner, effective_uid()?);
    if let Some(filter) = filter {
        command.arg(filter);
    }
    command.current_dir(runner.parent().unwrap_or(&paths.stage));
    run_checked(&mut command, "run integration suite")
}

fn integration_command(runner: &Path, elevated_runner: &Path, effective_uid: u32) -> Command {
    if effective_uid == 0 {
        Command::new(runner)
    } else {
        let mut command = Command::new("sudo");
        command.arg(elevated_runner);
        command
    }
}

fn effective_uid() -> Result<u32, XtaskError> {
    effective_id("-u")
}

fn effective_gid() -> Result<u32, XtaskError> {
    effective_id("-g")
}

fn effective_id(option: &'static str) -> Result<u32, XtaskError> {
    let output =
        Command::new("id")
            .arg(option)
            .output()
            .map_err(|source| XtaskError::CommandStart {
                action: "read effective uid",
                source,
            })?;
    if !output.status.success() {
        return Err(XtaskError::CommandFailed {
            action: "read effective uid",
            status: output.status,
        });
    }
    String::from_utf8_lossy(&output.stdout)
        .trim()
        .parse()
        .map_err(|_| XtaskError::InvalidUid)
}

fn run_checked(command: &mut Command, action: &'static str) -> Result<(), XtaskError> {
    let status = command
        .status()
        .map_err(|source| XtaskError::CommandStart { action, source })?;
    if status.success() {
        Ok(())
    } else {
        Err(XtaskError::CommandFailed { action, status })
    }
}

fn copy_tree(source: &Path, destination: &Path) -> Result<(), XtaskError> {
    fs::create_dir_all(destination).map_err(|source_error| XtaskError::Io {
        operation: "create copied directory",
        path: destination.to_path_buf(),
        source: source_error,
    })?;
    let entries = fs::read_dir(source).map_err(|source_error| XtaskError::Io {
        operation: "read copied directory",
        path: source.to_path_buf(),
        source: source_error,
    })?;
    for entry in entries {
        let entry = entry.map_err(|source_error| XtaskError::Io {
            operation: "read copied entry",
            path: source.to_path_buf(),
            source: source_error,
        })?;
        let source_path = entry.path();
        let destination_path = destination.join(entry.file_name());
        if entry
            .file_type()
            .map_err(|source_error| XtaskError::Io {
                operation: "inspect copied entry",
                path: source_path.clone(),
                source: source_error,
            })?
            .is_dir()
        {
            copy_tree(&source_path, &destination_path)?;
        } else {
            copy_file(&source_path, &destination_path)?;
        }
    }
    Ok(())
}

fn copy_file(source: &Path, destination: &Path) -> Result<(), XtaskError> {
    fs::copy(source, destination).map_err(|source_error| XtaskError::Io {
        operation: "copy file",
        path: source.to_path_buf(),
        source: source_error,
    })?;
    let permissions = fs::metadata(source)
        .map(|metadata| metadata.permissions())
        .map_err(|source_error| XtaskError::Io {
            operation: "read copied permissions",
            path: source.to_path_buf(),
            source: source_error,
        })?;
    fs::set_permissions(destination, permissions).map_err(|source_error| XtaskError::Io {
        operation: "set copied permissions",
        path: destination.to_path_buf(),
        source: source_error,
    })?;
    Ok(())
}

fn reject_extra(mut arguments: impl Iterator<Item = std::ffi::OsString>) -> Result<(), XtaskError> {
    if arguments.next().is_some() {
        Err(XtaskError::Usage(usage().into()))
    } else {
        Ok(())
    }
}

struct Paths {
    workspace: PathBuf,
    legacy: PathBuf,
    stage: PathBuf,
}

impl Paths {
    fn discover() -> Result<Self, XtaskError> {
        let workspace = PathBuf::from(env!("CARGO_MANIFEST_DIR"))
            .parent()
            .map(Path::to_path_buf)
            .ok_or(XtaskError::WorkspaceRoot)?;
        let legacy = workspace
            .parent()
            .map(Path::to_path_buf)
            .ok_or(XtaskError::WorkspaceRoot)?;
        let stage = workspace.join("target/integration");
        Ok(Self {
            workspace,
            legacy,
            stage,
        })
    }
}

#[derive(Debug)]
enum XtaskError {
    Usage(String),
    StagingPatch(String),
    WorkspaceRoot,
    MissingScenario(PathBuf),
    InvalidUid,
    Io {
        operation: &'static str,
        path: PathBuf,
        source: io::Error,
    },
    CommandStart {
        action: &'static str,
        source: io::Error,
    },
    CommandFailed {
        action: &'static str,
        status: ExitStatus,
    },
}

impl fmt::Display for XtaskError {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::Usage(usage) => formatter.write_str(usage),
            Self::StagingPatch(message) => formatter.write_str(message),
            Self::WorkspaceRoot => write!(formatter, "failed to locate workspace root"),
            Self::MissingScenario(path) => {
                write!(formatter, "scenario does not exist: {}", path.display())
            }
            Self::InvalidUid => write!(formatter, "could not parse effective uid"),
            Self::Io {
                operation,
                path,
                source,
            } => write!(
                formatter,
                "failed to {operation} {}: {source}",
                path.display()
            ),
            Self::CommandStart { action, source } => {
                write!(formatter, "failed to {action}: {source}")
            }
            Self::CommandFailed { action, status } => {
                write!(formatter, "failed to {action}: {status}")
            }
        }
    }
}

impl std::error::Error for XtaskError {}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn non_root_integration_elevates_only_the_staged_runner() {
        let runner = Path::new("/tmp/stage/tests/integration/run.sh");
        let elevated = Path::new("/tmp/stage/run-elevated.sh");
        let command = integration_command(runner, elevated, 1000);

        assert_eq!(command.get_program(), "sudo");
        assert_eq!(
            command.get_args().collect::<Vec<_>>(),
            vec![elevated.as_os_str()]
        );
    }

    #[test]
    fn root_integration_runs_the_staged_runner_directly() {
        let runner = Path::new("/tmp/stage/tests/integration/run.sh");
        let elevated = Path::new("/tmp/stage/run-elevated.sh");
        let command = integration_command(runner, elevated, 0);

        assert_eq!(command.get_program(), runner.as_os_str());
        assert!(command.get_args().next().is_none());
    }

    #[test]
    fn shell_quote_handles_spaces_and_apostrophes() {
        assert_eq!(shell_quote(Path::new("/tmp/a b/c'd")), "'/tmp/a b/c'\\''d'");
    }

    #[test]
    fn staged_helper_stops_if_procdump_exits() {
        let directory =
            std::env::temp_dir().join(format!("procdump-rs-xtask-helper-{}", std::process::id()));
        fs::create_dir_all(&directory).unwrap();
        let helper = directory.join("helpers.sh");
        fs::write(
            &helper,
            "  while [ ! -S $socketpath ]\n  do\n      sleep 1s\n  done\n",
        )
        .unwrap();

        add_staged_fail_fast_check(&helper).unwrap();
        let updated = fs::read_to_string(&helper).unwrap();
        assert!(updated.contains("ProcDump exited before creating"));
        fs::remove_dir_all(directory).unwrap();
    }
}
