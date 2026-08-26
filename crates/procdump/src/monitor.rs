use crate::config::{Config, DiagnosticsTarget, Platform, Threshold};
use crate::dump::{DumpBackend, DumpError, DumpKind, DumpRequest};
use crate::process::{ProcessError, ProcessIdentity, ProcessMetrics, ProcessSnapshot};
use crate::sync::{DumpGate, MonitorControl, WaitOutcome};
use std::fmt;
use std::path::PathBuf;
use std::sync::Arc;
use std::sync::atomic::{AtomicU32, Ordering};
use std::thread::{self, JoinHandle};
use std::time::Duration;

pub struct DumpCoordinator {
    backend: Arc<dyn DumpBackend>,
    sidecar: Option<Arc<dyn DumpSidecar>>,
    gate: DumpGate,
    control: Arc<MonitorControl>,
    request: DumpRequest,
    collected: AtomicU32,
    limit: u32,
    pub(crate) diagnostics: DiagnosticsTarget,
}

impl DumpCoordinator {
    pub(crate) fn new(
        backend: Arc<dyn DumpBackend>,
        sidecar: Option<Arc<dyn DumpSidecar>>,
        control: Arc<MonitorControl>,
        request: DumpRequest,
        limit: u32,
        diagnostics: DiagnosticsTarget,
    ) -> Self {
        Self {
            backend,
            sidecar,
            gate: DumpGate::new(),
            control,
            request,
            collected: AtomicU32::new(0),
            limit,
            diagnostics,
        }
    }

    pub fn collected(&self) -> u32 {
        self.collected.load(Ordering::Acquire)
    }

    pub fn limit_reached(&self) -> bool {
        self.collected() >= self.limit
    }

    pub fn write(&self, kind: DumpKind) -> Result<Option<PathBuf>, MonitorError> {
        if self.limit_reached() || self.control.is_quit_requested() {
            return Ok(None);
        }
        let Some(_permit) = self.gate.acquire(&self.control) else {
            return Ok(None);
        };
        if self.limit_reached() || self.control.is_quit_requested() {
            return Ok(None);
        }

        let mut request = self.request.clone();
        request.kind = kind;
        request.cancellation = Some(self.control.cancellation_token());
        let dump_path = if self
            .sidecar
            .as_ref()
            .is_none_or(|sidecar| sidecar.generate_primary_dump())
        {
            Some(self.backend.write_dump(&request)?)
        } else {
            None
        };
        let sidecar_path = self
            .sidecar
            .as_ref()
            .map(|sidecar| sidecar.write(request.kind, dump_path.as_deref()))
            .transpose()?;
        let dump_number = self.collected.fetch_add(1, Ordering::AcqRel);
        if let Some(path) = &dump_path {
            crate::diagnostics::info(
                self.diagnostics,
                format!("Core dump {dump_number} generated: {}", path.display()),
            );
        }
        if dump_number + 1 >= self.limit {
            self.control.request_quit();
        }
        Ok(dump_path.or(sidecar_path))
    }

    #[cfg(all(target_os = "linux", feature = "dotnet-triggers"))]
    pub(crate) fn record_external_dump(&self, path: &std::path::Path) -> bool {
        let Some(_permit) = self.gate.acquire(&self.control) else {
            return false;
        };
        if self.limit_reached() {
            return false;
        }
        let dump_number = self.collected.fetch_add(1, Ordering::AcqRel);
        crate::diagnostics::info(
            self.diagnostics,
            format!("Core dump {dump_number} generated: {}", path.display()),
        );
        if dump_number + 1 >= self.limit {
            self.control.request_quit();
        }
        true
    }
}

pub(crate) trait DumpSidecar: Send + Sync {
    fn generate_primary_dump(&self) -> bool;
    fn write(
        &self,
        kind: DumpKind,
        primary_path: Option<&std::path::Path>,
    ) -> Result<PathBuf, MonitorError>;
}

pub struct MonitorSet {
    control: Arc<MonitorControl>,
    threads: Vec<JoinHandle<Result<(), MonitorError>>>,
}

