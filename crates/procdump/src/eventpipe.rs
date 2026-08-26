use crate::config::{Config, PerfCounterTrigger};
use crate::dump::DumpKind;
use crate::monitor::{DumpCoordinator, MonitorError};
use crate::process::ProcessIdentity;
use crate::sync::{MonitorControl, WaitOutcome};
use std::collections::HashMap;
use std::io::{self, Read, Write};
use std::os::unix::net::UnixStream;
use std::sync::Arc;
use std::thread::{self, JoinHandle};
use std::time::{Duration, SystemTime, UNIX_EPOCH};

const IPC_HEADER_SIZE: usize = 20;
const MAX_BLOCK_SIZE: usize = 64 * 1024 * 1024;
const METRICS_PROVIDER: &str = "System.Diagnostics.Metrics";
const BEGIN_OBJECT: u8 = 5;
const END_OBJECT: u8 = 6;
const NULL_REFERENCE: u8 = 1;

pub(crate) fn spawn_counter_monitor(
    config: &Config,
    control: Arc<MonitorControl>,
    coordinator: Arc<DumpCoordinator>,
    identity: ProcessIdentity,
) -> Result<JoinHandle<Result<(), MonitorError>>, MonitorError> {
    let triggers = config.perf_counters.clone();
    let interval = Duration::from_millis(config.polling_interval_ms.max(1_000));
    let snooze = Duration::from_secs(config.threshold_seconds);
    thread::Builder::new()
        .name("eventpipe counter monitor".into())
        .spawn(move || {
            if control.wait_for_start() == WaitOutcome::Quit {
                return Ok(());
            }
            run_counter_monitor(
                &control,
                &coordinator,
                identity,
                &triggers,
                interval,
                snooze,
            )
            .map_err(|error| MonitorError::EventPipe(error.to_string()))
        })
        .map_err(MonitorError::Spawn)
}

