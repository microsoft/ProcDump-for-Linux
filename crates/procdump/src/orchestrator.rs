use crate::config::{Config, Platform, TargetSpec};
use crate::dump::DumpBackend;
use crate::monitor::{MonitorError, MonitorSet};
use crate::process::{
    ProcessDiscovery, ProcessError, ProcessId, ProcessIdentity, ProcessMetrics, ProcessSnapshot,
};
use std::collections::HashMap;
use std::ffi::{OsStr, OsString};
use std::fmt;
use std::io;
use std::sync::Arc;
use std::time::Duration;

pub fn monitor_processes<P>(
    config: &Config,
    platform: Platform,
    processes: Arc<P>,
    backend: Arc<dyn DumpBackend>,
    shutdown: Arc<crate::sync::MonitorControl>,
) -> Result<(), OrchestratorError>
where
    P: ProcessDiscovery + ProcessMetrics + 'static,
{
    let polling = Duration::from_millis(config.polling_interval_ms.max(50));
    let mode = MonitorMode::from_config(config)?;
    let mut active = HashMap::new();
    let mut monitored = HashMap::new();

    match &mode {
        MonitorMode::WaitForName(name) => crate::diagnostics::info(
            config.diagnostics,
            crate::cli_output::waiting_for_processes(&name.to_string_lossy()),
        ),
        MonitorMode::ProcessGroup(group) => crate::diagnostics::info(
            config.diagnostics,
            crate::cli_output::monitoring_process_group(*group),
        ),
        MonitorMode::Pid(_) | MonitorMode::SingleName(_) => {}
    }
    crate::diagnostics::info(config.diagnostics, crate::cli_output::PRESS_CTRL_C);

    let initial = match &mode {
        MonitorMode::Pid(pid) => vec![processes.sample(*pid)?],
        MonitorMode::SingleName(name) => vec![
            newest_named_process(processes.as_ref(), name, false)?
                .ok_or_else(|| ProcessError::NameNotFound(name.clone()))?,
        ],
        MonitorMode::WaitForName(name) => discover_named_processes(processes.as_ref(), name, true)?,
        MonitorMode::ProcessGroup(group) => {
            let matches = discover_group_processes(processes.as_ref(), *group)?;
            if matches.is_empty() {
                return Err(ProcessError::GroupNotFound(*group).into());
            }
            matches
        }
    };
    let summary_process = initial
        .first()
        .map(|process| (process.name.as_os_str(), process.identity.pid.get()));
    print!("{}", config.legacy_summary(platform, summary_process));
    start_new_monitors(
        config,
        platform,
        &processes,
        &backend,
        initial,
        &mut active,
        &mut monitored,
    )?;

    loop {
        if shutdown.is_quit_requested() {
            break;
        }
        reap_finished(processes.as_ref(), &mut active)?;
        if matches!(mode, MonitorMode::Pid(_) | MonitorMode::SingleName(_)) && active.is_empty() {
            break;
        }

        let discovered = match &mode {
            MonitorMode::WaitForName(name) => {
                discover_named_processes(processes.as_ref(), name, true)?
            }
            MonitorMode::ProcessGroup(group) => {
                discover_group_processes(processes.as_ref(), *group)?
            }
            MonitorMode::Pid(_) | MonitorMode::SingleName(_) => Vec::new(),
        };
        start_new_monitors(
            config,
            platform,
            &processes,
            &backend,
            discovered,
            &mut active,
            &mut monitored,
        )?;
        if matches!(mode, MonitorMode::ProcessGroup(_)) && active.is_empty() {
            break;
        }
        if shutdown.wait(polling) == crate::sync::WaitOutcome::Quit {
            break;
        }
    }

    for (_, session) in active.drain() {
        finish_session(processes.as_ref(), session)?;
    }
    Ok(())
}

#[derive(Debug)]
enum MonitorMode {
    Pid(ProcessId),
    SingleName(OsString),
    WaitForName(OsString),
    ProcessGroup(i32),
}

