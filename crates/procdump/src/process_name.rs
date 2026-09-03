use std::ffi::OsString;

#[cfg(target_os = "linux")]
pub(crate) fn name(pid: i32) -> Result<OsString, String> {
    use std::ffi::OsStr;
    use std::fs;
    use std::os::unix::ffi::OsStrExt;
    use std::path::Path;

    let stat_path = format!("/proc/{pid}/stat");
    let stat = fs::read_to_string(&stat_path).map_err(|error| {
        if error.kind() == std::io::ErrorKind::NotFound {
            format!("process {pid} no longer exists")
        } else {
            format!("failed to read {stat_path}: {error}")
        }
    })?;
    let command_start = stat
        .find('(')
        .ok_or_else(|| format!("invalid process data in {stat_path}: missing command start"))?;
    let command_end = stat
        .rfind(')')
        .filter(|end| *end > command_start)
        .ok_or_else(|| format!("invalid process data in {stat_path}: missing command end"))?;
    let fallback = OsString::from(&stat[command_start + 1..command_end]);

    let command_path = format!("/proc/{pid}/cmdline");
    let command_line = fs::read(&command_path)
        .map_err(|error| format!("failed to read {command_path}: {error}"))?;
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
        .map_or(fallback, OsStr::to_owned))
}

#[cfg(target_os = "macos")]
pub(crate) fn name(pid: i32) -> Result<OsString, String> {
    use std::ffi::OsStr;
    use std::os::unix::ffi::OsStrExt;
    use std::path::Path;

    const PROC_PIDPATHINFO_MAXSIZE: usize = 4096;
    let mut path = vec![0_u8; PROC_PIDPATHINFO_MAXSIZE];
    let bytes = unsafe { proc_pidpath(pid, path.as_mut_ptr().cast(), path.len() as u32) };
    if bytes <= 0 {
        return Err(format!(
            "failed to proc_pidpath pid {pid}: {}",
            std::io::Error::last_os_error()
        ));
    }
    if bytes as usize > path.len() {
        return Err("proc_pidpath returned an oversized path".into());
    }
    path.truncate(bytes as usize);
    let path = OsStr::from_bytes(&path);
    Ok(Path::new(path)
        .file_name()
        .map_or_else(|| path.to_owned(), OsStr::to_owned))
}

#[cfg(target_os = "macos")]
unsafe extern "C" {
    fn proc_pidpath(
        pid: std::ffi::c_int,
        buffer: *mut std::ffi::c_void,
        buffer_size: u32,
    ) -> std::ffi::c_int;
}

#[cfg(not(any(target_os = "linux", target_os = "macos")))]
pub(crate) fn name(_pid: i32) -> Result<OsString, String> {
    Err("this platform is not supported".into())
}