fn run_counter_monitor(
    control: &MonitorControl,
    coordinator: &DumpCoordinator,
    identity: ProcessIdentity,
    triggers: &[PerfCounterTrigger],
    interval: Duration,
    snooze: Duration,
) -> Result<(), EventPipeError> {
    let socket = crate::internal::find_diagnostics_socket(identity.pid.get())
        .map_err(|error| EventPipeError::Diagnostics(error.to_string()))?
        .ok_or(EventPipeError::NotManaged)?;
    let providers = unique_providers(triggers);
    let session_key = format!(
        "{}-{}",
        std::process::id(),
        SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .unwrap_or_default()
            .as_nanos()
    );
    let command =
        build_collect_command(&providers, interval.as_secs().max(1) as u32, &session_key)?;
    let mut stream = UnixStream::connect(&socket).map_err(EventPipeError::Connect)?;
    stream.write_all(&command).map_err(EventPipeError::Send)?;
    let mut response = [0_u8; IPC_HEADER_SIZE];
    stream
        .read_exact(&mut response)
        .map_err(EventPipeError::Read)?;
    if response[16] != 0xff || response[17] != 0x00 {
        return Err(EventPipeError::RuntimeResponse {
            command_set: response[16],
            command_id: response[17],
        });
    }
    let response_size = u16::from_le_bytes([response[14], response[15]]) as usize;
    if response_size < IPC_HEADER_SIZE {
        return Err(EventPipeError::InvalidResponseSize(response_size));
    }
    let mut response_payload = vec![0_u8; response_size - IPC_HEADER_SIZE];
    stream
        .read_exact(&mut response_payload)
        .map_err(EventPipeError::Read)?;

    let mut magic = [0_u8; 8];
    stream
        .read_exact(&mut magic)
        .map_err(EventPipeError::Read)?;
    if &magic != b"Nettrace" {
        return Err(EventPipeError::InvalidMagic);
    }
    let serialization = read_serialized_string(&mut stream)?;
    if serialization != "!FastSerialization.1" {
        return Err(EventPipeError::InvalidSerialization(serialization));
    }
    let mut stream_position = 8 + 4 + serialization.len();
    let mut parser = ParserState {
        metadata: HashMap::new(),
        metrics_session_id: session_key,
    };

    loop {
        let outer = match read_u8(&mut stream) {
            Ok(value) => value,
            Err(EventPipeError::Read(error)) if error.kind() == io::ErrorKind::UnexpectedEof => {
                return Ok(());
            }
            Err(error) => return Err(error),
        };
        stream_position += 1;
        if outer == NULL_REFERENCE {
            return Ok(());
        }
        if outer != BEGIN_OBJECT {
            return Err(EventPipeError::UnexpectedTag(outer));
        }
        let type_definition = read_u8(&mut stream)?;
        let type_reference = read_u8(&mut stream)?;
        stream_position += 2;
        if type_definition != BEGIN_OBJECT || type_reference != NULL_REFERENCE {
            return Err(EventPipeError::UnexpectedTag(type_definition));
        }
        let version = read_i32(&mut stream)?;
        let _minimum_version = read_i32(&mut stream)?;
        stream_position += 8;
        let type_name = read_serialized_string(&mut stream)?;
        stream_position += 4 + type_name.len();
        let end_type = read_u8(&mut stream)?;
        stream_position += 1;
        if end_type != END_OBJECT {
            return Err(EventPipeError::UnexpectedTag(end_type));
        }

        if type_name == "Trace" {
            let trace_size = if version >= 3 { 48 } else { 32 };
            skip_exact(&mut stream, trace_size)?;
            stream_position += trace_size;
        } else {
            let block_size = read_i32(&mut stream)?;
            stream_position += 4;
            if block_size <= 0 || block_size as usize > MAX_BLOCK_SIZE {
                return Err(EventPipeError::InvalidBlockSize(block_size));
            }
            let padding = (4 - (stream_position & 3)) & 3;
            skip_exact(&mut stream, padding)?;
            stream_position += padding;
            let mut block = vec![0_u8; block_size as usize];
            stream
                .read_exact(&mut block)
                .map_err(EventPipeError::Read)?;
            stream_position += block.len();
            let values = match type_name.as_str() {
                "MetadataBlock" => {
                    parse_block(&block, |metadata_id, payload| {
                        parser.register_metadata(metadata_id, payload)
                    })?;
                    Vec::new()
                }
                "EventBlock" => {
                    let mut values = Vec::new();
                    parse_block(&block, |metadata_id, payload| {
                        if let Some(value) = parser.parse_event(metadata_id, payload) {
                            values.push(value);
                        }
                    })?;
                    values
                }
                _ => Vec::new(),
            };

            for value in values {
                if evaluate_counter(
                    control,
                    coordinator,
                    identity.pid.get(),
                    triggers,
                    &value,
                    snooze,
                )? {
                    return Ok(());
                }
            }
        }
        let end = read_u8(&mut stream)?;
        stream_position += 1;
        if end != END_OBJECT {
            return Err(EventPipeError::UnexpectedTag(end));
        }
    }
}

fn evaluate_counter(
    control: &MonitorControl,
    coordinator: &DumpCoordinator,
    pid: i32,
    triggers: &[PerfCounterTrigger],
    value: &CounterValue,
    snooze: Duration,
) -> Result<bool, EventPipeError> {
    for trigger in triggers {
        if trigger.provider.eq_ignore_ascii_case(&value.provider)
            && trigger.counter.eq_ignore_ascii_case(&value.counter)
        {
            let comparison = value.value_for(trigger.percentile.unwrap_or(0.5));
            if !comparison.is_finite() {
                continue;
            }
            let triggered = if trigger.below {
                comparison < trigger.threshold
            } else {
                comparison >= trigger.threshold
            };
            if triggered {
                println!(
                    "Trigger: {}:{} value:{comparison:.4} threshold:{:.4} on process ID: {}",
                    value.provider, value.counter, trigger.threshold, pid
                );
                coordinator
                    .write(DumpKind::PerformanceCounter)
                    .map_err(|error| EventPipeError::Dump(error.to_string()))?;
                if coordinator.limit_reached() {
                    return Ok(true);
                }
                if control.wait(snooze) != WaitOutcome::TimedOut {
                    return Ok(true);
                }
            }
        }
    }
    Ok(control.is_quit_requested())
}