struct StartupGuard {
    control: Arc<MonitorControl>,
    threads: Vec<JoinHandle<Result<(), MonitorError>>>,
    armed: bool,
}

impl StartupGuard {
    fn new(control: Arc<MonitorControl>) -> Self {
        Self {
            control,
            threads: Vec::new(),
            armed: true,
        }
    }

    fn push(&mut self, thread: JoinHandle<Result<(), MonitorError>>) {
        self.threads.push(thread);
    }

    #[cfg(all(target_os = "linux", feature = "restrack"))]
    fn extend(&mut self, threads: impl IntoIterator<Item = JoinHandle<Result<(), MonitorError>>>) {
        self.threads.extend(threads);
    }

    fn finish(mut self) -> Vec<JoinHandle<Result<(), MonitorError>>> {
        self.control.start();
        self.armed = false;
        std::mem::take(&mut self.threads)
    }
}

impl Drop for StartupGuard {
    fn drop(&mut self) {
        if self.armed {
            self.control.request_quit();
            for thread in self.threads.drain(..) {
                let _ = thread.join();
            }
        }
    }
}

impl MonitorSet {
    pub fn start(
        config: &Config,
        platform: Platform,
        initial: ProcessSnapshot,
        metrics: Arc<dyn ProcessMetrics>,
        backend: Arc<dyn DumpBackend>,
    ) -> Result<Self, MonitorError> {
        let control = Arc::new(MonitorControl::new());
        let mut startup = StartupGuard::new(Arc::clone(&control));
        #[cfg(all(target_os = "linux", feature = "restrack"))]
        let sidecar: Option<Arc<dyn DumpSidecar>> = if config.restrack.is_some() {
            let runtime = crate::restrack::spawn_restrack_monitors(
                config,
                Arc::clone(&control),
                initial.clone(),
                platform,
            )?;
            startup.extend(runtime.threads);
            Some(runtime.reporter)
        } else {
            None
        };
        #[cfg(not(all(target_os = "linux", feature = "restrack")))]
        let sidecar: Option<Arc<dyn DumpSidecar>> = if config.restrack.is_some() {
            return Err(MonitorError::UnsupportedTrigger);
        } else {
            None
        };
        let coordinator = Arc::new(DumpCoordinator::new(
            backend,
            sidecar,
            Arc::clone(&control),
            DumpRequest {
                pid: initial.identity.pid,
                process_name: initial.name.clone(),
                kind: DumpKind::Manual,
                output: config.output.clone(),
                overwrite: config.overwrite,
                use_gcore: config.use_gcore,
                platform,
                cancellation: None,
                core_dump_mask: config.core_dump_mask,
            },
            config.dump_count,
            config.diagnostics,
        ));
        let identity = initial.identity;
        let polling = Duration::from_millis(config.polling_interval_ms);
        let snooze = Duration::from_secs(config.threshold_seconds);

        #[cfg(all(target_os = "linux", feature = "dotnet-triggers"))]
        if config.dotnet_trigger.is_some() {
            startup.push(crate::profiler::spawn_profiler_monitor(
                config,
                Arc::clone(&control),
                Arc::clone(&coordinator),
                identity,
            )?);
        }
        #[cfg(not(all(target_os = "linux", feature = "dotnet-triggers")))]
        if config.dotnet_trigger.is_some() {
            return Err(MonitorError::UnsupportedTrigger);
        }
        #[cfg(all(target_os = "linux", feature = "dotnet-triggers"))]
        if !config.perf_counters.is_empty() {
            startup.push(crate::eventpipe::spawn_counter_monitor(
                config,
                Arc::clone(&control),
                Arc::clone(&coordinator),
                identity,
            )?);
        }
        #[cfg(not(all(target_os = "linux", feature = "dotnet-triggers")))]
        if !config.perf_counters.is_empty() {
            return Err(MonitorError::UnsupportedTrigger);
        }

        if let Some(threshold) = config.cpu {
            startup.push(spawn_cpu(
                Arc::clone(&control),
                Arc::clone(&coordinator),
                Arc::clone(&metrics),
                identity,
                threshold,
                polling,
                snooze,
            )?);
        }
        if let Some(threshold) = &config.memory_mb {
            startup.push(spawn_memory(
                Arc::clone(&control),
                Arc::clone(&coordinator),
                Arc::clone(&metrics),
                identity,
                threshold.clone(),
                polling,
                snooze,
            )?);
        }
        if let Some(threshold) = config.thread_count {
            startup.push(spawn_count_monitor(
                "thread count monitor",
                Arc::clone(&control),
                Arc::clone(&coordinator),
                Arc::clone(&metrics),
                identity,
                threshold as u64,
                polling,
                snooze,
                DumpKind::Thread,
                |snapshot| snapshot.thread_count,
            )?);
        }
        if let Some(threshold) = config.file_descriptor_count {
            startup.push(spawn_count_monitor(
                "file descriptor monitor",
                Arc::clone(&control),
                Arc::clone(&coordinator),
                Arc::clone(&metrics),
                identity,
                threshold as u64,
                polling,
                snooze,
                DumpKind::FileDescriptor,
                |snapshot| snapshot.file_descriptor_count,
            )?);
        }
        #[cfg(target_os = "linux")]
        if !config.signals.is_empty() {
            startup.push(crate::signal::spawn_signal_monitor(
                Arc::clone(&control),
                Arc::clone(&coordinator),
                identity,
                config.signals.clone(),
            )?);
        }
        #[cfg(not(target_os = "linux"))]
        if !config.signals.is_empty() {
            return Err(MonitorError::UnsupportedTrigger);
        }
        if config.timer_trigger {
            startup.push(spawn_timer(Arc::clone(&control), coordinator, snooze)?);
        }
        if startup.threads.is_empty() {
            return Err(MonitorError::UnsupportedTrigger);
        }

        let threads = startup.finish();
        Ok(Self { control, threads })
    }

