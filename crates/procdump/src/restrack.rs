#![allow(unsafe_code)]

use crate::config::{Config, Platform, RestrackConfig};
use crate::dump::{DumpKind, DumpRequest, sidecar_path};
use crate::monitor::{DumpSidecar, MonitorError};
use crate::process::ProcessSnapshot;
use crate::sync::{MonitorControl, WaitOutcome};
use blazesym::Pid;
use blazesym::symbolize::source::{Process, Source};
use blazesym::symbolize::{Input, Symbolizer};
use libbpf_rs::skel::{OpenSkel, Skel, SkelBuilder};
use libbpf_rs::{MapCore, MapFlags, RingBufferBuilder};
use std::collections::HashMap;
use std::fs::File;
use std::io::{Read, Write};
use std::mem::MaybeUninit;
use std::os::fd::AsRawFd;
use std::os::unix::fs::MetadataExt;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::mpsc::{SyncSender, sync_channel};
use std::sync::{Arc, Mutex};
use std::thread::{self, JoinHandle};
use std::time::Duration;

#[allow(unsafe_code)]
mod bpf {
    include!(concat!(env!("OUT_DIR"), "/procdump_ebpf.skel.rs"));
}

const RESTRACK_ALLOC: u32 = 1;
const RESTRACK_FREE: u32 = 2;
const EVENT_HEADER_SIZE: usize = 40;
const MAX_CALL_STACK_FRAMES: usize = 100;

#[derive(Clone, Debug, Eq, Hash, PartialEq)]
struct Allocation {
    size: u64,
    stack: Vec<u64>,
}

type Allocations = Arc<Mutex<HashMap<u64, Arc<Allocation>>>>;

pub(crate) struct RestrackRuntime {
    pub threads: Vec<JoinHandle<Result<(), MonitorError>>>,
    pub reporter: Arc<dyn DumpSidecar>,
}

struct RestrackReporter {
    allocations: Allocations,
    incomplete: Arc<AtomicBool>,
    config: RestrackConfig,
    request: DumpRequest,
    diagnostics: crate::config::DiagnosticsTarget,
}

impl DumpSidecar for RestrackReporter {
    fn generate_primary_dump(&self) -> bool {
        self.config.generate_dump
    }

    fn write(
        &self,
        kind: DumpKind,
        primary_path: Option<&std::path::Path>,
    ) -> Result<std::path::PathBuf, MonitorError> {
        let mut request = self.request.clone();
        request.kind = kind;
        let path = restrack_path(&request, primary_path)?;
        let file = crate::engine::open_output_file(&path, request.overwrite).map_err(|error| {
            MonitorError::Restrack(format!("failed to create {}: {error}", path.display()))
        })?;
        let snapshot = match self.allocations.lock() {
            Ok(allocations) => allocations.clone(),
            Err(poisoned) => {
                self.incomplete.store(true, Ordering::Release);
                poisoned.into_inner().clone()
            }
        };
        let pid = request.pid.get();
        render_report(
            file,
            pid,
            &self.config,
            snapshot,
            self.incomplete.load(Ordering::Acquire),
        )
        .map_err(MonitorError::Restrack)?;
        crate::diagnostics::info(self.diagnostics, crate::cli_output::leak_report(&path));
        Ok(path)
    }
}

