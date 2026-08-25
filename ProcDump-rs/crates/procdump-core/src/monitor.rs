use crate::config::{Config, Platform, Threshold};
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
    gate: DumpGate,
    control: Arc<MonitorControl>,
    request: DumpRequest,
    collected: AtomicU32,
    limit: u32,
}

impl DumpCoordinator {
    pub fn new(
        backend: Arc<dyn DumpBackend>,
        control: Arc<MonitorControl>,
        request: DumpRequest,
        limit: u32,
    ) -> Self {
        Self {
            backend,
            gate: DumpGate::new(),
            control,
            request,
            collected: AtomicU32::new(0),
            limit,
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
        let path = self.backend.write_dump(&request)?;
        let dump_number = self.collected.fetch_add(1, Ordering::AcqRel);
        println!("Core dump {dump_number} generated: {}", path.display());
        if dump_number + 1 >= self.limit {
            self.control.request_quit();
        }
        Ok(Some(path))
    }
}

pub struct MonitorSet {
    control: Arc<MonitorControl>,
    threads: Vec<JoinHandle<Result<(), MonitorError>>>,
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
        let coordinator = Arc::new(DumpCoordinator::new(
            backend,
            Arc::clone(&control),
            DumpRequest {
                pid: initial.identity.pid,
                process_name: initial.name.clone(),
                kind: DumpKind::Manual,
                output: config.output.clone(),
                overwrite: config.overwrite,
                use_gcore: config.use_gcore,
                platform,
            },
            config.dump_count,
        ));
        let identity = initial.identity;
        let polling = Duration::from_millis(config.polling_interval_ms);
        let snooze = Duration::from_secs(config.threshold_seconds);
        let mut threads = Vec::new();

        #[cfg(target_os = "linux")]
        if config.restrack.is_some() {
            threads.extend(crate::restrack::spawn_restrack_monitors(
                config,
                Arc::clone(&control),
                initial.clone(),
                platform,
            )?);
        }
        #[cfg(not(target_os = "linux"))]
        if config.restrack.is_some() {
            return Err(MonitorError::UnsupportedTrigger);
        }

        #[cfg(target_os = "linux")]
        if config.dotnet_trigger.is_some() {
            threads.push(crate::profiler::spawn_profiler_monitor(
                config,
                Arc::clone(&control),
                identity,
            )?);
        }
        #[cfg(not(target_os = "linux"))]
        if config.dotnet_trigger.is_some() {
            return Err(MonitorError::UnsupportedTrigger);
        }
        #[cfg(target_os = "linux")]
        if !config.perf_counters.is_empty() {
            threads.push(crate::eventpipe::spawn_counter_monitor(
                config,
                Arc::clone(&control),
                Arc::clone(&coordinator),
                identity,
            )?);
        }
        #[cfg(not(target_os = "linux"))]
        if !config.perf_counters.is_empty() {
            return Err(MonitorError::UnsupportedTrigger);
        }

        if let Some(threshold) = config.cpu {
            threads.push(spawn_cpu(
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
            threads.push(spawn_memory(
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
            threads.push(spawn_count_monitor(
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
            threads.push(spawn_count_monitor(
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
            threads.push(crate::signal::spawn_signal_monitor(
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
            threads.push(spawn_timer(Arc::clone(&control), coordinator, snooze)?);
        }
        if threads.is_empty() {
            return Err(MonitorError::UnsupportedTrigger);
        }

        control.start();
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
        for thread in self.threads {
            match thread.join() {
                Ok(Ok(())) => {}
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
                    println!(
                        "Trigger: CPU usage:{usage}% on process ID: {}",
                        identity.pid.get()
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
                    println!(
                        "Trigger: Commit usage:{usage}MB on process ID: {}",
                        identity.pid.get()
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
                write!(formatter, "the selected trigger is not implemented yet")
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
}