    pub fn has_finished(&self) -> bool {
        self.threads.iter().any(JoinHandle::is_finished)
    }

    pub fn request_quit(&self) {
        self.control.request_quit();
    }

    pub fn wait(self) -> Result<(), MonitorError> {
        let mut first_error = None;
        let mut threads = self.threads;
        while !threads.is_empty() {
            let Some(index) = threads.iter().position(JoinHandle::is_finished) else {
                thread::sleep(Duration::from_millis(10));
                continue;
            };
            let thread = threads.swap_remove(index);
            match thread.join() {
                Ok(Ok(())) => {
                    if !self.control.is_quit_requested() {
                        self.control.request_quit();
                    }
                }
                Ok(Err(error)) => {
                    self.control.request_quit();
                    if first_error.is_none() {
                        first_error = Some(error);
                    }
                }
                Err(_) => {
                    self.control.request_quit();
                    if first_error.is_none() {
                        first_error = Some(MonitorError::ThreadPanicked);
                    }
                }
            }
        }
        first_error.map_or(Ok(()), Err)
    }
}

fn spawn_timer(
    control: Arc<MonitorControl>,
    coordinator: Arc<DumpCoordinator>,
    snooze: Duration,
) -> Result<JoinHandle<Result<(), MonitorError>>, MonitorError> {
    thread::Builder::new()
        .name("timer monitor".into())
        .spawn(move || {
            if control.wait_for_start() == WaitOutcome::Quit {
                return Ok(());
            }
            loop {
                coordinator.write(DumpKind::Timer)?;
                if coordinator.limit_reached() || control.wait(snooze) != WaitOutcome::TimedOut {
                    return Ok(());
                }
            }
        })
        .map_err(MonitorError::Spawn)
}