impl MonitorMode {
    fn from_config(config: &Config) -> Result<Self, ProcessError> {
        match &config.target {
            TargetSpec::Pid(pid) => Ok(Self::Pid(ProcessId::new(*pid)?)),
            TargetSpec::Name(name) if config.wait_for_process => {
                Ok(Self::WaitForName(name.clone()))
            }
            TargetSpec::Name(name) => Ok(Self::SingleName(name.clone())),
            TargetSpec::ProcessGroup(group) => Ok(Self::ProcessGroup(*group)),
        }
    }
}

struct ActiveMonitor {
    identity: ProcessIdentity,
    name: OsString,
    multiple: bool,
    monitor: MonitorSet,
}

fn start_new_monitors<P>(
    config: &Config,
    platform: Platform,
    processes: &Arc<P>,
    backend: &Arc<dyn DumpBackend>,
    discovered: Vec<ProcessSnapshot>,
    active: &mut HashMap<ProcessId, ActiveMonitor>,
    monitored: &mut HashMap<ProcessId, ProcessIdentity>,
) -> Result<(), OrchestratorError>
where
    P: ProcessDiscovery + ProcessMetrics + 'static,
{
    for snapshot in discovered {
        let identity = snapshot.identity;
        let name = snapshot.name.clone();
        if active
            .get(&identity.pid)
            .is_some_and(|session| session.identity == identity)
            || monitored.get(&identity.pid) == Some(&identity)
        {
            continue;
        }
        if let Some(previous) = active.remove(&identity.pid) {
            finish_session(processes.as_ref(), previous)?;
        }
        let metrics: Arc<dyn ProcessMetrics> = processes.clone();
        let monitor = MonitorSet::start(config, platform, snapshot, metrics, Arc::clone(backend))?;
        crate::diagnostics::info(
            config.diagnostics,
            crate::cli_output::starting_monitor(&name.to_string_lossy(), identity.pid.get()),
        );
        monitored.insert(identity.pid, identity);
        active.insert(
            identity.pid,
            ActiveMonitor {
                identity,
                name,
                multiple: matches!(
                    config.target,
                    TargetSpec::Name(_) if config.wait_for_process
                ) || matches!(config.target, TargetSpec::ProcessGroup(_)),
                monitor,
            },
        );
    }
    Ok(())
}

fn reap_finished<P>(
    processes: &P,
    active: &mut HashMap<ProcessId, ActiveMonitor>,
) -> Result<(), OrchestratorError>
where
    P: ProcessDiscovery,
{
    let mut completed = Vec::new();
    for (pid, session) in active.iter() {
        let alive = match processes.is_alive(session.identity) {
            Ok(alive) => alive,
            Err(ProcessError::Disappeared(_)) => false,
            Err(error) => return Err(error.into()),
        };
        if session.monitor.has_finished() || !alive {
            completed.push(*pid);
        }
    }
    for pid in completed {
        if let Some(session) = active.remove(&pid) {
            finish_session(processes, session)?;
        }
    }
    Ok(())
}

fn finish_session<P>(processes: &P, session: ActiveMonitor) -> Result<(), OrchestratorError>
where
    P: ProcessDiscovery,
{
    let alive = match processes.is_alive(session.identity) {
        Ok(alive) => alive,
        Err(ProcessError::Disappeared(_)) => false,
        Err(error) => return Err(error.into()),
    };
    if session.multiple {
        crate::diagnostics::info(
            crate::config::DiagnosticsTarget::None,
            crate::cli_output::stopping_monitors(
                &session.name.to_string_lossy(),
                session.identity.pid.get(),
            ),
        );
    }
    session.monitor.request_quit();
    let result = match session.monitor.wait() {
        Ok(()) => Ok(()),
        Err(MonitorError::Process(ProcessError::Disappeared(_)) | MonitorError::PidReused(_)) => {
            Ok(())
        }
        Err(_) if !alive => Ok(()),
        Err(error) => Err(error.into()),
    };
    if !session.multiple {
        crate::diagnostics::info(
            crate::config::DiagnosticsTarget::None,
            crate::cli_output::stopping_monitor(
                &session.name.to_string_lossy(),
                session.identity.pid.get(),
            ),
        );
    }
    result
}

fn newest_named_process<P>(
    processes: &P,
    name: &OsStr,
    case_sensitive: bool,
) -> Result<Option<ProcessSnapshot>, ProcessError>
where
    P: ProcessDiscovery + ProcessMetrics,
{
    Ok(discover_named_processes(processes, name, case_sensitive)?
        .into_iter()
        .max_by_key(|snapshot| snapshot.identity.start_time))
}