pub(crate) fn spawn_restrack_monitors(
    config: &Config,
    control: Arc<MonitorControl>,
    process: ProcessSnapshot,
    platform: Platform,
) -> Result<RestrackRuntime, MonitorError> {
    let restrack = config.restrack.clone().ok_or_else(|| {
        MonitorError::Restrack("resource tracking configuration is missing".into())
    })?;
    let allocations = Arc::new(Mutex::new(HashMap::new()));
    let incomplete = Arc::new(AtomicBool::new(false));
    let (ready_tx, ready_rx) = sync_channel(1);
    let collector_control = Arc::clone(&control);
    let collector_allocations = Arc::clone(&allocations);
    let collector_incomplete = Arc::clone(&incomplete);
    let pid = process.identity.pid.get();
    let sample_rate = restrack.sample_rate;
    let collector = thread::Builder::new()
        .name("restrack collector".into())
        .spawn(move || {
            let result = collect_events(
                pid,
                sample_rate,
                collector_control,
                collector_allocations,
                collector_incomplete,
                &ready_tx,
            );
            if let Err(error) = &result {
                let _ = ready_tx.send(Err(error.clone()));
            }
            result.map_err(MonitorError::Restrack)
        })
        .map_err(MonitorError::Spawn)?;

    match ready_rx.recv() {
        Ok(Ok(())) => {}
        Ok(Err(error)) => {
            let _ = collector.join();
            return Err(MonitorError::Restrack(error));
        }
        Err(error) => {
            let _ = collector.join();
            return Err(MonitorError::Restrack(format!(
                "resource tracking initialization ended unexpectedly: {error}"
            )));
        }
    }

    let reporter = Arc::new(RestrackReporter {
        allocations,
        incomplete,
        config: restrack,
        request: DumpRequest {
            pid: process.identity.pid,
            process_name: process.name,
            kind: DumpKind::Manual,
            output: config.output.clone(),
            overwrite: config.overwrite,
            use_gcore: config.use_gcore,
            platform,
            cancellation: None,
            core_dump_mask: config.core_dump_mask,
        },
        diagnostics: config.diagnostics,
    });
    let mut threads = vec![collector];
    if needs_manual_trigger(config) {
        threads.push(spawn_manual_trigger(control, Arc::clone(&reporter))?);
    }
    Ok(RestrackRuntime { threads, reporter })
}

fn collect_events(
    pid: i32,
    sample_rate: u32,
    control: Arc<MonitorControl>,
    allocations: Allocations,
    incomplete: Arc<AtomicBool>,
    ready: &SyncSender<Result<(), String>>,
) -> Result<(), String> {
    let namespace = std::fs::metadata(format!("/proc/{pid}/ns/pid"))
        .map_err(|error| format!("failed to inspect the target PID namespace: {error}"))?;

    let mut open_object = MaybeUninit::uninit();
    let mut open_skeleton = bpf::ProcdumpEbpfSkelBuilder::default()
        .open(&mut open_object)
        .map_err(|error| format!("failed to open the resource tracking eBPF program: {error}"))?;
    let bss = open_skeleton
        .maps
        .bss_data
        .as_mut()
        .ok_or_else(|| "resource tracking eBPF BSS data is unavailable".to_string())?;
    bss.target_PID = pid;
    bss.dev = namespace.dev() as u32;
    bss.inode = namespace.ino() as u32;
    bss.sampleRate = sample_rate as i32;

    let mut skeleton = open_skeleton
        .load()
        .map_err(|error| format!("failed to load the resource tracking eBPF program: {error}"))?;
    skeleton
        .attach()
        .map_err(|error| format!("failed to attach the resource tracking eBPF probes: {error}"))?;

    let callback_allocations = Arc::clone(&allocations);
    let callback_incomplete = Arc::clone(&incomplete);
    let mut ring_builder = RingBufferBuilder::new();
    ring_builder
        .add(&skeleton.maps.ringBuffer, move |data| {
            handle_event(data, &callback_allocations, &callback_incomplete);
            0
        })
        .map_err(|error| {
            format!("failed to configure the resource tracking ring buffer: {error}")
        })?;
    let ring = ring_builder
        .build()
        .map_err(|error| format!("failed to create the resource tracking ring buffer: {error}"))?;
    ready
        .send(Ok(()))
        .map_err(|_| "resource tracking startup was cancelled".to_string())?;

    if control.wait_for_start() == WaitOutcome::Quit {
        return Ok(());
    }
    while !control.is_quit_requested() {
        ring.poll(Duration::from_millis(100)).map_err(|error| {
            format!("failed to poll the resource tracking ring buffer: {error}")
        })?;
        let mut lost = [0_u8; size_of::<u64>()];
        skeleton
            .maps
            .eventStats
            .lookup_into(&0_i32.to_ne_bytes(), &mut lost, MapFlags::ANY)
            .map_err(|error| format!("failed to read resource tracking loss counter: {error}"))?;
        if u64::from_ne_bytes(lost) > 0 {
            incomplete.store(true, Ordering::Release);
        }
    }
    Ok(())
}