fn spawn_cpu(
    control: Arc<MonitorControl>,
    coordinator: Arc<DumpCoordinator>,
    metrics: Arc<dyn ProcessMetrics>,
    identity: ProcessIdentity,
    threshold: Threshold<u32>,
    polling: Duration,
    snooze: Duration,
) -> Result<JoinHandle<Result<(), MonitorError>>, MonitorError> {
    thread::Builder::new()
        .name("cpu monitor".into())
        .spawn(move || {
            if control.wait_for_start() == WaitOutcome::Quit {
                return Ok(());
            }
            while control.wait(polling) == WaitOutcome::TimedOut {
                let snapshot = sample_identity(metrics.as_ref(), identity)?;
                let usage = metrics.cpu_usage_percent(&snapshot)?;
                if threshold_matches(threshold, usage) {
                    crate::diagnostics::info(
                        coordinator.diagnostics,
                        format!(
                            "Trigger: CPU usage:{usage}% on process ID: {}",
                            identity.pid.get()
                        ),
                    );
                    coordinator.write(DumpKind::Cpu)?;
                    if coordinator.limit_reached() || control.wait(snooze) != WaitOutcome::TimedOut
                    {
                        break;
                    }
                }
            }
            Ok(())
        })
        .map_err(MonitorError::Spawn)
}

fn spawn_memory(
    control: Arc<MonitorControl>,
    coordinator: Arc<DumpCoordinator>,
    metrics: Arc<dyn ProcessMetrics>,
    identity: ProcessIdentity,
    threshold: Threshold<Vec<u64>>,
    polling: Duration,
    snooze: Duration,
) -> Result<JoinHandle<Result<(), MonitorError>>, MonitorError> {
    thread::Builder::new()
        .name("memory monitor".into())
        .spawn(move || {
            if control.wait_for_start() == WaitOutcome::Quit {
                return Ok(());
            }
            let (below, thresholds) = match threshold {
                Threshold::AtLeast(values) => (false, values),
                Threshold::Below(values) => (true, values),
            };
            let mut current = 0;
            while current < thresholds.len() && control.wait(polling) == WaitOutcome::TimedOut {
                let snapshot = sample_identity(metrics.as_ref(), identity)?;
                let usage = snapshot.commit_megabytes();
                let triggered = if below {
                    usage < thresholds[current]
                } else {
                    usage >= thresholds[current]
                };
                if triggered {
                    crate::diagnostics::info(
                        coordinator.diagnostics,
                        format!(
                            "Trigger: Commit usage:{usage}MB on process ID: {}",
                            identity.pid.get()
                        ),
                    );
                    coordinator.write(DumpKind::Commit)?;
                    current += 1;
                    if coordinator.limit_reached() || control.wait(snooze) != WaitOutcome::TimedOut
                    {
                        break;
                    }
                }
            }
            Ok(())
        })
        .map_err(MonitorError::Spawn)
}

#[allow(clippy::too_many_arguments)]
fn spawn_count_monitor<F>(
    name: &'static str,
    control: Arc<MonitorControl>,
    coordinator: Arc<DumpCoordinator>,
    metrics: Arc<dyn ProcessMetrics>,
    identity: ProcessIdentity,
    threshold: u64,
    polling: Duration,
    snooze: Duration,
    kind: DumpKind,
    value: F,
) -> Result<JoinHandle<Result<(), MonitorError>>, MonitorError>
where
    F: Fn(&ProcessSnapshot) -> u64 + Send + 'static,
{
    thread::Builder::new()
        .name(name.into())
        .spawn(move || {
            if control.wait_for_start() == WaitOutcome::Quit {
                return Ok(());
            }
            while control.wait(polling) == WaitOutcome::TimedOut {
                let snapshot = sample_identity(metrics.as_ref(), identity)?;
                if value(&snapshot) >= threshold {
                    coordinator.write(kind)?;
                    if coordinator.limit_reached() || control.wait(snooze) != WaitOutcome::TimedOut
                    {
                        break;
                    }
                }
            }
            Ok(())
        })
        .map_err(MonitorError::Spawn)
}

fn sample_identity(
    metrics: &dyn ProcessMetrics,
    identity: ProcessIdentity,
) -> Result<ProcessSnapshot, MonitorError> {
    let snapshot = metrics.sample(identity.pid)?;
    if snapshot.identity != identity {
        return Err(MonitorError::PidReused(identity.pid.get()));
    }
    Ok(snapshot)
}

fn threshold_matches(threshold: Threshold<u32>, value: u32) -> bool {
    match threshold {
        Threshold::AtLeast(threshold) => value >= threshold,
        Threshold::Below(threshold) => value < threshold,
    }
}

