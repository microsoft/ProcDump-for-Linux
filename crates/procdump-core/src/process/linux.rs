#![allow(unsafe_code)]

use super::{
    ProcessDiscovery, ProcessError, ProcessId, ProcessIdentity, ProcessMetrics, ProcessSnapshot,
    ProcessState,
};
use std::ffi::{OsStr, OsString};
use std::fs;
use std::io;
use std::path::{Path, PathBuf};

#[derive(Clone, Debug)]
pub struct LinuxProcfs {
    root: PathBuf,
    page_size: u64,
    clock_ticks_per_second: u64,
}

impl LinuxProcfs {
    pub fn new() -> Result<Self, ProcessError> {
        let page_size = sysconf(libc::_SC_PAGESIZE, "page size")?;
        let clock_ticks_per_second = sysconf(libc::_SC_CLK_TCK, "clock ticks")?;
        Ok(Self {
            root: PathBuf::from("/proc"),
            page_size,
            clock_ticks_per_second,
        })
    }

    #[cfg(test)]
    fn with_root(root: PathBuf, page_size: u64, clock_ticks_per_second: u64) -> Self {
        Self {
            root,
            page_size,
            clock_ticks_per_second,
        }
    }

    fn process_path(&self, pid: ProcessId, leaf: &str) -> PathBuf {
        self.root.join(pid.get().to_string()).join(leaf)
    }

    fn read_stat(&self, pid: ProcessId) -> Result<Stat, ProcessError> {
        let path = self.process_path(pid, "stat");
        let value = fs::read_to_string(&path).map_err(|error| map_process_io(pid, &path, error))?;
        parse_stat(&value, &path)
    }

    fn uptime_seconds(&self) -> Result<u64, ProcessError> {
        let path = self.root.join("uptime");
        let value =
            fs::read_to_string(&path).map_err(|error| ProcessError::io("read", &path, error))?;
        value
            .split_whitespace()
            .next()
            .and_then(|value| value.parse::<f64>().ok())
            .filter(|value| value.is_finite() && *value >= 0.0)
            .map(|value| value as u64)
            .ok_or_else(|| ProcessError::invalid_data(path, "missing uptime"))
    }

    fn file_descriptor_count(&self, pid: ProcessId) -> Result<u64, ProcessError> {
        let path = self.process_path(pid, "fdinfo");
        let entries = fs::read_dir(&path).map_err(|error| map_process_io(pid, &path, error))?;
        let mut count = 0_u64;
        for entry in entries {
            entry.map_err(|error| ProcessError::io("enumerate", &path, error))?;
            count += 1;
        }
        Ok(count)
    }

    fn process_name(&self, pid: ProcessId, fallback: &OsStr) -> Result<OsString, ProcessError> {
        let path = self.process_path(pid, "cmdline");
        let command_line = fs::read(&path).map_err(|error| map_process_io(pid, &path, error))?;
        let mut arguments = command_line
            .split(|byte| *byte == 0)
            .filter(|argument| !argument.is_empty());
        let first = arguments.next();
        let executable = if first == Some(b"sudo".as_slice()) {
            arguments.next()
        } else {
            first
        };

        Ok(executable
            .and_then(|value| Path::new(OsStr::from_bytes(value)).file_name())
            .map_or_else(|| fallback.to_owned(), OsStr::to_owned))
    }
}

impl ProcessDiscovery for LinuxProcfs {
    fn list_processes(&self) -> Result<Vec<ProcessId>, ProcessError> {
        let entries = fs::read_dir(&self.root)
            .map_err(|error| ProcessError::io("enumerate", &self.root, error))?;
        let mut processes = Vec::new();
        for entry in entries {
            let entry = entry.map_err(|error| ProcessError::io("enumerate", &self.root, error))?;
            if let Some(pid) = entry
                .file_name()
                .to_str()
                .and_then(|value| value.parse::<i32>().ok())
                .and_then(|value| ProcessId::new(value).ok())
            {
                processes.push(pid);
            }
        }
        processes.sort_unstable();
        Ok(processes)
    }