fn unique_providers(triggers: &[PerfCounterTrigger]) -> Vec<String> {
    let mut providers = Vec::new();
    for trigger in triggers {
        if !providers
            .iter()
            .any(|provider: &String| provider.eq_ignore_ascii_case(&trigger.provider))
        {
            providers.push(trigger.provider.clone());
        }
    }
    providers
}

fn build_collect_command(
    providers: &[String],
    interval_seconds: u32,
    metrics_session_id: &str,
) -> Result<Vec<u8>, EventPipeError> {
    let event_counter_filter = format!("EventCounterIntervalSec={interval_seconds}");
    let meter_names = providers.join(",");
    let metrics_filter = format!(
        "SessionId={metrics_session_id};Metrics={meter_names};RefreshInterval={interval_seconds};MaxTimeSeries=1000;MaxHistograms=20;ClientId={metrics_session_id}"
    );
    let mut configurations: Vec<(&str, u64, u32, &str)> = providers
        .iter()
        .map(|provider| (provider.as_str(), 0, 4, event_counter_filter.as_str()))
        .collect();
    configurations.push((METRICS_PROVIDER, 0x2, 4, metrics_filter.as_str()));

    let mut payload = Vec::new();
    payload.extend_from_slice(&256_u32.to_le_bytes());
    payload.extend_from_slice(&1_u32.to_le_bytes());
    payload.push(0);
    payload.extend_from_slice(&(configurations.len() as u32).to_le_bytes());
    for (provider, keywords, level, filter) in configurations {
        payload.extend_from_slice(&keywords.to_le_bytes());
        payload.extend_from_slice(&level.to_le_bytes());
        append_utf16(&mut payload, provider)?;
        append_utf16(&mut payload, filter)?;
    }
    let total_size = IPC_HEADER_SIZE + payload.len();
    let total_size = u16::try_from(total_size).map_err(|_| EventPipeError::PacketTooLarge)?;
    let mut packet = Vec::with_capacity(total_size as usize);
    packet.extend_from_slice(b"DOTNET_IPC_V1\0");
    packet.extend_from_slice(&total_size.to_le_bytes());
    packet.push(0x02);
    packet.push(0x03);
    packet.extend_from_slice(&0_u16.to_le_bytes());
    packet.extend_from_slice(&payload);
    Ok(packet)
}

fn append_utf16(buffer: &mut Vec<u8>, value: &str) -> Result<(), EventPipeError> {
    let mut value: Vec<u16> = value.encode_utf16().collect();
    value.push(0);
    let length = u32::try_from(value.len()).map_err(|_| EventPipeError::PacketTooLarge)?;
    buffer.extend_from_slice(&length.to_le_bytes());
    for character in value {
        buffer.extend_from_slice(&character.to_le_bytes());
    }
    Ok(())
}

struct ParserState {
    metadata: HashMap<u32, Metadata>,
    metrics_session_id: String,
}

impl ParserState {
    fn register_metadata(&mut self, _header_metadata_id: u32, payload: &[u8]) {
        let mut cursor = Cursor::new(payload);
        let Some(metadata_id) = cursor.read_i32().map(|value| value as u32) else {
            return;
        };
        let Some(provider) = cursor.read_utf16() else {
            return;
        };
        if cursor.read_i32().is_none() {
            return;
        }
        let Some(event) = cursor.read_utf16() else {
            return;
        };
        self.metadata
            .insert(metadata_id, Metadata { provider, event });
    }