#[derive(Debug)]
pub enum MonitorError {
    UnsupportedTrigger,
    Spawn(std::io::Error),
    Process(ProcessError),
    Dump(DumpError),
    #[cfg(target_os = "linux")]
    EventPipe(String),
    #[cfg(target_os = "linux")]
    Profiler(String),
    #[cfg(target_os = "linux")]
    Restrack(String),
    #[cfg(target_os = "linux")]
    Signal(String),
    PidReused(i32),
    ThreadPanicked,
}

impl fmt::Display for MonitorError {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::UnsupportedTrigger => {
                write!(
                    formatter,
                    "the selected trigger configuration is unsupported"
                )
            }
            Self::Spawn(error) => write!(formatter, "failed to create monitor thread: {error}"),
            Self::Process(error) => error.fmt(formatter),
            Self::Dump(error) => error.fmt(formatter),
            #[cfg(target_os = "linux")]
            Self::EventPipe(error) => error.fmt(formatter),
            #[cfg(target_os = "linux")]
            Self::Profiler(error) => error.fmt(formatter),
            #[cfg(target_os = "linux")]
            Self::Restrack(error) => error.fmt(formatter),
            #[cfg(target_os = "linux")]
            Self::Signal(error) => error.fmt(formatter),
            Self::PidReused(pid) => write!(formatter, "process ID {pid} was reused"),
            Self::ThreadPanicked => write!(formatter, "a monitor thread panicked"),
        }
    }
}

impl std::error::Error for MonitorError {
    fn source(&self) -> Option<&(dyn std::error::Error + 'static)> {
        match self {
            Self::Spawn(error) => Some(error),
            Self::Process(error) => Some(error),
            Self::Dump(error) => Some(error),
            _ => None,
        }
    }
}

impl From<ProcessError> for MonitorError {
    fn from(value: ProcessError) -> Self {
        Self::Process(value)
    }
}