    fn identity(&self, pid: ProcessId) -> Result<ProcessIdentity, ProcessError> {
        let stat = self.read_stat(pid)?;
        Ok(ProcessIdentity {
            pid,
            start_time: stat.start_time,
        })
    }

    fn name(&self, pid: ProcessId) -> Result<OsString, ProcessError> {
        let stat = self.read_stat(pid)?;
        self.process_name(pid, OsStr::new(&stat.command))
    }

    fn process_group(&self, pid: ProcessId) -> Result<i32, ProcessError> {
        Ok(self.read_stat(pid)?.process_group)
    }

    fn is_alive(&self, identity: ProcessIdentity) -> Result<bool, ProcessError> {
        match self.identity(identity.pid) {
            Ok(current) => Ok(current == identity),
            Err(ProcessError::Disappeared(_)) => Ok(false),
            Err(error) => Err(error),
        }
    }
}

impl ProcessMetrics for LinuxProcfs {
    fn sample(&self, pid: ProcessId) -> Result<ProcessSnapshot, ProcessError> {
        let stat = self.read_stat(pid)?;
        let name = self.process_name(pid, OsStr::new(&stat.command))?;
        let file_descriptor_count = self.file_descriptor_count(pid)?;
        Ok(ProcessSnapshot {
            identity: ProcessIdentity {
                pid,
                start_time: stat.start_time,
            },
            name,
            state: ProcessState::from(stat.state),
            parent_pid: stat.parent_pid,
            process_group: stat.process_group,
            cpu_time_ticks: stat.user_ticks.saturating_add(stat.system_ticks),
            rss_bytes: stat.rss_pages.saturating_mul(self.page_size),
            swap_bytes: stat.swap_pages.saturating_mul(self.page_size),
            thread_count: stat.thread_count,
            file_descriptor_count,
        })
    }

    fn cpu_usage_percent(&self, snapshot: &ProcessSnapshot) -> Result<u32, ProcessError> {
        let uptime = self.uptime_seconds()?;
        let process_seconds = snapshot.cpu_time_ticks / self.clock_ticks_per_second;
        let start_seconds = snapshot.identity.start_time / self.clock_ticks_per_second;
        let elapsed = uptime.saturating_sub(start_seconds);
        if elapsed == 0 {
            return Ok(0);
        }
        Ok(process_seconds.saturating_mul(100).saturating_div(elapsed) as u32)
    }
}

#[derive(Clone, Debug, Eq, PartialEq)]
struct Stat {
    command: String,
    state: char,
    parent_pid: i32,
    process_group: i32,
    user_ticks: u64,
    system_ticks: u64,
    thread_count: u64,
    start_time: u64,
    rss_pages: u64,
    swap_pages: u64,
}

fn parse_stat(value: &str, path: &Path) -> Result<Stat, ProcessError> {
    let command_start = value
        .find('(')
        .ok_or_else(|| ProcessError::invalid_data(path, "missing command start"))?;
    let command_end = value
        .rfind(')')
        .filter(|end| *end > command_start)
        .ok_or_else(|| ProcessError::invalid_data(path, "missing command end"))?;
    let command = value[command_start + 1..command_end].to_owned();
    let fields: Vec<&str> = value[command_end + 1..].split_whitespace().collect();

    Ok(Stat {
        command,
        state: field(&fields, 0, "state", path)?
            .chars()
            .next()
            .ok_or_else(|| ProcessError::invalid_data(path, "empty state"))?,
        parent_pid: parse_field(&fields, 1, "parent PID", path)?,
        process_group: parse_field(&fields, 2, "process group", path)?,
        user_ticks: parse_field(&fields, 11, "user ticks", path)?,
        system_ticks: parse_field(&fields, 12, "system ticks", path)?,
        thread_count: parse_field(&fields, 17, "thread count", path)?,
        start_time: parse_field(&fields, 19, "start time", path)?,
        rss_pages: parse_field::<i64>(&fields, 21, "resident pages", path)?.max(0) as u64,
        swap_pages: parse_field(&fields, 33, "swap pages", path)?,
    })
}

