#![allow(unsafe_code)]

use crate::dump::DumpKind;
use crate::monitor::{DumpCoordinator, MonitorError};
use crate::process::ProcessIdentity;
use crate::sync::{MonitorControl, WaitOutcome};
use std::io;
use std::ptr;
use std::sync::Arc;
use std::thread::{self, JoinHandle};
use std::time::Duration;

pub(crate) fn spawn_signal_monitor(
    control: Arc<MonitorControl>,
    coordinator: Arc<DumpCoordinator>,
    identity: ProcessIdentity,
    signals: Vec<i32>,
) -> Result<JoinHandle<Result<(), MonitorError>>, MonitorError> {
    thread::Builder::new()
        .name("signal monitor".into())
        .spawn(move || {
            if control.wait_for_start() == WaitOutcome::Quit {
                return Ok(());
            }
            monitor_signals(&control, &coordinator, identity, &signals)
                .map_err(|error| MonitorError::Signal(error.to_string()))
        })
        .map_err(MonitorError::Spawn)
}

fn monitor_signals(
    control: &MonitorControl,
    coordinator: &DumpCoordinator,
    identity: ProcessIdentity,
    signals: &[i32],
) -> Result<(), SignalError> {
    let pid = identity.pid.get();
    let mut attachment = PtraceAttachment::seize(pid)?;
    while !control.is_quit_requested() {
        let mut status = 0;
        let waited = loop {
            if control.is_quit_requested() {
                return Ok(());
            }
            let result = unsafe { libc::waitpid(pid, &raw mut status, libc::WNOHANG) };
            if result == -1 && io::Error::last_os_error().kind() == io::ErrorKind::Interrupted {
                continue;
            }
            if result == 0 {
                thread::sleep(Duration::from_millis(25));
                continue;
            }
            break result;
        };
        if waited == -1 {
            let error = io::Error::last_os_error();
            if matches!(error.raw_os_error(), Some(libc::ECHILD | libc::ESRCH)) {
                return Ok(());
            }
            return Err(SignalError::Wait(error));
        }
        if libc::WIFEXITED(status) || libc::WIFSIGNALED(status) {
            attachment.disarm();
            return Ok(());
        }
        if !libc::WIFSTOPPED(status) {
            continue;
        }

        let signal = libc::WSTOPSIG(status);
        if signals.contains(&signal) {
            attachment.detach(libc::SIGSTOP)?;
            crate::diagnostics::info(
                coordinator.diagnostics,
                crate::cli_output::signal_trigger(signal, pid),
            );
            let dump_result = coordinator
                .write(DumpKind::Signal)
                .map_err(|error| SignalError::Dump(error.to_string()));
            let resume_result = send_signal(pid, libc::SIGCONT);
            let delivery_result = send_signal(pid, signal);
            dump_result?;
            resume_result?;
            delivery_result?;
            if coordinator.limit_reached() || control.is_quit_requested() {
                return Ok(());
            }
            attachment = PtraceAttachment::seize(pid)?;
            continue;
        }

        ptrace_continue(pid, signal)?;
    }
    Ok(())
}

struct PtraceAttachment {
    pid: i32,
    attached: bool,
}

impl PtraceAttachment {
    fn seize(pid: i32) -> Result<Self, SignalError> {
        let result = unsafe {
            libc::ptrace(
                libc::PTRACE_SEIZE,
                pid,
                ptr::null_mut::<libc::c_void>(),
                ptr::null_mut::<libc::c_void>(),
            )
        };
        if result == -1 {
            Err(SignalError::Seize(io::Error::last_os_error()))
        } else {
            Ok(Self {
                pid,
                attached: true,
            })
        }
    }

    fn detach(&mut self, signal: i32) -> Result<(), SignalError> {
        let result = unsafe {
            libc::ptrace(
                libc::PTRACE_DETACH,
                self.pid,
                ptr::null_mut::<libc::c_void>(),
                signal as usize as *mut libc::c_void,
            )
        };
        if result == -1 {
            Err(SignalError::Detach(io::Error::last_os_error()))
        } else {
            self.attached = false;
            Ok(())
        }
    }

    fn disarm(&mut self) {
        self.attached = false;
    }
}

impl Drop for PtraceAttachment {
    fn drop(&mut self) {
        if self.attached {
            let result = unsafe {
                libc::ptrace(
                    libc::PTRACE_DETACH,
                    self.pid,
                    ptr::null_mut::<libc::c_void>(),
                    ptr::null_mut::<libc::c_void>(),
                )
            };
            if result == -1 {
                eprintln!(
                    "warning: failed to detach process {} during cleanup: {}",
                    self.pid,
                    io::Error::last_os_error()
                );
            } else {
                self.attached = false;
            }
        }
    }
}

fn ptrace_continue(pid: i32, signal: i32) -> Result<(), SignalError> {
    let result = unsafe {
        libc::ptrace(
            libc::PTRACE_CONT,
            pid,
            ptr::null_mut::<libc::c_void>(),
            signal as usize as *mut libc::c_void,
        )
    };
    if result == -1 {
        Err(SignalError::Continue(io::Error::last_os_error()))
    } else {
        Ok(())
    }
}

fn send_signal(pid: i32, signal: i32) -> Result<(), SignalError> {
    if unsafe { libc::kill(pid, signal) } == -1 {
        Err(SignalError::Deliver(io::Error::last_os_error()))
    } else {
        Ok(())
    }
}

#[derive(Debug)]
enum SignalError {
    Seize(io::Error),
    Wait(io::Error),
    Detach(io::Error),
    Continue(io::Error),
    Deliver(io::Error),
    Dump(String),
}

impl std::fmt::Display for SignalError {
    fn fmt(&self, formatter: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            Self::Seize(error) => write!(formatter, "unable to seize target process: {error}"),
            Self::Wait(error) => write!(formatter, "failed waiting for target signal: {error}"),
            Self::Detach(error) => write!(formatter, "failed to detach target process: {error}"),
            Self::Continue(error) => {
                write!(formatter, "failed to continue target process: {error}")
            }
            Self::Deliver(error) => write!(formatter, "failed to deliver target signal: {error}"),
            Self::Dump(error) => formatter.write_str(error),
        }
    }
}

impl std::error::Error for SignalError {}
