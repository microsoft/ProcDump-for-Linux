use crate::config::DiagnosticsTarget;
use std::ffi::CString;
use std::sync::Once;

static OPEN_SYSLOG: Once = Once::new();

pub(crate) fn info(target: DiagnosticsTarget, message: impl AsRef<str>) {
    let message = message.as_ref();
    println!("{message}");
    if target == DiagnosticsTarget::Syslog {
        write_syslog(libc::LOG_INFO, message);
    }
}

fn write_syslog(priority: libc::c_int, message: &str) {
    OPEN_SYSLOG.call_once(|| unsafe {
        libc::openlog(c"procdump".as_ptr(), libc::LOG_PID, libc::LOG_USER);
    });
    let message = CString::new(message.replace('\0', " ")).unwrap_or_default();
    unsafe {
        libc::syslog(priority, c"%s".as_ptr(), message.as_ptr());
    }
}