    fn parse_event(&self, metadata_id: u32, payload: &[u8]) -> Option<CounterValue> {
        let metadata = self.metadata.get(&metadata_id)?;
        if metadata.event.eq_ignore_ascii_case("EventCounters") {
            return parse_event_counter(payload, &metadata.provider);
        }
        if metadata.provider.eq_ignore_ascii_case(METRICS_PROVIDER) {
            return parse_metrics_event(payload, &metadata.event, &self.metrics_session_id);
        }
        None
    }
}

struct Metadata {
    provider: String,
    event: String,
}

#[derive(Debug, PartialEq)]
struct CounterValue {
    provider: String,
    counter: String,
    value: f64,
    quantiles: Vec<(f64, f64)>,
}

impl CounterValue {
    fn value_for(&self, percentile: f64) -> f64 {
        self.quantiles
            .iter()
            .find(|(key, _)| (*key - percentile).abs() < 0.001)
            .map_or(self.value, |(_, value)| *value)
    }
}

fn parse_event_counter(payload: &[u8], provider: &str) -> Option<CounterValue> {
    let mut cursor = Cursor::new(payload);
    let counter = cursor.read_utf16()?;
    cursor.read_utf16()?;
    let numeric_start = cursor.position;
    if cursor.remaining() >= 40 {
        let mean = cursor.read_f64()?;
        cursor.skip(8 + 4 + 8 + 8 + 4)?;
        cursor.read_utf16()?;
        if cursor.read_utf16()?.eq_ignore_ascii_case("Mean") {
            return Some(CounterValue {
                provider: provider.into(),
                counter,
                value: mean,
                quantiles: Vec::new(),
            });
        }
    }
    cursor.position = numeric_start;
    cursor.read_utf16()?;
    let increment = cursor.read_f64()?;
    cursor.skip(4)?;
    cursor.read_utf16()?;
    cursor.read_utf16()?;
    if cursor.read_utf16()?.eq_ignore_ascii_case("Sum") {
        Some(CounterValue {
            provider: provider.into(),
            counter,
            value: increment,
            quantiles: Vec::new(),
        })
    } else {
        None
    }
}

fn parse_metrics_event(payload: &[u8], event: &str, session_id: &str) -> Option<CounterValue> {
    let mut cursor = Cursor::new(payload);
    if cursor.read_utf16()? != session_id {
        return None;
    }
    let provider = cursor.read_utf16()?;
    cursor.read_utf16()?;
    let counter = cursor.read_utf16()?;
    cursor.read_utf16()?;
    cursor.read_utf16()?;
    let value = match event {
        value if value.eq_ignore_ascii_case("GaugeValuePublished") => {
            cursor.read_utf16()?.parse().ok()?
        }
        value if value.eq_ignore_ascii_case("CounterRateValuePublished") => {
            cursor.read_utf16()?.parse().ok()?
        }
        value if value.eq_ignore_ascii_case("UpDownCounterRateValuePublished") => {
            let rate = cursor.read_utf16()?;
            cursor
                .read_utf16()
                .filter(|value| !value.is_empty())
                .unwrap_or(rate)
                .parse()
                .ok()?
        }
        value if value.eq_ignore_ascii_case("HistogramValuePublished") => {
            let quantiles = parse_quantiles(&cursor.read_utf16()?);
            let default = quantiles
                .iter()
                .find(|(key, _)| (*key - 0.5).abs() < 0.001)
                .or_else(|| quantiles.first())?
                .1;
            return Some(CounterValue {
                provider,
                counter,
                value: default,
                quantiles,
            });
        }
        _ => return None,
    };
    Some(CounterValue {
        provider,
        counter,
        value,
        quantiles: Vec::new(),
    })
}

fn parse_quantiles(value: &str) -> Vec<(f64, f64)> {
    value
        .split(';')
        .filter_map(|entry| {
            let (key, value) = entry.split_once('=')?;
            Some((key.parse().ok()?, value.parse().ok()?))
        })
        .filter(|(key, value): &(f64, f64)| key.is_finite() && value.is_finite())
        .collect()
}