fn handle_event(data: &[u8], allocations: &Allocations, incomplete: &AtomicBool) {
    if data.len() < EVENT_HEADER_SIZE {
        return;
    }
    let address = read_u64(data, 0);
    let resource_type = read_u32(data, 16);
    let Ok(mut current) = allocations.lock() else {
        incomplete.store(true, Ordering::Release);
        return;
    };
    match resource_type {
        RESTRACK_ALLOC => {
            let size = read_u64(data, 24);
            let frame_count = read_i64(data, 32).clamp(0, MAX_CALL_STACK_FRAMES as i64) as usize;
            let available_frames = data.len().saturating_sub(EVENT_HEADER_SIZE) / size_of::<u64>();
            let frame_count = frame_count.min(available_frames);
            let stack = (0..frame_count)
                .map(|index| read_u64(data, EVENT_HEADER_SIZE + index * size_of::<u64>()))
                .take_while(|address| *address != 0)
                .collect();
            current.insert(address, Arc::new(Allocation { size, stack }));
        }
        RESTRACK_FREE => {
            current.remove(&address);
        }
        _ => {}
    }
}

fn spawn_manual_trigger(
    control: Arc<MonitorControl>,
    reporter: Arc<RestrackReporter>,
) -> Result<JoinHandle<Result<(), MonitorError>>, MonitorError> {
    thread::Builder::new()
        .name("restrack manual trigger".into())
        .spawn(move || {
            if control.wait_for_start() == WaitOutcome::Quit {
                return Ok(());
            }
            crate::diagnostics::info(reporter.diagnostics, crate::cli_output::RESTRACK_PROMPT);
            let result = match wait_for_manual_input(&control) {
                Ok(None) => Ok(()),
                Ok(Some(input)) if input.eq_ignore_ascii_case(&b't') => {
                    crate::diagnostics::info(
                        reporter.diagnostics,
                        crate::cli_output::RESTRACK_TRIGGERED,
                    );
                    reporter.write(DumpKind::Manual, None).map(|_| ())
                }
                Ok(Some(_)) => Ok(()),
                Err(error) => Err(MonitorError::Restrack(format!(
                    "failed to read the resource tracking trigger: {error}"
                ))),
            };
            control.request_quit();
            result
        })
        .map_err(MonitorError::Spawn)
}

fn wait_for_manual_input(control: &MonitorControl) -> std::io::Result<Option<u8>> {
    let stdin = std::io::stdin();
    let mut poll_fd = libc::pollfd {
        fd: stdin.as_raw_fd(),
        events: libc::POLLIN,
        revents: 0,
    };
    loop {
        if control.is_quit_requested() {
            return Ok(None);
        }
        let result = unsafe { libc::poll(&raw mut poll_fd, 1, 100) };
        if result == -1 {
            let error = std::io::Error::last_os_error();
            if error.kind() == std::io::ErrorKind::Interrupted {
                continue;
            }
            return Err(error);
        }
        if result > 0 && poll_fd.revents & libc::POLLIN != 0 {
            let mut input = [0_u8; 1];
            return std::io::stdin()
                .read(&mut input)
                .map(|count| (count > 0).then_some(input[0]));
        }
    }
}

