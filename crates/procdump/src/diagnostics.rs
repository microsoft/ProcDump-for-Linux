use crate::config::DiagnosticsTarget;
use std::ffi::CStr;

pub(crate) fn info(_target: DiagnosticsTarget, message: impl AsRef<str>) {
    let message = message.as_ref();
    let line = format_log_line(&local_time(), "INFO", message);
    println!("{line}");
}

pub fn error(message: impl AsRef<str>) {
    println!(
        "{}",
        format_log_line(&local_time(), "ERROR", message.as_ref())
    );
}

pub(crate) fn format_log_line(timestamp: &str, level: &str, message: &str) -> String {
    format!("[{timestamp} - {level}]: {message}")
}

fn local_time() -> String {
    let mut now = unsafe { std::mem::zeroed::<libc::time_t>() };
    if unsafe { libc::time(&raw mut now) } == -1 {
        return "00:00:00".into();
    }
    let mut local = unsafe { std::mem::zeroed::<libc::tm>() };
    if unsafe { libc::localtime_r(&now, &raw mut local) }.is_null() {
        return "00:00:00".into();
    }
    let mut buffer = [0 as libc::c_char; 9];
    if unsafe {
        libc::strftime(
            buffer.as_mut_ptr(),
            buffer.len(),
            c"%T".as_ptr(),
            &raw const local,
        )
    } == 0
    {
        return "00:00:00".into();
    }
    unsafe { CStr::from_ptr(buffer.as_ptr()) }
        .to_string_lossy()
        .into_owned()
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn info_line_matches_legacy_character_for_character() {
        assert_eq!(
            format_log_line(
                "12:34:56",
                "INFO",
                "Press Ctrl-C to end monitoring without terminating the process(es)."
            ),
            "[12:34:56 - INFO]: Press Ctrl-C to end monitoring without terminating the process(es)."
        );
    }
}
