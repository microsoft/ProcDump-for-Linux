use crate::engine::CancellationToken;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Condvar, Mutex, MutexGuard};
use std::time::Duration;

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum WaitOutcome {
    Started,
    TimedOut,
    Quit,
}

#[derive(Debug, Default)]
struct ControlState {
    started: bool,
    quit: bool,
}

#[derive(Debug, Default)]
pub struct MonitorControl {
    state: Mutex<ControlState>,
    changed: Condvar,
    cancellation: CancellationToken,
}

impl MonitorControl {
    pub fn new() -> Self {
        Self::default()
    }

    pub fn start(&self) {
        let mut state = self.lock_state();
        state.started = true;
        self.changed.notify_all();
    }

    pub fn request_quit(&self) {
        self.cancellation.cancel();
        let mut state = self.lock_state();
        state.quit = true;
        self.changed.notify_all();
    }

    pub fn is_quit_requested(&self) -> bool {
        self.cancellation.is_cancelled()
    }

    pub(crate) fn cancellation_token(&self) -> CancellationToken {
        self.cancellation.clone()
    }

    pub fn wait_for_start(&self) -> WaitOutcome {
        let mut state = self.lock_state();
        while !state.started && !state.quit {
            state = self.changed.wait(state).unwrap_or_else(|poisoned| {
                self.cancellation.cancel();
                poisoned.into_inner()
            });
        }
        if state.quit {
            WaitOutcome::Quit
        } else {
            WaitOutcome::Started
        }
    }

    pub fn wait(&self, duration: Duration) -> WaitOutcome {
        let state = self.lock_state();
        if state.quit {
            return WaitOutcome::Quit;
        }
        let (state, _) = self
            .changed
            .wait_timeout_while(state, duration, |state| !state.quit)
            .unwrap_or_else(|poisoned| {
                self.cancellation.cancel();
                poisoned.into_inner()
            });
        if state.quit {
            WaitOutcome::Quit
        } else {
            WaitOutcome::TimedOut
        }
    }

    fn lock_state(&self) -> MutexGuard<'_, ControlState> {
        self.state.lock().unwrap_or_else(|poisoned| {
            self.cancellation.cancel();
            poisoned.into_inner()
        })
    }
}

#[derive(Debug, Default)]
pub struct DumpGate {
    active: Mutex<bool>,
    available: Condvar,
    poisoned: AtomicBool,
}

impl DumpGate {
    pub fn new() -> Self {
        Self::default()
    }

    pub fn acquire<'a>(&'a self, control: &MonitorControl) -> Option<DumpPermit<'a>> {
        if self.poisoned.load(Ordering::Acquire) {
            control.request_quit();
            return None;
        }
        let mut active = self.active.lock().unwrap_or_else(|poisoned| {
            self.poisoned.store(true, Ordering::Release);
            control.request_quit();
            poisoned.into_inner()
        });
        while *active {
            if control.is_quit_requested() {
                return None;
            }
            let (next, _) = self
                .available
                .wait_timeout(active, Duration::from_millis(50))
                .unwrap_or_else(|poisoned| {
                    self.poisoned.store(true, Ordering::Release);
                    control.request_quit();
                    poisoned.into_inner()
                });
            active = next;
        }
        if control.is_quit_requested() {
            return None;
        }
        *active = true;
        Some(DumpPermit { gate: self })
    }
}

#[derive(Debug)]
pub struct DumpPermit<'a> {
    gate: &'a DumpGate,
}

impl Drop for DumpPermit<'_> {
    fn drop(&mut self) {
        let mut active = self.gate.active.lock().unwrap_or_else(|poisoned| {
            self.gate.poisoned.store(true, Ordering::Release);
            poisoned.into_inner()
        });
        *active = false;
        self.gate.available.notify_one();
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::sync::Arc;
    use std::thread;
    use std::time::Instant;

    #[test]
    fn start_releases_all_waiting_monitors() {
        let control = Arc::new(MonitorControl::new());
        let waiters: Vec<_> = (0..3)
            .map(|_| {
                let control = Arc::clone(&control);
                thread::spawn(move || control.wait_for_start())
            })
            .collect();

        control.start();
        for waiter in waiters {
            assert_eq!(waiter.join().unwrap(), WaitOutcome::Started);
        }
    }

    #[test]
    fn quit_interrupts_timed_wait() {
        let control = Arc::new(MonitorControl::new());
        let waiting = Arc::clone(&control);
        let started = Instant::now();
        let waiter = thread::spawn(move || waiting.wait(Duration::from_secs(30)));

        control.request_quit();
        assert_eq!(waiter.join().unwrap(), WaitOutcome::Quit);
        assert!(started.elapsed() < Duration::from_secs(1));
    }

    #[test]
    fn dump_gate_serializes_callers() {
        let gate = DumpGate::new();
        let control = MonitorControl::new();
        let first = gate.acquire(&control).unwrap();
        let before = Instant::now();
        drop(first);
        let second = gate.acquire(&control).unwrap();

        assert!(before.elapsed() < Duration::from_secs(1));
        drop(second);
    }

    #[test]
    fn dump_gate_honors_quit() {
        let gate = DumpGate::new();
        let control = MonitorControl::new();
        control.request_quit();

        assert!(gate.acquire(&control).is_none());
    }
}
