use std::ffi::{OsStr, OsString};
use std::fmt;
use std::path::{Path, PathBuf};

pub const DEFAULT_DUMP_COUNT: u32 = 1;
pub const DEFAULT_POLLING_INTERVAL_MS: u64 = 1_000;
pub const DEFAULT_SAMPLE_RATE: u32 = 1;
pub const DEFAULT_THRESHOLD_SECONDS: u64 = 10;
pub const MAX_DUMP_COUNT: u32 = 100;
pub const MAX_PERF_COUNTER_TRIGGERS: usize = 5;

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum Platform {
    Linux,
    MacOs,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct Capabilities {
    pub process_groups: bool,
    pub signal_triggers: bool,
    pub dotnet_triggers: bool,
    pub resource_tracking: bool,
    pub custom_dump_mask: bool,
    pub native_core_writer: bool,
}

impl Capabilities {
    pub const fn for_platform(platform: Platform) -> Self {
        match platform {
            Platform::Linux => Self {
                process_groups: true,
                signal_triggers: true,
                dotnet_triggers: cfg!(feature = "dotnet-triggers"),
                resource_tracking: cfg!(feature = "restrack"),
                custom_dump_mask: true,
                native_core_writer: true,
            },
            Platform::MacOs => Self {
                process_groups: false,
                signal_triggers: false,
                dotnet_triggers: false,
                resource_tracking: false,
                custom_dump_mask: false,
                native_core_writer: false,
            },
        }
    }
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub enum TargetSpec {
    Pid(i32),
    Name(OsString),
    ProcessGroup(i32),
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct OutputSpec {
    pub directory: PathBuf,
    pub file_name: Option<OsString>,
}

impl Default for OutputSpec {
    fn default() -> Self {
        Self {
            directory: PathBuf::from("."),
            file_name: None,
        }
    }
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum DiagnosticsTarget {
    None,
    Stdout,
    Syslog,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum Threshold<T> {
    AtLeast(T),
    Below(T),
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum GcHeap {
    Cumulative,
    Generation(u8),
    LargeObject,
    PinnedObject,
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub enum DotNetTrigger {
    Exception,
    GcMemory {
        heap: GcHeap,
        thresholds_mb: Vec<u64>,
    },
    GcGeneration(u8),
}

#[derive(Clone, Debug, PartialEq)]
pub struct PerfCounterTrigger {
    pub provider: String,
    pub counter: String,
    pub threshold: f64,
    pub below: bool,
    pub percentile: Option<f64>,
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct RestrackConfig {
    pub generate_dump: bool,
    pub sample_rate: u32,
    pub exclude_filter: Option<OsString>,
}

#[derive(Clone, Debug, PartialEq)]
pub struct Config {
    pub(crate) target: TargetSpec,
    pub(crate) output: OutputSpec,
    pub(crate) cpu: Option<Threshold<u32>>,
    pub(crate) memory_mb: Option<Threshold<Vec<u64>>>,
    pub(crate) thread_count: Option<u32>,
    pub(crate) file_descriptor_count: Option<u32>,
    pub(crate) polling_interval_ms: u64,
    pub(crate) threshold_seconds: u64,
    pub(crate) dump_count: u32,
    pub(crate) wait_for_process: bool,
    pub(crate) overwrite: bool,
    pub(crate) diagnostics: DiagnosticsTarget,
    pub(crate) use_gcore: bool,
    pub(crate) timer_trigger: bool,
    pub(crate) signals: Vec<i32>,
    pub(crate) dotnet_trigger: Option<DotNetTrigger>,
    pub(crate) exception_filter: Option<OsString>,
    pub(crate) perf_counters: Vec<PerfCounterTrigger>,
    pub(crate) restrack: Option<RestrackConfig>,
    pub(crate) core_dump_mask: Option<u32>,
}

impl Config {
    pub fn builder(target: TargetSpec) -> ConfigBuilder {
        ConfigBuilder {
            config: Self {
                target,
                output: OutputSpec::default(),
                cpu: None,
                memory_mb: None,
                thread_count: None,
                file_descriptor_count: None,
                polling_interval_ms: DEFAULT_POLLING_INTERVAL_MS,
                threshold_seconds: DEFAULT_THRESHOLD_SECONDS,
                dump_count: DEFAULT_DUMP_COUNT,
                wait_for_process: false,
                overwrite: false,
                diagnostics: DiagnosticsTarget::None,
                use_gcore: false,
                timer_trigger: true,
                signals: Vec::new(),
                dotnet_trigger: None,
                exception_filter: None,
                perf_counters: Vec::new(),
                restrack: None,
                core_dump_mask: None,
            },
        }
    }

    pub fn target(&self) -> &TargetSpec {
        &self.target
    }

    pub fn output(&self) -> &OutputSpec {
        &self.output
    }

    pub fn diagnostics(&self) -> DiagnosticsTarget {
        self.diagnostics
    }

    pub fn dump_count(&self) -> u32 {
        self.dump_count
    }

    pub fn requires_gcore_preflight(&self) -> bool {
        self.dotnet_trigger.is_none()
            && self.perf_counters.is_empty()
            && !self
                .restrack
                .as_ref()
                .is_some_and(|restrack| !restrack.generate_dump)
    }

    fn validate(&self) -> Result<(), ParseError> {
        match self.target {
            TargetSpec::Pid(pid) if pid <= 0 => {
                return Err(ParseError::InvalidCombination(
                    "process ID must be greater than zero".into(),
                ));
            }
            TargetSpec::ProcessGroup(group) if group <= 0 => {
                return Err(ParseError::InvalidCombination(
                    "process group must be greater than zero".into(),
                ));
            }
            _ => {}
        }
        if self.dump_count == 0 || self.dump_count > MAX_DUMP_COUNT {
            return Err(ParseError::InvalidCombination(format!(
                "dump count must be between 1 and {MAX_DUMP_COUNT}"
            )));
        }
        if self.polling_interval_ms == 0 {
            return Err(ParseError::InvalidCombination(
                "polling interval must be greater than zero".into(),
            ));
        }
        if self.signals.iter().any(|signal| {
            !(1..=64).contains(signal) || matches!(*signal, libc::SIGKILL | libc::SIGSTOP)
        }) {
            return Err(ParseError::InvalidCombination(
                "signals must be catchable values between 1 and 64".into(),
            ));
        }
        if self.restrack.as_ref().is_some_and(|restrack| {
            restrack.sample_rate == 0 || restrack.sample_rate > i32::MAX as u32
        }) {
            return Err(ParseError::InvalidCombination(
                "resource tracking sample rate is outside the eBPF range".into(),
            ));
        }
        if self.wait_for_process && !matches!(self.target, TargetSpec::Name(_)) {
            return Err(ParseError::InvalidCombination(
                "the wait option requires a process name".into(),
            ));
        }
        if self.perf_counters.len() > MAX_PERF_COUNTER_TRIGGERS {
            return Err(ParseError::InvalidCombination(format!(
                "at most {MAX_PERF_COUNTER_TRIGGERS} performance counter triggers are allowed"
            )));
        }
        if (self.dotnet_trigger.is_some() || !self.perf_counters.is_empty())
            && !cfg!(feature = "dotnet-triggers")
        {
            return Err(ParseError::InvalidCombination(
                ".NET triggers require the dotnet-triggers Cargo feature".into(),
            ));
        }
        if self.restrack.is_some() && !cfg!(feature = "restrack") {
            return Err(ParseError::InvalidCombination(
                "resource tracking requires the restrack Cargo feature".into(),
            ));
        }
        if self.exception_filter.is_some() && self.dotnet_trigger != Some(DotNetTrigger::Exception)
        {
            return Err(ParseError::InvalidCombination(
                "exception filters require the exception trigger".into(),
            ));
        }
        let exclusive_trigger =
            !self.signals.is_empty() || self.dotnet_trigger == Some(DotNetTrigger::Exception);
        if exclusive_trigger
            && (self.cpu.is_some()
                || self.memory_mb.is_some()
                || self.thread_count.is_some()
                || self.file_descriptor_count.is_some()
                || !self.perf_counters.is_empty())
        {
            return Err(ParseError::InvalidCombination(
                "signal and exception triggers must be the only trigger specified".into(),
            ));
        }
        Ok(())
    }
}

pub struct ConfigBuilder {
    config: Config,
}

impl ConfigBuilder {
    pub fn output(mut self, output: OutputSpec) -> Self {
        self.config.output = output;
        self
    }
    pub fn cpu(mut self, threshold: Threshold<u32>) -> Self {
        self.config.cpu = Some(threshold);
        self.config.timer_trigger = false;
        self
    }
    pub fn memory(mut self, threshold: Threshold<Vec<u64>>) -> Self {
        self.config.memory_mb = Some(threshold);
        self.config.timer_trigger = false;
        self
    }
    pub fn thread_count(mut self, threshold: u32) -> Self {
        self.config.thread_count = Some(threshold);
        self.config.timer_trigger = false;
        self
    }
    pub fn file_descriptor_count(mut self, threshold: u32) -> Self {
        self.config.file_descriptor_count = Some(threshold);
        self.config.timer_trigger = false;
        self
    }
    pub fn polling_interval_ms(mut self, interval: u64) -> Self {
        self.config.polling_interval_ms = interval;
        self
    }
    pub fn threshold_seconds(mut self, seconds: u64) -> Self {
        self.config.threshold_seconds = seconds;
        self
    }
    pub fn dump_count(mut self, count: u32) -> Self {
        self.config.dump_count = count;
        self
    }
    pub fn wait_for_process(mut self, wait: bool) -> Self {
        self.config.wait_for_process = wait;
        self
    }
    pub fn overwrite(mut self, overwrite: bool) -> Self {
        self.config.overwrite = overwrite;
        self
    }
    pub fn diagnostics(mut self, diagnostics: DiagnosticsTarget) -> Self {
        self.config.diagnostics = diagnostics;
        self
    }
    pub fn use_gcore(mut self, use_gcore: bool) -> Self {
        self.config.use_gcore = use_gcore;
        self
    }
    pub fn timer(mut self, enabled: bool) -> Self {
        self.config.timer_trigger = enabled;
        self
    }
    pub fn signals(mut self, signals: Vec<i32>) -> Self {
        self.config.signals = signals;
        self.config.timer_trigger = false;
        self
    }
    pub fn dotnet_trigger(mut self, trigger: DotNetTrigger) -> Self {
        self.config.dotnet_trigger = Some(trigger);
        self.config.timer_trigger = false;
        self
    }
    pub fn exception_filter(mut self, filter: impl Into<OsString>) -> Self {
        self.config.exception_filter = Some(filter.into());
        self
    }
    pub fn perf_counter(mut self, trigger: PerfCounterTrigger) -> Self {
        self.config.perf_counters.push(trigger);
        self.config.timer_trigger = false;
        self
    }
    pub fn restrack(mut self, config: RestrackConfig) -> Self {
        self.config.restrack = Some(config);
        self
    }
    pub fn core_dump_mask(mut self, mask: u32) -> Self {
        self.config.core_dump_mask = Some(mask);
        self
    }

    pub fn build(self) -> Result<Config, ParseError> {
        self.config.validate()?;
        Ok(self.config)
    }
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub enum ParseError {
    HelpRequested,
    MissingTarget,
    MissingValue(String),
    DuplicateOption(String),
    InvalidValue { option: String, value: OsString },
    UnsupportedOption(String),
    TooManyPositionals,
    InvalidOutputDirectory(PathBuf),
    InvalidCombination(String),
}

impl fmt::Display for ParseError {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::HelpRequested => write!(formatter, "help requested"),
            Self::MissingTarget => write!(formatter, "a process name or PID is required"),
            Self::MissingValue(option) => write!(formatter, "{option} requires a value"),
            Self::DuplicateOption(option) => {
                write!(formatter, "{option} may only be specified once")
            }
            Self::InvalidValue { option, value } => {
                write!(
                    formatter,
                    "invalid value for {option}: {}",
                    value.to_string_lossy()
                )
            }
            Self::UnsupportedOption(option) => {
                write!(formatter, "{option} is not supported on this platform")
            }
            Self::TooManyPositionals => write!(formatter, "too many positional arguments"),
            Self::InvalidOutputDirectory(path) => {
                write!(
                    formatter,
                    "invalid core dump output directory: {}",
                    path.display()
                )
            }
            Self::InvalidCombination(message) => formatter.write_str(message),
        }
    }
}

impl std::error::Error for ParseError {}

pub fn parse<I, S>(arguments: I, platform: Platform) -> Result<Config, ParseError>
where
    I: IntoIterator<Item = S>,
    S: Into<OsString>,
{
    parse_with_directory_check(arguments, platform, |path| path.is_dir())
}

fn parse_with_directory_check<I, S, F>(
    arguments: I,
    platform: Platform,
    is_directory: F,
) -> Result<Config, ParseError>
where
    I: IntoIterator<Item = S>,
    S: Into<OsString>,
    F: Fn(&Path) -> bool,
{
    let capabilities = Capabilities::for_platform(platform);
    let arguments: Vec<OsString> = arguments.into_iter().map(Into::into).collect();
    let mut index = 0;
    let mut cpu = None;
    let mut memory_mb = None;
    let mut thread_count = None;
    let mut file_descriptor_count = None;
    let mut polling_interval_ms = None;
    let mut threshold_seconds = None;
    let mut dump_count = None;
    let mut wait_for_process = false;
    let mut process_group = false;
    let mut overwrite = false;
    let mut diagnostics = DiagnosticsTarget::None;
    let mut use_gcore = platform == Platform::MacOs;
    let mut signals = None;
    let mut dotnet_trigger = None;
    let mut exception_filter = None;
    let mut perf_counters = Vec::new();
    let mut restrack_generate_dump = None;
    let mut sample_rate = None;
    let mut exclude_filter = None;
    let mut core_dump_mask = None;
    let mut target = None;
    let mut output = None;

    while index < arguments.len() {
        let argument = &arguments[index];
        let normalized = normalize_option(argument);

        match normalized.as_deref() {
            Some("?") => return Err(ParseError::HelpRequested),
            Some("c") | Some("cl") => {
                ensure_absent(&cpu, normalized.as_deref().unwrap())?;
                let value = next_value(&arguments, &mut index, normalized.as_deref().unwrap())?;
                let value = parse_number::<u32>(normalized.as_deref().unwrap(), value)?;
                cpu = Some(if normalized.as_deref() == Some("cl") {
                    Threshold::Below(value)
                } else {
                    Threshold::AtLeast(value)
                });
            }
            Some("m") | Some("ml") => {
                ensure_absent(&memory_mb, normalized.as_deref().unwrap())?;
                if matches!(dotnet_trigger, Some(DotNetTrigger::GcMemory { .. })) {
                    return Err(ParseError::DuplicateOption(
                        normalized.as_deref().unwrap().into(),
                    ));
                }
                let value = next_value(&arguments, &mut index, normalized.as_deref().unwrap())?;
                let values = parse_list::<u64>(normalized.as_deref().unwrap(), value)?;
                memory_mb = Some(if normalized.as_deref() == Some("ml") {
                    Threshold::Below(values)
                } else {
                    Threshold::AtLeast(values)
                });
            }
            Some("gcm") => {
                require_capability(capabilities.dotnet_triggers, "gcm")?;
                ensure_absent(&dotnet_trigger, "gcm")?;
                ensure_absent(&memory_mb, "gcm")?;
                let value = next_value(&arguments, &mut index, "gcm")?;
                let (heap, thresholds_mb) = parse_gc_memory(value)?;
                dotnet_trigger = Some(DotNetTrigger::GcMemory {
                    heap,
                    thresholds_mb,
                });
            }
            Some("gcgen") => {
                require_capability(capabilities.dotnet_triggers, "gcgen")?;
                ensure_absent(&dotnet_trigger, "gcgen")?;
                let generation: u8 = parse_next_number(&arguments, &mut index, "gcgen")?;
                if generation > 2 {
                    return Err(ParseError::InvalidValue {
                        option: "gcgen".into(),
                        value: generation.to_string().into(),
                    });
                }
                if dump_count.is_some() {
                    return Err(ParseError::InvalidCombination(
                        "-n cannot be combined with -gcgen".into(),
                    ));
                }
                dump_count = Some(2);
                dotnet_trigger = Some(DotNetTrigger::GcGeneration(generation));
            }
            Some("restrack") => {
                require_capability(capabilities.resource_tracking, "restrack")?;
                if restrack_generate_dump.is_some() {
                    return Err(ParseError::DuplicateOption("restrack".into()));
                }
                let no_dump = arguments
                    .get(index + 1)
                    .is_some_and(|value| value.eq_ignore_ascii_case(OsStr::new("nodump")));
                if no_dump {
                    index += 1;
                }
                restrack_generate_dump = Some(!no_dump);
            }
            Some("sr") => {
                require_capability(capabilities.resource_tracking, "sr")?;
                ensure_absent(&sample_rate, "sr")?;
                let value: u32 = parse_next_number(&arguments, &mut index, "sr")?;
                if value > i32::MAX as u32 {
                    return Err(ParseError::InvalidValue {
                        option: "sr".into(),
                        value: value.to_string().into(),
                    });
                }
                sample_rate = Some(value.max(DEFAULT_SAMPLE_RATE));
            }
            Some("sig") => {
                require_capability(capabilities.signal_triggers, "sig")?;
                ensure_absent(&signals, "sig")?;
                let value = next_value(&arguments, &mut index, "sig")?;
                signals = Some(parse_list::<i32>("sig", value)?);
                if signals
                    .as_ref()
                    .is_some_and(|values| values.iter().any(|value| *value < 0))
                {
                    return Err(ParseError::InvalidValue {
                        option: "sig".into(),
                        value: value.clone(),
                    });
                }
            }
            Some("mc") => {
                require_capability(capabilities.custom_dump_mask, "mc")?;
                ensure_absent(&core_dump_mask, "mc")?;
                let value = next_value(&arguments, &mut index, "mc")?;
                core_dump_mask = Some(parse_hex("mc", value)?);
            }
            Some("pc") | Some("pcl") => {
                require_capability(capabilities.dotnet_triggers, normalized.as_deref().unwrap())?;
                if perf_counters.len() >= MAX_PERF_COUNTER_TRIGGERS {
                    return Err(ParseError::InvalidCombination(format!(
                        "at most {MAX_PERF_COUNTER_TRIGGERS} performance counter triggers are allowed"
                    )));
                }
                let specification =
                    next_value(&arguments, &mut index, normalized.as_deref().unwrap())?;
                let threshold = next_value(&arguments, &mut index, normalized.as_deref().unwrap())?;
                perf_counters.push(parse_perf_counter(
                    specification,
                    threshold,
                    normalized.as_deref() == Some("pcl"),
                )?);
            }
            Some("tc") => {
                ensure_absent(&thread_count, "tc")?;
                thread_count = Some(parse_next_number(&arguments, &mut index, "tc")?);
            }
            Some("fc") => {
                ensure_absent(&file_descriptor_count, "fc")?;
                file_descriptor_count = Some(parse_next_number(&arguments, &mut index, "fc")?);
            }
            Some("pf") => {
                ensure_absent(&polling_interval_ms, "pf")?;
                polling_interval_ms = Some(parse_next_number(&arguments, &mut index, "pf")?);
            }
            Some("s") => {
                ensure_absent(&threshold_seconds, "s")?;
                threshold_seconds = Some(parse_next_number(&arguments, &mut index, "s")?);
            }
            Some("n") => {
                ensure_absent(&dump_count, "n")?;
                let count: u32 = parse_next_number(&arguments, &mut index, "n")?;
                if count > MAX_DUMP_COUNT {
                    return Err(ParseError::InvalidValue {
                        option: "n".into(),
                        value: count.to_string().into(),
                    });
                }
                dump_count = Some(count);
            }
            Some("log") => {
                let value = next_value(&arguments, &mut index, "log")?;
                diagnostics = if value.eq_ignore_ascii_case(OsStr::new("stdout")) {
                    DiagnosticsTarget::Stdout
                } else if value.eq_ignore_ascii_case(OsStr::new("syslog")) {
                    DiagnosticsTarget::Syslog
                } else {
                    return Err(ParseError::InvalidValue {
                        option: "log".into(),
                        value: value.clone(),
                    });
                };
            }
            Some("e") => {
                require_capability(capabilities.dotnet_triggers, "e")?;
                ensure_absent(&dotnet_trigger, "e")?;
                dotnet_trigger = Some(DotNetTrigger::Exception);
            }
            Some("f") => {
                require_capability(capabilities.dotnet_triggers, "f")?;
                ensure_absent(&exception_filter, "f")?;
                let value = next_value(&arguments, &mut index, "f")?;
                let valid_first = value
                    .to_str()
                    .and_then(|value| value.chars().next())
                    .is_some_and(|value| value == '*' || value.is_ascii_alphabetic());
                if !valid_first {
                    return Err(ParseError::InvalidValue {
                        option: "f".into(),
                        value: value.clone(),
                    });
                }
                exception_filter = Some(value.clone());
            }
            Some("fx") => {
                require_capability(capabilities.resource_tracking, "fx")?;
                ensure_absent(&exclude_filter, "fx")?;
                exclude_filter = Some(next_value(&arguments, &mut index, "fx")?.clone());
            }
            Some("o") => overwrite = true,
            Some("w") => wait_for_process = true,
            Some("pgid") => {
                if !capabilities.process_groups {
                    return Err(ParseError::UnsupportedOption("pgid".into()));
                }
                process_group = true;
            }
            Some("usegcore") if argument.to_string_lossy().starts_with('-') => use_gcore = true,
            Some(option) => return Err(ParseError::UnsupportedOption(option.into())),
            None if target.is_none() => target = Some(argument.clone()),
            None if output.is_none() => output = Some(parse_output(argument, &is_directory)?),
            None => return Err(ParseError::TooManyPositionals),
        }

        index += 1;
    }

    let target = parse_target(target.ok_or(ParseError::MissingTarget)?, process_group)?;
    if wait_for_process && !matches!(target, TargetSpec::Name(_)) {
        return Err(ParseError::InvalidCombination(
            "the wait option requires the process be specified by name".into(),
        ));
    }
    if (process_group || wait_for_process)
        && output
            .as_ref()
            .is_some_and(|value| value.file_name.is_some())
    {
        return Err(ParseError::InvalidCombination(
            "a custom dump name is invalid when monitoring multiple processes".into(),
        ));
    }
    if memory_mb
        .as_ref()
        .is_some_and(|threshold| threshold.len() > 1)
        && dump_count.is_some()
    {
        return Err(ParseError::InvalidCombination(
            "-n is invalid when multiple memory thresholds are specified".into(),
        ));
    }

    if exception_filter.is_some() && dotnet_trigger != Some(DotNetTrigger::Exception) {
        return Err(ParseError::InvalidCombination(
            "-f requires the -e exception trigger".into(),
        ));
    }
    if sample_rate.is_some_and(|value| value > 0) && restrack_generate_dump.is_none() {
        return Err(ParseError::InvalidCombination(
            "-sr requires resource tracking".into(),
        ));
    }
    if exclude_filter.is_some() && restrack_generate_dump.is_none() {
        return Err(ParseError::InvalidCombination(
            "-fx requires resource tracking".into(),
        ));
    }

    let signal_or_exception = signals.as_ref().is_some_and(|values| !values.is_empty())
        || dotnet_trigger == Some(DotNetTrigger::Exception);
    if signal_or_exception
        && (cpu.is_some()
            || memory_mb.is_some()
            || thread_count.is_some()
            || file_descriptor_count.is_some()
            || !perf_counters.is_empty())
    {
        return Err(ParseError::InvalidCombination(
            "signal and exception triggers must be the only trigger specified".into(),
        ));
    }
    if signal_or_exception && polling_interval_ms.is_some() {
        return Err(ParseError::InvalidCombination(
            "the polling interval is invalid during signal or exception monitoring".into(),
        ));
    }

    let memory_dump_count = memory_mb.as_ref().map_or_else(
        || match &dotnet_trigger {
            Some(DotNetTrigger::GcMemory { thresholds_mb, .. }) => thresholds_mb.len(),
            _ => 0,
        },
        Threshold::len,
    ) as u32;
    if memory_dump_count > 1 && dump_count.is_some() {
        return Err(ParseError::InvalidCombination(
            "-n is invalid when multiple memory thresholds are specified".into(),
        ));
    }

    let explicit_restrack_timer =
        restrack_generate_dump.is_some() && threshold_seconds.is_some_and(|seconds| seconds > 0);
    let timer_trigger = !signal_or_exception
        && (explicit_restrack_timer
            || (cpu.is_none()
                && memory_mb.is_none()
                && thread_count.is_none()
                && file_descriptor_count.is_none()
                && dotnet_trigger.is_none()
                && signals.as_ref().is_none_or(Vec::is_empty)
                && perf_counters.is_empty()
                && restrack_generate_dump.is_none()));

    let restrack = restrack_generate_dump.map(|generate_dump| RestrackConfig {
        generate_dump,
        sample_rate: sample_rate.unwrap_or(DEFAULT_SAMPLE_RATE),
        exclude_filter,
    });

    let config = Config {
        target,
        output: output.unwrap_or_default(),
        cpu,
        memory_mb,
        thread_count,
        file_descriptor_count,
        polling_interval_ms: polling_interval_ms.unwrap_or(DEFAULT_POLLING_INTERVAL_MS),
        threshold_seconds: threshold_seconds.unwrap_or(DEFAULT_THRESHOLD_SECONDS),
        dump_count: if memory_dump_count > 1 {
            memory_dump_count
        } else {
            dump_count.unwrap_or(DEFAULT_DUMP_COUNT)
        },
        wait_for_process,
        overwrite,
        diagnostics,
        use_gcore,
        timer_trigger,
        signals: signals.unwrap_or_default(),
        dotnet_trigger,
        exception_filter,
        perf_counters,
        restrack,
        core_dump_mask,
    };
    config.validate()?;
    Ok(config)
}

impl<T> Threshold<Vec<T>> {
    fn len(&self) -> usize {
        match self {
            Self::AtLeast(values) | Self::Below(values) => values.len(),
        }
    }
}

fn normalize_option(argument: &OsStr) -> Option<String> {
    let argument = argument.to_str()?;
    let (prefix, option) = argument
        .strip_prefix('-')
        .map(|option| ('-', option))
        .or_else(|| argument.strip_prefix('/').map(|option| ('/', option)))?;
    if option.is_empty() {
        return None;
    }
    let option = option.to_ascii_lowercase();
    let recognized = matches!(
        option.as_str(),
        "?" | "c"
            | "cl"
            | "m"
            | "ml"
            | "gcm"
            | "gcgen"
            | "restrack"
            | "sr"
            | "sig"
            | "mc"
            | "pc"
            | "pcl"
            | "tc"
            | "fc"
            | "pf"
            | "n"
            | "s"
            | "log"
            | "e"
            | "f"
            | "fx"
            | "o"
            | "w"
            | "pgid"
    ) || (prefix == '-' && option == "usegcore");

    recognized.then_some(option)
}

fn next_value<'a>(
    arguments: &'a [OsString],
    index: &mut usize,
    option: &str,
) -> Result<&'a OsString, ParseError> {
    *index += 1;
    arguments
        .get(*index)
        .ok_or_else(|| ParseError::MissingValue(option.into()))
}

fn parse_next_number<T>(
    arguments: &[OsString],
    index: &mut usize,
    option: &str,
) -> Result<T, ParseError>
where
    T: std::str::FromStr,
{
    let value = next_value(arguments, index, option)?;
    parse_number(option, value)
}

fn parse_number<T>(option: &str, value: &OsStr) -> Result<T, ParseError>
where
    T: std::str::FromStr,
{
    value
        .to_str()
        .and_then(|value| value.parse().ok())
        .ok_or_else(|| ParseError::InvalidValue {
            option: option.into(),
            value: value.to_owned(),
        })
}

fn parse_hex(option: &str, value: &OsStr) -> Result<u32, ParseError> {
    let parsed = value.to_str().and_then(|value| {
        u32::from_str_radix(
            value
                .strip_prefix("0x")
                .or_else(|| value.strip_prefix("0X"))
                .unwrap_or(value),
            16,
        )
        .ok()
    });
    parsed.ok_or_else(|| ParseError::InvalidValue {
        option: option.into(),
        value: value.to_owned(),
    })
}

fn parse_gc_memory(value: &OsStr) -> Result<(GcHeap, Vec<u64>), ParseError> {
    let Some(value) = value.to_str() else {
        return Err(ParseError::InvalidValue {
            option: "gcm".into(),
            value: value.to_owned(),
        });
    };
    let (heap, thresholds) = if let Some((heap, thresholds)) = value.split_once(':') {
        let heap = if heap.eq_ignore_ascii_case("loh") {
            GcHeap::LargeObject
        } else if heap.eq_ignore_ascii_case("poh") {
            GcHeap::PinnedObject
        } else {
            let generation = heap
                .parse::<u8>()
                .ok()
                .filter(|generation| *generation <= 2);
            GcHeap::Generation(generation.ok_or_else(|| ParseError::InvalidValue {
                option: "gcm".into(),
                value: value.into(),
            })?)
        };
        (heap, thresholds)
    } else {
        (GcHeap::Cumulative, value)
    };
    let thresholds = parse_list::<u64>("gcm", OsStr::new(thresholds))?;
    Ok((heap, thresholds))
}

fn parse_perf_counter(
    specification: &OsStr,
    threshold: &OsStr,
    below: bool,
) -> Result<PerfCounterTrigger, ParseError> {
    let Some(specification) = specification.to_str() else {
        return Err(ParseError::InvalidValue {
            option: "pc".into(),
            value: specification.to_owned(),
        });
    };
    let Some((provider, counter)) = specification.split_once(':') else {
        return Err(ParseError::InvalidValue {
            option: "pc".into(),
            value: specification.into(),
        });
    };
    if provider.is_empty() || counter.is_empty() {
        return Err(ParseError::InvalidValue {
            option: "pc".into(),
            value: specification.into(),
        });
    }

    let (counter, percentile) = parse_percentile(counter);
    let threshold = parse_number::<f64>("pc", threshold)?;
    if !threshold.is_finite() {
        return Err(ParseError::InvalidValue {
            option: "pc".into(),
            value: threshold.to_string().into(),
        });
    }

    Ok(PerfCounterTrigger {
        provider: provider.into(),
        counter,
        threshold,
        below,
        percentile,
    })
}

fn parse_percentile(counter: &str) -> (String, Option<f64>) {
    let Some(bracket) = counter.find("[p") else {
        return (counter.into(), None);
    };
    let Some(end) = counter[bracket + 2..].find(']') else {
        return (counter.into(), None);
    };
    let end = bracket + 2 + end;
    let percentile = counter[bracket + 2..end]
        .parse::<f64>()
        .ok()
        .map(|value| if value >= 1.0 { value / 100.0 } else { value })
        .filter(|value| *value > 0.0 && *value < 1.0);
    (counter[..bracket].into(), percentile)
}

fn require_capability(supported: bool, option: &str) -> Result<(), ParseError> {
    if supported {
        Ok(())
    } else {
        Err(ParseError::UnsupportedOption(option.into()))
    }
}

fn parse_list<T>(option: &str, value: &OsStr) -> Result<Vec<T>, ParseError>
where
    T: std::str::FromStr,
{
    let Some(value) = value.to_str() else {
        return Err(ParseError::InvalidValue {
            option: option.into(),
            value: value.to_owned(),
        });
    };
    let values: Option<Vec<T>> = value.split(',').map(|item| item.parse().ok()).collect();
    values
        .filter(|values| !values.is_empty())
        .ok_or_else(|| ParseError::InvalidValue {
            option: option.into(),
            value: value.into(),
        })
}

fn ensure_absent<T>(value: &Option<T>, option: &str) -> Result<(), ParseError> {
    if value.is_some() {
        Err(ParseError::DuplicateOption(option.into()))
    } else {
        Ok(())
    }
}

fn parse_target(value: OsString, process_group: bool) -> Result<TargetSpec, ParseError> {
    let numeric = value
        .to_str()
        .filter(|value| !value.is_empty() && value.bytes().all(|byte| byte.is_ascii_digit()))
        .and_then(|value| value.parse::<i32>().ok());

    if process_group {
        numeric
            .map(TargetSpec::ProcessGroup)
            .ok_or_else(|| ParseError::InvalidValue {
                option: "pgid".into(),
                value,
            })
    } else {
        Ok(numeric.map_or(TargetSpec::Name(value), TargetSpec::Pid))
    }
}

fn parse_output<F>(value: &OsStr, is_directory: &F) -> Result<OutputSpec, ParseError>
where
    F: Fn(&Path) -> bool,
{
    let path = PathBuf::from(value);
    let text_ends_in_separator = value.to_string_lossy().ends_with('/');
    if is_directory(&path) || text_ends_in_separator {
        if !is_directory(&path) {
            return Err(ParseError::InvalidOutputDirectory(path));
        }
        return Ok(OutputSpec {
            directory: path,
            file_name: None,
        });
    }

    let directory = path.parent().unwrap_or_else(|| Path::new("."));
    if !is_directory(directory) {
        return Err(ParseError::InvalidOutputDirectory(directory.to_path_buf()));
    }

    Ok(OutputSpec {
        directory: if directory.as_os_str().is_empty() {
            PathBuf::from(".")
        } else {
            directory.to_path_buf()
        },
        file_name: path.file_name().map(OsStr::to_owned),
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    fn parse_test(arguments: &[&str], platform: Platform) -> Result<Config, ParseError> {
        parse_with_directory_check(arguments.iter().copied(), platform, |path| {
            matches!(path.to_str(), Some(".") | Some("/tmp") | Some("/tmp/dumps"))
        })
    }

    #[test]
    fn parses_linux_cpu_scenario_with_defaults() {
        let config = parse_test(
            &["-log", "stdout", "-c", "25", "1234", "/tmp/dumps"],
            Platform::Linux,
        )
        .unwrap();

        assert_eq!(config.target, TargetSpec::Pid(1234));
        assert_eq!(config.cpu, Some(Threshold::AtLeast(25)));
        assert_eq!(config.dump_count, 1);
        assert_eq!(config.threshold_seconds, 10);
        assert_eq!(config.polling_interval_ms, 1_000);
        assert_eq!(config.diagnostics, DiagnosticsTarget::Stdout);
        assert!(!config.timer_trigger);
    }

    #[test]
    fn accepts_case_insensitive_slash_options_after_target() {
        let config = parse_test(
            &["ProcDumpTestApplication", "/CL", "20", "/N", "3", "/tmp"],
            Platform::Linux,
        )
        .unwrap();

        assert_eq!(
            config.target,
            TargetSpec::Name(OsString::from("ProcDumpTestApplication"))
        );
        assert_eq!(config.cpu, Some(Threshold::Below(20)));
        assert_eq!(config.dump_count, 3);
    }

    #[test]
    fn multiple_memory_thresholds_set_dump_count() {
        let config = parse_test(&["-m", "20,40,60", "42"], Platform::Linux).unwrap();

        assert_eq!(config.memory_mb, Some(Threshold::AtLeast(vec![20, 40, 60])));
        assert_eq!(config.dump_count, 3);
    }

    #[test]
    fn custom_name_is_rejected_for_wait_by_name() {
        let error = parse_test(
            &["-w", "-c", "25", "worker", "/tmp/custom.core"],
            Platform::Linux,
        )
        .unwrap_err();

        assert!(matches!(error, ParseError::InvalidCombination(_)));
    }

    #[test]
    fn macos_rejects_process_groups() {
        let error = parse_test(&["-pgid", "123"], Platform::MacOs).unwrap_err();

        assert_eq!(error, ParseError::UnsupportedOption("pgid".into()));
    }

    #[test]
    fn no_explicit_trigger_enables_timer() {
        let config = parse_test(&["1234"], Platform::Linux).unwrap();

        assert!(config.timer_trigger);
        assert_eq!(config.threshold_seconds, DEFAULT_THRESHOLD_SECONDS);
    }

    #[cfg(feature = "dotnet-triggers")]
    #[test]
    fn parses_loh_gc_threshold_scenario() {
        let config = parse_test(
            &["-log", "stdout", "-gcm", "LOH:10,20,30", "-w", "TestWebApi"],
            Platform::Linux,
        )
        .unwrap();

        assert_eq!(
            config.dotnet_trigger,
            Some(DotNetTrigger::GcMemory {
                heap: GcHeap::LargeObject,
                thresholds_mb: vec![10, 20, 30],
            })
        );
        assert_eq!(config.dump_count, 3);
        assert!(!config.timer_trigger);
    }

    #[cfg(feature = "dotnet-triggers")]
    #[test]
    fn parses_histogram_percentile_counter() {
        let config = parse_test(
            &[
                "-pc",
                "Microsoft.AspNetCore.Hosting:http.server.request.duration[p95]",
                "0.5",
                "-n",
                "1",
                "42",
            ],
            Platform::Linux,
        )
        .unwrap();

        assert_eq!(config.perf_counters.len(), 1);
        assert_eq!(
            config.perf_counters[0].provider,
            "Microsoft.AspNetCore.Hosting"
        );
        assert_eq!(
            config.perf_counters[0].counter,
            "http.server.request.duration"
        );
        assert_eq!(config.perf_counters[0].percentile, Some(0.95));
        assert_eq!(config.perf_counters[0].threshold, 0.5);
    }

    #[test]
    fn signal_rejects_polling_and_other_triggers() {
        let polling = parse_test(&["-sig", "10,12", "-pf", "1000", "42"], Platform::Linux);
        assert!(matches!(polling, Err(ParseError::InvalidCombination(_))));

        let cpu = parse_test(&["-sig", "10", "-c", "50", "42"], Platform::Linux);
        assert!(matches!(cpu, Err(ParseError::InvalidCombination(_))));
    }

    #[cfg(feature = "restrack")]
    #[test]
    fn restrack_timer_requires_explicit_seconds() {
        let manual = parse_test(&["-restrack", "42"], Platform::Linux).unwrap();
        assert!(!manual.timer_trigger);
        assert_eq!(manual.restrack.unwrap().sample_rate, DEFAULT_SAMPLE_RATE);

        let timed = parse_test(&["-restrack", "nodump", "-s", "5", "42"], Platform::Linux).unwrap();
        assert!(timed.timer_trigger);
        assert!(!timed.restrack.unwrap().generate_dump);
    }

    #[cfg(feature = "restrack")]
    #[test]
    fn restrack_sample_rate_matches_ebpf_range() {
        let zero = parse_test(&["-restrack", "-sr", "0", "42"], Platform::Linux).unwrap();
        assert_eq!(zero.restrack.unwrap().sample_rate, DEFAULT_SAMPLE_RATE);

        let too_large = parse_test(&["-restrack", "-sr", "2147483648", "42"], Platform::Linux);
        assert!(matches!(too_large, Err(ParseError::InvalidValue { .. })));
    }

    #[test]
    fn macos_rejects_linux_advanced_options() {
        for option in ["-sig", "-gcm", "-restrack", "-mc", "-pc"] {
            let error = parse_test(&[option, "1", "42"], Platform::MacOs).unwrap_err();
            assert!(matches!(error, ParseError::UnsupportedOption(_)));
        }
    }

    #[test]
    fn builder_rejects_invalid_safe_api_states() {
        assert!(Config::builder(TargetSpec::Pid(0)).build().is_err());
        assert!(
            Config::builder(TargetSpec::Pid(42))
                .dump_count(0)
                .build()
                .is_err()
        );
        assert!(
            Config::builder(TargetSpec::Pid(42))
                .polling_interval_ms(0)
                .build()
                .is_err()
        );
        assert!(
            Config::builder(TargetSpec::Pid(42))
                .signals(vec![libc::SIGKILL])
                .build()
                .is_err()
        );
    }

    #[test]
    fn builder_creates_cpu_monitor_configuration() {
        let config = Config::builder(TargetSpec::Pid(42))
            .cpu(Threshold::AtLeast(80))
            .dump_count(2)
            .build()
            .unwrap();

        assert_eq!(config.cpu, Some(Threshold::AtLeast(80)));
        assert_eq!(config.dump_count(), 2);
        assert!(!config.timer_trigger);
    }
}