fn parse_block<F>(block: &[u8], mut callback: F) -> Result<(), EventPipeError>
where
    F: FnMut(u32, &[u8]),
{
    let mut cursor = Cursor::new(block);
    let header_size = cursor.read_u16().ok_or(EventPipeError::TruncatedBlock)? as usize;
    let flags = cursor.read_u16().ok_or(EventPipeError::TruncatedBlock)?;
    if header_size < 4 || header_size > block.len() {
        return Err(EventPipeError::TruncatedBlock);
    }
    cursor.position = header_size;
    let compressed = flags & 1 != 0;
    let mut last_metadata = 0_u32;
    let mut last_payload = 0_usize;
    while cursor.position < block.len() {
        let (metadata_id, payload_size) = if compressed {
            let event_flags = cursor.read_u8().ok_or(EventPipeError::TruncatedBlock)?;
            let metadata = if event_flags & 0x01 != 0 {
                let value = cursor
                    .read_var_u32()
                    .ok_or(EventPipeError::TruncatedBlock)?;
                last_metadata = value;
                value
            } else {
                last_metadata
            };
            if event_flags & 0x02 != 0 {
                cursor
                    .read_var_u32()
                    .ok_or(EventPipeError::TruncatedBlock)?;
                cursor
                    .read_var_u64()
                    .ok_or(EventPipeError::TruncatedBlock)?;
                cursor
                    .read_var_u32()
                    .ok_or(EventPipeError::TruncatedBlock)?;
            }
            if event_flags & 0x04 != 0 {
                cursor
                    .read_var_u64()
                    .ok_or(EventPipeError::TruncatedBlock)?;
            }
            if event_flags & 0x08 != 0 {
                cursor
                    .read_var_u32()
                    .ok_or(EventPipeError::TruncatedBlock)?;
            }
            cursor
                .read_var_u64()
                .ok_or(EventPipeError::TruncatedBlock)?;
            if event_flags & 0x10 != 0 {
                cursor.skip(16).ok_or(EventPipeError::TruncatedBlock)?;
            }
            if event_flags & 0x20 != 0 {
                cursor.skip(16).ok_or(EventPipeError::TruncatedBlock)?;
            }
            let payload = if event_flags & 0x80 != 0 {
                let value = cursor
                    .read_var_u32()
                    .ok_or(EventPipeError::TruncatedBlock)? as usize;
                last_payload = value;
                value
            } else {
                last_payload
            };
            (metadata, payload)
        } else {
            if cursor.remaining() < 80 {
                break;
            }
            cursor.skip(4).ok_or(EventPipeError::TruncatedBlock)?;
            let metadata =
                cursor.read_i32().ok_or(EventPipeError::TruncatedBlock)? as u32 & 0x7fff_ffff;
            cursor.skip(68).ok_or(EventPipeError::TruncatedBlock)?;
            let payload = cursor.read_i32().ok_or(EventPipeError::TruncatedBlock)?;
            if payload < 0 {
                return Err(EventPipeError::TruncatedBlock);
            }
            (metadata, payload as usize)
        };
        let payload = cursor
            .read_slice(payload_size)
            .ok_or(EventPipeError::TruncatedBlock)?;
        callback(metadata_id, payload);
        if !compressed {
            let padding = (4 - (payload_size & 3)) & 3;
            cursor.skip(padding).ok_or(EventPipeError::TruncatedBlock)?;
        }
    }
    Ok(())
}

struct Cursor<'a> {
    data: &'a [u8],
    position: usize,
}