fn discover_named_processes<P>(
    processes: &P,
    name: &OsStr,
    case_sensitive: bool,
) -> Result<Vec<ProcessSnapshot>, ProcessError>
where
    P: ProcessDiscovery + ProcessMetrics,
{
    let mut matches = Vec::new();
    for pid in processes.list_processes()? {
        let candidate = match processes.name(pid) {
            Ok(candidate) => candidate,
            Err(error) if is_transient_discovery_error(&error) => continue,
            Err(error) => return Err(error),
        };
        let matched = if case_sensitive {
            candidate == name
        } else {
            candidate
                .to_string_lossy()
                .eq_ignore_ascii_case(&name.to_string_lossy())
        };
        if matched {
            match processes.sample(pid) {
                Ok(snapshot) => matches.push(snapshot),
                Err(error) if is_transient_discovery_error(&error) => {}
                Err(error) => return Err(error),
            }
        }
    }
    Ok(matches)
}

fn discover_group_processes<P>(
    processes: &P,
    group: i32,
) -> Result<Vec<ProcessSnapshot>, ProcessError>
where
    P: ProcessDiscovery + ProcessMetrics,
{
    let mut matches = Vec::new();
    for pid in processes.list_processes()? {
        match processes.process_group(pid) {
            Ok(candidate) if candidate == group => match processes.sample(pid) {
                Ok(snapshot) => matches.push(snapshot),
                Err(error) if is_transient_discovery_error(&error) => {}
                Err(error) => return Err(error),
            },
            Ok(_) => {}
            Err(error) if is_transient_discovery_error(&error) => {}
            Err(error) => return Err(error),
        }
    }
    Ok(matches)
}

fn is_transient_discovery_error(error: &ProcessError) -> bool {
    match error {
        ProcessError::Disappeared(_) => true,
        ProcessError::Io { source, .. } => matches!(
            source.kind(),
            io::ErrorKind::NotFound | io::ErrorKind::PermissionDenied
        ),
        _ => false,
    }
}

#[derive(Debug)]
pub enum OrchestratorError {
    Process(ProcessError),
    Monitor(MonitorError),
}

impl fmt::Display for OrchestratorError {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::Process(error) => error.fmt(formatter),
            Self::Monitor(error) => error.fmt(formatter),
        }
    }
}

impl std::error::Error for OrchestratorError {
    fn source(&self) -> Option<&(dyn std::error::Error + 'static)> {
        match self {
            Self::Process(error) => Some(error),
            Self::Monitor(error) => Some(error),
        }
    }
}

impl From<ProcessError> for OrchestratorError {
    fn from(value: ProcessError) -> Self {
        Self::Process(value)
    }
}