fn field<'a>(
    fields: &'a [&str],
    index: usize,
    name: &str,
    path: &Path,
) -> Result<&'a str, ProcessError> {
    fields
        .get(index)
        .copied()
        .ok_or_else(|| ProcessError::invalid_data(path, format!("missing {name}")))
}

fn parse_field<T>(fields: &[&str], index: usize, name: &str, path: &Path) -> Result<T, ProcessError>
where
    T: std::str::FromStr,
{
    field(fields, index, name, path)?
        .parse()
        .map_err(|_| ProcessError::invalid_data(path, format!("invalid {name}")))
}

fn map_process_io(pid: ProcessId, path: &Path, error: io::Error) -> ProcessError {
    if error.kind() == io::ErrorKind::NotFound {
        ProcessError::Disappeared(pid)
    } else {
        ProcessError::io("read", path, error)
    }
}

fn sysconf(name: libc::c_int, description: &str) -> Result<u64, ProcessError> {
    let value = unsafe { libc::sysconf(name) };
    if value <= 0 {
        Err(ProcessError::invalid_data(
            "/proc",
            format!("unable to determine {description}"),
        ))
    } else {
        Ok(value as u64)
    }
}

use std::os::unix::ffi::OsStrExt;

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn parses_stat_with_spaces_and_parentheses_in_command() {
        let mut fields = vec!["0"; 34];
        fields[0] = "S";
        fields[1] = "7";
        fields[2] = "9";
        fields[11] = "500";
        fields[12] = "25";
        fields[17] = "8";
        fields[19] = "12345";
        fields[20] = "4096";
        fields[21] = "256";
        fields[33] = "12";
        let input = format!("42 (worker (pool) 1) {}", fields.join(" "));
        let stat = parse_stat(&input, Path::new("stat")).unwrap();

        assert_eq!(stat.command, "worker (pool) 1");
        assert_eq!(stat.parent_pid, 7);
        assert_eq!(stat.process_group, 9);
        assert_eq!(stat.user_ticks, 500);
        assert_eq!(stat.system_ticks, 25);
        assert_eq!(stat.thread_count, 8);
        assert_eq!(stat.start_time, 12345);
        assert_eq!(stat.rss_pages, 256);
        assert_eq!(stat.swap_pages, 12);
    }

    #[test]
    fn samples_current_process() {
        let procfs = LinuxProcfs::new().unwrap();
        let pid = ProcessId::new(std::process::id() as i32).unwrap();
        let snapshot = procfs.sample(pid).unwrap();

        assert_eq!(snapshot.identity.pid, pid);
        assert!(snapshot.identity.start_time > 0);
        assert!(snapshot.thread_count > 0);
        assert!(!snapshot.name.is_empty());
        assert!(procfs.is_alive(snapshot.identity).unwrap());
    }

    #[test]
    fn calculates_legacy_lifetime_cpu_percentage() {
        let root = std::env::temp_dir().join(format!("procdump-rs-{}", std::process::id()));
        fs::create_dir_all(&root).unwrap();
        fs::write(root.join("uptime"), "200.75 0.00\n").unwrap();
        let procfs = LinuxProcfs::with_root(root.clone(), 4096, 100);
        let snapshot = ProcessSnapshot {
            identity: ProcessIdentity {
                pid: ProcessId::new(42).unwrap(),
                start_time: 10_000,
            },
            name: "worker".into(),
            state: ProcessState::Running,
            parent_pid: 1,
            process_group: 42,
            cpu_time_ticks: 2_500,
            rss_bytes: 0,
            swap_bytes: 0,
            thread_count: 1,
            file_descriptor_count: 3,
        };

        assert_eq!(procfs.cpu_usage_percent(&snapshot).unwrap(), 25);
        fs::remove_dir_all(root).unwrap();
    }
}