impl<'a> Cursor<'a> {
    fn new(data: &'a [u8]) -> Self {
        Self { data, position: 0 }
    }
    fn remaining(&self) -> usize {
        self.data.len().saturating_sub(self.position)
    }
    fn read_slice(&mut self, length: usize) -> Option<&'a [u8]> {
        let end = self.position.checked_add(length)?;
        let value = self.data.get(self.position..end)?;
        self.position = end;
        Some(value)
    }
    fn skip(&mut self, length: usize) -> Option<()> {
        self.read_slice(length).map(|_| ())
    }
    fn read_u8(&mut self) -> Option<u8> {
        Some(*self.read_slice(1)?.first()?)
    }
    fn read_u16(&mut self) -> Option<u16> {
        Some(u16::from_le_bytes(self.read_slice(2)?.try_into().ok()?))
    }
    fn read_i32(&mut self) -> Option<i32> {
        Some(i32::from_le_bytes(self.read_slice(4)?.try_into().ok()?))
    }
    fn read_f64(&mut self) -> Option<f64> {
        Some(f64::from_le_bytes(self.read_slice(8)?.try_into().ok()?))
    }
    fn read_utf16(&mut self) -> Option<String> {
        let mut value = Vec::new();
        loop {
            let character = self.read_u16()?;
            if character == 0 {
                return String::from_utf16(&value).ok();
            }
            value.push(character);
        }
    }
    fn read_var_u32(&mut self) -> Option<u32> {
        let mut result = 0_u32;
        for shift in (0..35).step_by(7) {
            let byte = self.read_u8()?;
            result |= u32::from(byte & 0x7f) << shift;
            if byte & 0x80 == 0 {
                return Some(result);
            }
        }
        None
    }
    fn read_var_u64(&mut self) -> Option<u64> {
        let mut result = 0_u64;
        for shift in (0..70).step_by(7) {
            let byte = self.read_u8()?;
            result |= u64::from(byte & 0x7f) << shift;
            if byte & 0x80 == 0 {
                return Some(result);
            }
        }
        None
    }
}

fn read_u8(reader: &mut impl Read) -> Result<u8, EventPipeError> {
    let mut value = [0_u8; 1];
    reader
        .read_exact(&mut value)
        .map_err(EventPipeError::Read)?;
    Ok(value[0])
}

fn read_i32(reader: &mut impl Read) -> Result<i32, EventPipeError> {
    let mut value = [0_u8; 4];
    reader
        .read_exact(&mut value)
        .map_err(EventPipeError::Read)?;
    Ok(i32::from_le_bytes(value))
}

fn read_serialized_string(reader: &mut impl Read) -> Result<String, EventPipeError> {
    let length = read_i32(reader)?;
    if length <= 0 || length as usize > 1_048_576 {
        return Err(EventPipeError::InvalidStringLength(length));
    }
    let mut value = vec![0_u8; length as usize];
    reader
        .read_exact(&mut value)
        .map_err(EventPipeError::Read)?;
    String::from_utf8(value).map_err(|_| EventPipeError::InvalidUtf8)
}

fn skip_exact(reader: &mut impl Read, mut length: usize) -> Result<(), EventPipeError> {
    let mut buffer = [0_u8; 4096];
    while length > 0 {
        let count = length.min(buffer.len());
        reader
            .read_exact(&mut buffer[..count])
            .map_err(EventPipeError::Read)?;
        length -= count;
    }
    Ok(())
}

#[derive(Debug)]
enum EventPipeError {
    Diagnostics(String),
    NotManaged,
    Connect(io::Error),
    Send(io::Error),
    Read(io::Error),
    RuntimeResponse { command_set: u8, command_id: u8 },
    InvalidResponseSize(usize),
    InvalidMagic,
    InvalidSerialization(String),
    UnexpectedTag(u8),
    InvalidBlockSize(i32),
    TruncatedBlock,
    InvalidStringLength(i32),
    InvalidUtf8,
    PacketTooLarge,
    Dump(String),
}