fn render_report(
    mut file: File,
    pid: i32,
    config: &RestrackConfig,
    snapshot: HashMap<u64, Arc<Allocation>>,
    incomplete: bool,
) -> Result<(), String> {
    let mut grouped: HashMap<Arc<Allocation>, u64> = HashMap::new();
    for allocation in snapshot.into_values() {
        *grouped.entry(allocation).or_default() += 1;
    }
    let mut grouped: Vec<_> = grouped.into_iter().collect();
    grouped.sort_by_key(|(allocation, count)| {
        std::cmp::Reverse(allocation.size.saturating_mul(*count))
    });

    if grouped.is_empty() {
        file.write_all(b"No leaks detected.\n")
            .map_err(|error| format!("failed to write restrack report: {error}"))?;
        write_incomplete_warning(&mut file, incomplete)?;
        return Ok(());
    }

    let symbolizer = Symbolizer::builder().enable_demangling(false).build();
    let mut process = Process::new(Pid::from(pid as u32));
    process.debug_syms = false;
    process.map_files = false;
    let source = Source::Process(process);
    let mut total = 0_u64;
    let mut missing_symbols = false;
    for (allocation, count) in grouped {
        let frame_lines = symbolize_stack(&symbolizer, &source, &allocation.stack);
        missing_symbols |= frame_lines.iter().any(|frame| !frame.symbolized);
        if config.exclude_filter.as_ref().is_some_and(|filter| {
            frame_lines
                .iter()
                .any(|frame| wildcard_matches(&frame.text, &filter.to_string_lossy()))
        }) {
            continue;
        }
        let allocation_total = allocation.size.saturating_mul(count);
        total = total.saturating_add(allocation_total);
        writeln!(
            file,
            "+++ Leaked Allocation [allocation size: 0x{:x} count:0x{count:x} total size:0x{allocation_total:x}]",
            allocation.size
        )
        .map_err(|error| format!("failed to write restrack report: {error}"))?;
        for frame in frame_lines {
            writeln!(file, "{}", frame.text)
                .map_err(|error| format!("failed to write restrack report: {error}"))?;
        }
        writeln!(file).map_err(|error| format!("failed to write restrack report: {error}"))?;
    }
    writeln!(file, "\nTotal leaked: 0x{total:x}")
        .map_err(|error| format!("failed to write restrack report: {error}"))?;
    if missing_symbols {
        writeln!(
            file,
            "\n[INFO] Some call stack frames could not be resolved to symbols. This may indicate missing debug symbols."
        )
        .map_err(|error| format!("failed to write restrack report: {error}"))?;
    }
    write_incomplete_warning(&mut file, incomplete)?;
    Ok(())
}

fn write_incomplete_warning(file: &mut File, incomplete: bool) -> Result<(), String> {
    if incomplete {
        writeln!(
            file,
            "\n[WARNING] Resource tracking events were dropped; this report is incomplete."
        )
        .map_err(|error| format!("failed to write restrack report: {error}"))?;
    }
    Ok(())
}

fn restrack_path(
    request: &DumpRequest,
    primary_path: Option<&std::path::Path>,
) -> Result<std::path::PathBuf, MonitorError> {
    primary_path.map_or_else(
        || sidecar_path(request, "restrack").map_err(MonitorError::from),
        |path| {
            Ok(std::path::PathBuf::from(format!(
                "{}.restrack",
                path.display()
            )))
        },
    )
}

struct StackFrame {
    text: String,
    symbolized: bool,
}

fn symbolize_stack(symbolizer: &Symbolizer, source: &Source<'_>, stack: &[u64]) -> Vec<StackFrame> {
    let symbols = symbolizer.symbolize(source, Input::AbsAddr(stack));
    stack
        .iter()
        .enumerate()
        .map(|(index, address)| {
            let symbol = symbols
                .as_ref()
                .ok()
                .and_then(|symbols| symbols.get(index))
                .and_then(|symbol| symbol.as_sym());
            match symbol {
                Some(symbol) => StackFrame {
                    text: format!("\t[0x{address:x}] {}+0x{:x}", symbol.name, symbol.offset),
                    symbolized: true,
                },
                None => StackFrame {
                    text: format!("\t[0x{address:x}]"),
                    symbolized: false,
                },
            }
        })
        .collect()
}

fn needs_manual_trigger(config: &Config) -> bool {
    !config.timer_trigger
        && config.cpu.is_none()
        && config.memory_mb.is_none()
        && config.thread_count.is_none()
        && config.file_descriptor_count.is_none()
        && config.dotnet_trigger.is_none()
        && config.signals.is_empty()
        && config.perf_counters.is_empty()
}

fn read_u32(data: &[u8], offset: usize) -> u32 {
    u32::from_ne_bytes(data[offset..offset + size_of::<u32>()].try_into().unwrap())
}

fn read_u64(data: &[u8], offset: usize) -> u64 {
    u64::from_ne_bytes(data[offset..offset + size_of::<u64>()].try_into().unwrap())
}

fn read_i64(data: &[u8], offset: usize) -> i64 {
    i64::from_ne_bytes(data[offset..offset + size_of::<i64>()].try_into().unwrap())
}

