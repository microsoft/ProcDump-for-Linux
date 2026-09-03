use std::path::Path;

pub(crate) const PRESS_CTRL_C: &str =
    "Press Ctrl-C to end monitoring without terminating the process(es).";
#[cfg(any(test, all(target_os = "linux", feature = "restrack")))]
pub(crate) const RESTRACK_PROMPT: &str =
    "Press 't' to trigger a Restrack snapshot (or any other key to exit)...";
#[cfg(any(test, all(target_os = "linux", feature = "restrack")))]
pub(crate) const RESTRACK_TRIGGERED: &str = "Triggering Restrack snapshot...";

pub(crate) fn waiting_for_processes(name: &str) -> String {
    format!("Waiting for processes '{name}' to launch\n")
}

pub(crate) fn monitoring_process_group(group: i32) -> String {
    format!("Monitoring processes of PGID '{group}'\n")
}

pub(crate) fn starting_monitor(name: &str, pid: i32) -> String {
    format!("Starting monitor for process {name} ({pid})")
}

pub(crate) fn stopping_monitor(name: &str, pid: i32) -> String {
    format!("Stopping monitor for process {name} ({pid})")
}

pub(crate) fn stopping_monitors(name: &str, pid: i32) -> String {
    format!("Stopping monitors for process: {name} ({pid})")
}

pub(crate) fn core_dump(number: u32, path: &Path) -> String {
    format!("Core dump {number} generated: {}", path.display())
}

#[cfg(any(test, all(target_os = "linux", feature = "dotnet-triggers")))]
pub(crate) fn managed_core_dump(path: &Path) -> String {
    format!("Core dump generated: {}", path.display())
}

pub(crate) fn commit_trigger(usage: u64, pid: i32) -> String {
    format!("Trigger: Commit usage:{usage}MB on process ID: {pid}")
}

pub(crate) fn thread_trigger(count: u64, pid: i32) -> String {
    format!("Trigger: Thread count:{count} on process ID: {pid}")
}

#[cfg(any(test, target_os = "linux"))]
pub(crate) fn signal_trigger(signal: i32, pid: i32) -> String {
    format!("Trigger: Signal:{signal} on process ID: {pid}")
}

pub(crate) fn cpu_trigger(usage: u32, pid: i32) -> String {
    format!("Trigger: CPU usage:{usage}% on process ID: {pid}")
}

pub(crate) fn timer_trigger(polling_seconds: u64, pid: i32) -> String {
    format!("Trigger: Timer:{polling_seconds}(s) on process ID: {pid}")
}

#[cfg(any(test, all(target_os = "linux", feature = "dotnet-triggers")))]
pub(crate) fn performance_counter_trigger(
    provider: &str,
    counter: &str,
    value: f64,
    threshold: f64,
    pid: i32,
) -> String {
    format!(
        "Trigger: {provider}:{counter} value:{value:.4} threshold:{threshold:.4} on process ID: {pid}"
    )
}

#[cfg(any(test, all(target_os = "linux", feature = "restrack")))]
pub(crate) fn leak_report(path: &Path) -> String {
    format!("Leak report generated: {}", path.display())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn informational_templates_match_legacy_character_for_character() {
        let actual = [
            PRESS_CTRL_C.into(),
            waiting_for_processes("worker"),
            monitoring_process_group(42),
            starting_monitor("worker", 1234),
            stopping_monitor("worker", 1234),
            stopping_monitors("worker", 1234),
            core_dump(0, Path::new("/tmp/core.1234")),
            managed_core_dump(Path::new("/tmp/core.1234")),
            commit_trigger(100, 1234),
            thread_trigger(25, 1234),
            signal_trigger(12, 1234),
            cpu_trigger(65, 1234),
            timer_trigger(1, 1234),
            performance_counter_trigger("Provider", "Counter", 1.25, 1.0, 1234),
            leak_report(Path::new("/tmp/core.1234.restrack")),
            RESTRACK_PROMPT.into(),
            RESTRACK_TRIGGERED.into(),
        ];
        let actual = actual
            .iter()
            .map(|line| line.replace('\n', "\\n"))
            .collect::<Vec<_>>();
        let expected = include_str!("../../../tests/cli-compat/legacy-info-messages.txt")
            .lines()
            .collect::<Vec<_>>();

        assert_eq!(actual, expected);
    }

    #[test]
    fn monitor_sources_enforce_legacy_character_for_character_contract() {
        let sources = [
            ("monitor.rs", include_str!("monitor.rs")),
            ("orchestrator.rs", include_str!("orchestrator.rs")),
            ("eventpipe.rs", include_str!("eventpipe.rs")),
            ("restrack.rs", include_str!("restrack.rs")),
            ("signal.rs", include_str!("signal.rs")),
        ];
        let protected_prefixes = [
            "\"Press Ctrl-C",
            "\"Waiting for processes",
            "\"Monitoring processes",
            "\"Starting monitor",
            "\"Stopping monitor",
            "\"Core dump",
            "\"Trigger:",
            "\"Leak report generated",
            "\"Press 't'",
            "\"Triggering Restrack",
        ];

        for (name, source) in sources {
            for prefix in protected_prefixes {
                assert!(
                    !source.contains(prefix),
                    "{name} bypasses cli_output with {prefix}"
                );
            }
        }
    }
}