impl From<DumpError> for MonitorError {
    fn from(value: DumpError) -> Self {
        Self::Dump(value)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::config::{DiagnosticsTarget, OutputSpec, TargetSpec};
    use crate::process::{ProcessId, ProcessState};
    use std::ffi::OsString;
    use std::sync::Mutex;
    use std::sync::atomic::{AtomicBool, Ordering};

    #[derive(Clone)]
    struct FixedMetrics(ProcessSnapshot);

    impl ProcessMetrics for FixedMetrics {
        fn sample(&self, _pid: crate::process::ProcessId) -> Result<ProcessSnapshot, ProcessError> {
            Ok(self.0.clone())
        }

        fn cpu_usage_percent(&self, _snapshot: &ProcessSnapshot) -> Result<u32, ProcessError> {
            Ok(80)
        }
    }

    #[derive(Default)]
    struct RecordingBackend(Mutex<Vec<DumpKind>>);

    impl DumpBackend for RecordingBackend {
        fn write_dump(&self, request: &DumpRequest) -> Result<PathBuf, DumpError> {
            self.0.lock().unwrap().push(request.kind);
            Ok(PathBuf::from(format!("dump.{}", request.pid.get())))
        }
    }

    struct RecordingSidecar {
        generate_dump: bool,
        kinds: Mutex<Vec<DumpKind>>,
    }

    impl DumpSidecar for RecordingSidecar {
        fn generate_primary_dump(&self) -> bool {
            self.generate_dump
        }

        fn write(
            &self,
            kind: DumpKind,
            _primary_path: Option<&std::path::Path>,
        ) -> Result<PathBuf, MonitorError> {
            self.kinds.lock().unwrap().push(kind);
            Ok(PathBuf::from("report.restrack"))
        }
    }

    fn snapshot() -> ProcessSnapshot {
        ProcessSnapshot {
            identity: ProcessIdentity {
                pid: ProcessId::new(42).unwrap(),
                start_time: 100,
            },
            name: OsString::from("worker"),
            state: ProcessState::Running,
            parent_pid: 1,
            process_group: 42,
            cpu_time_ticks: 100,
            rss_bytes: 100 * 1024 * 1024,
            swap_bytes: 0,
            thread_count: 10,
            file_descriptor_count: 20,
        }
    }

    fn config() -> Config {
        Config {
            target: TargetSpec::Pid(42),
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
        }
    }

    #[test]
    fn timer_monitor_writes_one_dump_and_exits() {
        let backend = Arc::new(RecordingBackend::default());
        let monitor = MonitorSet::start(
            &config(),
            Platform::Linux,
            snapshot(),
            Arc::new(FixedMetrics(snapshot())),
            backend.clone(),
        )
        .unwrap();

        monitor.wait().unwrap();
        assert_eq!(*backend.0.lock().unwrap(), vec![DumpKind::Timer]);
    }

    #[test]
    fn cpu_monitor_uses_dedicated_trigger_kind() {
        let backend = Arc::new(RecordingBackend::default());
        let mut config = config();
        config.timer_trigger = false;
        config.cpu = Some(Threshold::AtLeast(50));
        let monitor = MonitorSet::start(
            &config,
            Platform::Linux,
            snapshot(),
            Arc::new(FixedMetrics(snapshot())),
            backend.clone(),
        )
        .unwrap();

        monitor.wait().unwrap();
        assert_eq!(*backend.0.lock().unwrap(), vec![DumpKind::Cpu]);
    }

    #[test]
    fn nodump_sidecar_completes_trigger_without_primary_dump() {
        let backend = Arc::new(RecordingBackend::default());
        let sidecar = Arc::new(RecordingSidecar {
            generate_dump: false,
            kinds: Mutex::new(Vec::new()),
        });
        let control = Arc::new(MonitorControl::new());
        let request = DumpRequest {
            pid: snapshot().identity.pid,
            process_name: snapshot().name,
            kind: DumpKind::Manual,
            output: OutputSpec::default(),
            overwrite: false,
            use_gcore: false,
            platform: Platform::Linux,
            cancellation: None,
            core_dump_mask: None,
        };
        let coordinator = DumpCoordinator::new(
            backend.clone(),
            Some(sidecar.clone()),
            control,
            request,
            1,
            DiagnosticsTarget::None,
        );

        assert_eq!(
            coordinator.write(DumpKind::Timer).unwrap(),
            Some(PathBuf::from("report.restrack"))
        );
        assert!(backend.0.lock().unwrap().is_empty());
        assert_eq!(*sidecar.kinds.lock().unwrap(), vec![DumpKind::Timer]);
        assert_eq!(coordinator.collected(), 1);
    }

    #[test]
    fn startup_guard_cancels_and_joins_waiting_workers() {
        let control = Arc::new(MonitorControl::new());
        let finished = Arc::new(AtomicBool::new(false));
        let waiting_control = Arc::clone(&control);
        let worker_finished = Arc::clone(&finished);
        let worker = thread::spawn(move || {
            assert_eq!(waiting_control.wait_for_start(), WaitOutcome::Quit);
            worker_finished.store(true, Ordering::Release);
            Ok(())
        });

        let mut startup = StartupGuard::new(control);
        startup.push(worker);
        drop(startup);

        assert!(finished.load(Ordering::Acquire));
    }

    #[cfg(all(target_os = "linux", feature = "dotnet-triggers"))]
    #[test]
    fn external_dumps_share_the_coordinator_limit() {
        let backend = Arc::new(RecordingBackend::default());
        let control = Arc::new(MonitorControl::new());
        let request = DumpRequest {
            pid: snapshot().identity.pid,
            process_name: snapshot().name,
            kind: DumpKind::Manual,
            output: OutputSpec::default(),
            overwrite: false,
            use_gcore: false,
            platform: Platform::Linux,
            cancellation: None,
            core_dump_mask: None,
        };
        let coordinator =
            DumpCoordinator::new(backend, None, control, request, 1, DiagnosticsTarget::None);

        assert!(coordinator.record_external_dump(std::path::Path::new("managed.core")));
        assert!(!coordinator.record_external_dump(std::path::Path::new("extra.core")));
        assert_eq!(coordinator.collected(), 1);
    }
}