impl From<MonitorError> for OrchestratorError {
    fn from(value: MonitorError) -> Self {
        Self::Monitor(value)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::config::{DiagnosticsTarget, OutputSpec};
    use crate::dump::{DumpError, DumpRequest};
    use crate::process::ProcessState;
    use std::path::PathBuf;
    use std::sync::Mutex;

    struct FakeProcesses {
        snapshots: Mutex<Vec<ProcessSnapshot>>,
    }

    impl ProcessDiscovery for FakeProcesses {
        fn list_processes(&self) -> Result<Vec<ProcessId>, ProcessError> {
            Ok(self
                .snapshots
                .lock()
                .unwrap()
                .iter()
                .map(|snapshot| snapshot.identity.pid)
                .collect())
        }

        fn identity(&self, pid: ProcessId) -> Result<ProcessIdentity, ProcessError> {
            Ok(self.find(pid)?.identity)
        }

        fn name(&self, pid: ProcessId) -> Result<OsString, ProcessError> {
            Ok(self.find(pid)?.name)
        }

        fn process_group(&self, pid: ProcessId) -> Result<i32, ProcessError> {
            Ok(self.find(pid)?.process_group)
        }

        fn is_alive(&self, identity: ProcessIdentity) -> Result<bool, ProcessError> {
            Ok(self.find(identity.pid)?.identity == identity)
        }
    }

    impl ProcessMetrics for FakeProcesses {
        fn sample(&self, pid: ProcessId) -> Result<ProcessSnapshot, ProcessError> {
            self.find(pid)
        }

        fn cpu_usage_percent(&self, _snapshot: &ProcessSnapshot) -> Result<u32, ProcessError> {
            Ok(0)
        }
    }

    impl FakeProcesses {
        fn find(&self, pid: ProcessId) -> Result<ProcessSnapshot, ProcessError> {
            self.snapshots
                .lock()
                .unwrap()
                .iter()
                .find(|snapshot| snapshot.identity.pid == pid)
                .cloned()
                .ok_or(ProcessError::Disappeared(pid))
        }
    }

    fn snapshot(pid: i32, start_time: u64, name: &str, group: i32) -> ProcessSnapshot {
        ProcessSnapshot {
            identity: ProcessIdentity {
                pid: ProcessId::new(pid).unwrap(),
                start_time,
            },
            name: name.into(),
            state: ProcessState::Sleeping,
            parent_pid: 1,
            process_group: group,
            cpu_time_ticks: 0,
            rss_bytes: 0,
            swap_bytes: 0,
            thread_count: 1,
            file_descriptor_count: 1,
        }
    }

    #[test]
    fn group_discovery_returns_every_member() {
        let processes = FakeProcesses {
            snapshots: Mutex::new(vec![
                snapshot(10, 1, "one", 7),
                snapshot(11, 2, "two", 7),
                snapshot(12, 3, "other", 8),
            ]),
        };
        let matches = discover_group_processes(&processes, 7).unwrap();
        assert_eq!(matches.len(), 2);
    }

    #[test]
    fn wait_by_name_is_case_sensitive_and_returns_all_incarnations() {
        let processes = FakeProcesses {
            snapshots: Mutex::new(vec![
                snapshot(10, 1, "worker", 7),
                snapshot(11, 2, "worker", 7),
                snapshot(12, 3, "Worker", 7),
            ]),
        };
        let matches = discover_named_processes(&processes, OsStr::new("worker"), true).unwrap();
        assert_eq!(matches.len(), 2);
    }

    #[test]
    fn direct_name_selects_newest_case_insensitively() {
        let processes = FakeProcesses {
            snapshots: Mutex::new(vec![
                snapshot(10, 1, "Worker", 7),
                snapshot(11, 9, "worker", 7),
            ]),
        };
        let newest = newest_named_process(&processes, OsStr::new("WORKER"), false)
            .unwrap()
            .unwrap();
        assert_eq!(newest.identity.pid.get(), 11);
    }

    #[derive(Default)]
    struct RecordingBackend(Mutex<Vec<i32>>);

    impl DumpBackend for RecordingBackend {
        fn write_dump(&self, request: &DumpRequest) -> Result<PathBuf, DumpError> {
            self.0.lock().unwrap().push(request.pid.get());
            Ok(PathBuf::from(format!("dump.{}", request.pid.get())))
        }
    }

    #[test]
    fn process_group_runs_a_monitor_set_for_each_member() {
        let processes = Arc::new(FakeProcesses {
            snapshots: Mutex::new(vec![
                snapshot(10, 1, "one", 7),
                snapshot(11, 2, "two", 7),
                snapshot(12, 3, "other", 8),
            ]),
        });
        let backend = Arc::new(RecordingBackend::default());
        let config = Config {
            target: TargetSpec::ProcessGroup(7),
            output: OutputSpec::default(),
            cpu: None,
            memory_mb: None,
            thread_count: None,
            file_descriptor_count: None,
            polling_interval_ms: 1,
            threshold_seconds: 0,
            dump_count: 1,
            wait_for_process: false,
            overwrite: false,
            diagnostics: DiagnosticsTarget::None,
            use_gcore: true,
            timer_trigger: true,
            signals: Vec::new(),
            dotnet_trigger: None,
            exception_filter: None,
            perf_counters: Vec::new(),
            restrack: None,
            core_dump_mask: None,
        };

        monitor_processes(
            &config,
            Platform::Linux,
            processes,
            backend.clone(),
            Arc::new(crate::sync::MonitorControl::new()),
        )
        .unwrap();
        let mut dumped = backend.0.lock().unwrap().clone();
        dumped.sort_unstable();
        assert_eq!(dumped, vec![10, 11]);
    }
}
