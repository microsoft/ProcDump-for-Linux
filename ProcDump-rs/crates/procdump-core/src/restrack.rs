#![allow(unsafe_code)]

use crate::config::{Config, Platform, RestrackConfig};
use crate::dump::{DumpKind, DumpRequest, sidecar_path};
use crate::monitor::MonitorError;
use crate::process::ProcessSnapshot;
use crate::sync::{MonitorControl, WaitOutcome};
use libbpf_rs::RingBufferBuilder;
use libbpf_rs::skel::{OpenSkel, Skel, SkelBuilder};
use std::collections::HashMap;
use std::fs::File;
use std::io::{Read, Write};
use std::mem::MaybeUninit;
use std::os::unix::fs::MetadataExt;
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

type Allocations = Arc<Mutex<HashMap<u64, Allocation>>>;

pub fn spawn_restrack_monitors(
    config: &Config,
    control: Arc<MonitorControl>,
    process: ProcessSnapshot,
    platform: Platform,
) -> Result<Vec<JoinHandle<Result<(), MonitorError>>>, MonitorError> {
    let restrack = config.restrack.clone().ok_or_else(|| {
        MonitorError::Restrack("resource tracking configuration is missing".into())
    })?;
    let allocations = Arc::new(Mutex::new(HashMap::new()));
    let (ready_tx, ready_rx) = sync_channel(1);
    let collector_control = Arc::clone(&control);
    let collector_allocations = Arc::clone(&allocations);
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

    let mut threads = vec![collector];
    if needs_manual_trigger(config) {
        let request = DumpRequest {
            pid: process.identity.pid,
            process_name: process.name,
            kind: DumpKind::Manual,
            output: config.output.clone(),
            overwrite: config.overwrite,
            platform,
        };
        threads.push(spawn_manual_trigger(
            control,
            allocations,
            restrack,
            request,
        )?);
    }
    Ok(threads)
}

fn collect_events(
    pid: i32,
    sample_rate: u32,
    control: Arc<MonitorControl>,
    allocations: Allocations,
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
    bss.currentSampleCount = 1;

    let mut skeleton = open_skeleton
        .load()
        .map_err(|error| format!("failed to load the resource tracking eBPF program: {error}"))?;
    skeleton
        .attach()
        .map_err(|error| format!("failed to attach the resource tracking eBPF probes: {error}"))?;

    let callback_allocations = Arc::clone(&allocations);
    let mut ring_builder = RingBufferBuilder::new();
    ring_builder
        .add(&skeleton.maps.ringBuffer, move |data| {
            handle_event(data, &callback_allocations);
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
    }
    Ok(())
}

fn handle_event(data: &[u8], allocations: &Allocations) {
    if data.len() < EVENT_HEADER_SIZE {
        return;
    }
    let address = read_u64(data, 0);
    let resource_type = read_u32(data, 16);
    let mut current = allocations
        .lock()
        .unwrap_or_else(|poisoned| poisoned.into_inner());
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
            current.insert(address, Allocation { size, stack });
        }
        RESTRACK_FREE => {
            current.remove(&address);
        }
        _ => {}
    }
}

fn spawn_manual_trigger(
    control: Arc<MonitorControl>,
    allocations: Allocations,
    config: RestrackConfig,
    request: DumpRequest,
) -> Result<JoinHandle<Result<(), MonitorError>>, MonitorError> {
    thread::Builder::new()
        .name("restrack manual trigger".into())
        .spawn(move || {
            if control.wait_for_start() == WaitOutcome::Quit {
                return Ok(());
            }
            println!("Press 't' to trigger a Restrack snapshot (or any other key to exit)...");
            let mut input = [0_u8; 1];
            let result = match std::io::stdin().read(&mut input) {
                Ok(0) => Ok(()),
                Ok(_) if input[0].eq_ignore_ascii_case(&b't') => {
                    println!("Triggering Restrack snapshot...");
                    write_report(&request, &config, &allocations).map(|path| {
                        println!("Leak report generated: {}", path.display());
                    })
                }
                Ok(_) => Ok(()),
                Err(error) => Err(format!(
                    "failed to read the resource tracking trigger: {error}"
                )),
            };
            control.request_quit();
            result.map_err(MonitorError::Restrack)
        })
        .map_err(MonitorError::Spawn)
}

fn write_report(
    request: &DumpRequest,
    config: &RestrackConfig,
    allocations: &Allocations,
) -> Result<std::path::PathBuf, String> {
    let path = sidecar_path(request, "restrack").map_err(|error| error.to_string())?;
    let snapshot = allocations
        .lock()
        .unwrap_or_else(|poisoned| poisoned.into_inner())
        .clone();
    let mut grouped: HashMap<Allocation, u64> = HashMap::new();
    for allocation in snapshot.into_values() {
        *grouped.entry(allocation).or_default() += 1;
    }
    let mut grouped: Vec<_> = grouped.into_iter().collect();
    grouped.sort_by_key(|(allocation, count)| std::cmp::Reverse(allocation.size * count));

    let mut file = File::create(&path)
        .map_err(|error| format!("failed to create {}: {error}", path.display()))?;
    if grouped.is_empty() {
        file.write_all(b"No leaks detected.\n")
            .map_err(|error| format!("failed to write {}: {error}", path.display()))?;
        return Ok(path);
    }

    let mut total = 0_u64;
    for (allocation, count) in grouped {
        let frame_lines: Vec<_> = allocation
            .stack
            .iter()
            .map(|address| format!("\t[0x{address:x}]"))
            .collect();
        if config.exclude_filter.as_ref().is_some_and(|filter| {
            frame_lines
                .iter()
                .any(|frame| wildcard_matches(frame, &filter.to_string_lossy()))
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
        .map_err(|error| format!("failed to write {}: {error}", path.display()))?;
        for frame in frame_lines {
            writeln!(file, "{frame}")
                .map_err(|error| format!("failed to write {}: {error}", path.display()))?;
        }
        writeln!(file).map_err(|error| format!("failed to write {}: {error}", path.display()))?;
    }
    writeln!(file, "\nTotal leaked: 0x{total:x}")
        .map_err(|error| format!("failed to write {}: {error}", path.display()))?;
    Ok(path)
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
        let mut event = vec![0_u8; EVENT_HEADER_SIZE + size_of::<u64>()];
        event[0..8].copy_from_slice(&42_u64.to_ne_bytes());
        event[16..20].copy_from_slice(&RESTRACK_ALLOC.to_ne_bytes());
        event[24..32].copy_from_slice(&64_u64.to_ne_bytes());
        event[32..40].copy_from_slice(&1_i64.to_ne_bytes());
        event[40..48].copy_from_slice(&99_u64.to_ne_bytes());
        handle_event(&event, &allocations);

        let allocation = allocations.lock().unwrap().get(&42).cloned().unwrap();
        assert_eq!(allocation.size, 64);
        assert_eq!(allocation.stack, vec![99]);

        event[16..20].copy_from_slice(&RESTRACK_FREE.to_ne_bytes());
        handle_event(&event, &allocations);
        assert!(allocations.lock().unwrap().is_empty());
    }

    #[test]
    fn wildcard_matching_is_case_insensitive() {
        assert!(wildcard_matches("[0x123] malloc+0x4", "*MALLOC*"));
        assert!(!wildcard_matches("[0x123] calloc+0x4", "*malloc*"));
    }
}