fn wildcard_matches(value: &str, pattern: &str) -> bool {
    let value = value.to_ascii_lowercase().into_bytes();
    let pattern = pattern.to_ascii_lowercase().into_bytes();
    let (mut value_index, mut pattern_index) = (0, 0);
    let (mut wildcard, mut restart) = (None, 0);
    while value_index < value.len() {
        if pattern.get(pattern_index) == Some(&value[value_index]) {
            value_index += 1;
            pattern_index += 1;
        } else if pattern.get(pattern_index) == Some(&b'*') {
            wildcard = Some(pattern_index);
            pattern_index += 1;
            restart = value_index;
        } else if let Some(index) = wildcard {
            pattern_index = index + 1;
            restart += 1;
            value_index = restart;
        } else {
            return false;
        }
    }
    pattern[pattern_index..].iter().all(|byte| *byte == b'*')
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn allocation_and_free_events_update_the_map() {
        let allocations = Arc::new(Mutex::new(HashMap::new()));
        let incomplete = AtomicBool::new(false);
        let mut event = vec![0_u8; EVENT_HEADER_SIZE + size_of::<u64>()];
        event[0..8].copy_from_slice(&42_u64.to_ne_bytes());
        event[16..20].copy_from_slice(&RESTRACK_ALLOC.to_ne_bytes());
        event[24..32].copy_from_slice(&64_u64.to_ne_bytes());
        event[32..40].copy_from_slice(&1_i64.to_ne_bytes());
        event[40..48].copy_from_slice(&99_u64.to_ne_bytes());
        handle_event(&event, &allocations, &incomplete);

        let allocation = allocations.lock().unwrap().get(&42).cloned().unwrap();
        assert_eq!(allocation.size, 64);
        assert_eq!(allocation.stack, vec![99]);

        event[16..20].copy_from_slice(&RESTRACK_FREE.to_ne_bytes());
        handle_event(&event, &allocations, &incomplete);
        assert!(allocations.lock().unwrap().is_empty());
    }

    #[test]
    fn wildcard_matching_is_case_insensitive() {
        assert!(wildcard_matches("[0x123] malloc+0x4", "*MALLOC*"));
        assert!(!wildcard_matches("[0x123] calloc+0x4", "*malloc*"));
    }

    #[test]
    fn process_symbolizer_resolves_libc_function() {
        let symbolizer = Symbolizer::builder().enable_demangling(false).build();
        let mut process = Process::new(Pid::Slf);
        process.debug_syms = false;
        process.map_files = false;
        let source = Source::Process(process);
        let stack = [libc::malloc as *const () as usize as u64];
        let frames = symbolize_stack(&symbolizer, &source, &stack);

        assert!(frames[0].symbolized);
        assert!(frames[0].text.contains("malloc"));
    }

    #[test]
    fn manual_input_returns_when_monitor_is_cancelled() {
        let control = MonitorControl::new();
        control.request_quit();

        assert_eq!(wait_for_manual_input(&control).unwrap(), None);
    }

    #[test]
    fn sidecar_uses_exact_primary_dump_path() {
        let request = DumpRequest {
            pid: crate::process::ProcessId::new(42).unwrap(),
            process_name: "worker".into(),
            kind: DumpKind::Manual,
            output: crate::config::OutputSpec::default(),
            overwrite: false,
            use_gcore: false,
            platform: Platform::Linux,
            cancellation: None,
            core_dump_mask: None,
        };

        assert_eq!(
            restrack_path(&request, Some(std::path::Path::new("/tmp/dump.42"))).unwrap(),
            std::path::PathBuf::from("/tmp/dump.42.restrack")
        );
    }

    #[test]
    fn incomplete_empty_report_is_marked() {
        let path =
            std::env::temp_dir().join(format!("procdump-incomplete-report-{}", std::process::id()));
        let file = File::create(&path).unwrap();
        let config = RestrackConfig {
            generate_dump: false,
            sample_rate: 1,
            exclude_filter: None,
        };

        render_report(
            file,
            std::process::id() as i32,
            &config,
            HashMap::new(),
            true,
        )
        .unwrap();

        let contents = std::fs::read_to_string(&path).unwrap();
        assert!(contents.contains("report is incomplete"));
        std::fs::remove_file(path).unwrap();
    }
}