impl std::fmt::Display for EventPipeError {
    fn fmt(&self, formatter: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            Self::Diagnostics(error) => formatter.write_str(error),
            Self::NotManaged => write!(formatter, "performance counters require a .NET process"),
            Self::Connect(error) => write!(formatter, "failed to connect to EventPipe: {error}"),
            Self::Send(error) => write!(formatter, "failed to start EventPipe session: {error}"),
            Self::Read(error) => write!(formatter, "failed to read EventPipe stream: {error}"),
            Self::RuntimeResponse {
                command_set,
                command_id,
            } => write!(
                formatter,
                "EventPipe runtime rejected session: set=0x{command_set:02x}, id=0x{command_id:02x}"
            ),
            Self::InvalidResponseSize(size) => {
                write!(formatter, "invalid EventPipe response size: {size}")
            }
            Self::InvalidMagic => write!(formatter, "invalid nettrace magic"),
            Self::InvalidSerialization(value) => {
                write!(formatter, "unsupported serialization header: {value}")
            }
            Self::UnexpectedTag(tag) => {
                write!(formatter, "unexpected FastSerialization tag: {tag}")
            }
            Self::InvalidBlockSize(size) => {
                write!(formatter, "invalid nettrace block size: {size}")
            }
            Self::TruncatedBlock => write!(formatter, "truncated nettrace block"),
            Self::InvalidStringLength(length) => {
                write!(formatter, "invalid serialized string length: {length}")
            }
            Self::InvalidUtf8 => write!(formatter, "invalid serialized UTF-8 string"),
            Self::PacketTooLarge => write!(formatter, "EventPipe request is too large"),
            Self::Dump(error) => formatter.write_str(error),
        }
    }
}

impl std::error::Error for EventPipeError {}

#[cfg(test)]
mod tests {
    use super::*;

    fn utf16(value: &str) -> Vec<u8> {
        let mut result = Vec::new();
        for character in value.encode_utf16().chain(std::iter::once(0)) {
            result.extend_from_slice(&character.to_le_bytes());
        }
        result
    }

    #[test]
    fn parses_gauge_event_counter() {
        let mut payload = utf16("test-gauge");
        payload.extend(utf16("Test Gauge"));
        payload.extend_from_slice(&100_f64.to_le_bytes());
        payload.extend_from_slice(&0_f64.to_le_bytes());
        payload.extend_from_slice(&1_i32.to_le_bytes());
        payload.extend_from_slice(&100_f64.to_le_bytes());
        payload.extend_from_slice(&100_f64.to_le_bytes());
        payload.extend_from_slice(&1_f32.to_le_bytes());
        payload.extend(utf16("Interval=1000"));
        payload.extend(utf16("Mean"));

        let value = parse_event_counter(&payload, "TestWebApi.PerfCounter").unwrap();
        assert_eq!(value.provider, "TestWebApi.PerfCounter");
        assert_eq!(value.counter, "test-gauge");
        assert_eq!(value.value, 100.0);
    }

    #[test]
    fn parses_metrics_histogram_percentile() {
        let mut payload = utf16("session");
        payload.extend(utf16("Microsoft.AspNetCore.Hosting"));
        payload.extend(utf16("1.0"));
        payload.extend(utf16("http.server.request.duration"));
        payload.extend(utf16("s"));
        payload.extend(utf16(""));
        payload.extend(utf16("0.5=1.0;0.95=2.0"));

        let value = parse_metrics_event(&payload, "HistogramValuePublished", "session").unwrap();
        assert_eq!(value.value_for(0.95), 2.0);
    }

    #[test]
    fn collect_request_subscribes_to_eventcounters_and_metrics() {
        let packet = build_collect_command(&["Test".into()], 1, "session").unwrap();
        assert_eq!(&packet[..14], b"DOTNET_IPC_V1\0");
        assert_eq!(packet[16], 0x02);
        assert_eq!(packet[17], 0x03);
        assert!(
            !packet
                .windows(METRICS_PROVIDER.len())
                .any(|window| window == METRICS_PROVIDER.as_bytes())
        );
    }
}
