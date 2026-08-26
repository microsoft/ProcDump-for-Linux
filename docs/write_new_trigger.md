# Adding a trigger

ProcDump preserves one operating-system thread per configured trigger. A new
trigger should fit that architecture and reuse the shared dump coordinator.

## 1. Extend configuration

Add the parsed representation to `crates/procdump/src/config.rs`:

* Parse both supported switch prefixes where appropriate.
* Validate platform capabilities and incompatible combinations.
* Add parser tests for valid, invalid, duplicate, and platform-specific forms.

Update the root README and man pages when the public command-line contract
changes.

## 2. Implement the monitor

Place shared monitor control in `crates/procdump/src/monitor.rs`. Put
platform-specific implementation details in a focused module.

A trigger thread should:

1. Wait on `MonitorControl::wait_for_start`.
2. Observe the configured polling interval or external event.
3. Validate the original `ProcessIdentity` to detect PID reuse.
4. Call `DumpCoordinator::write` with a distinct `DumpKind`.
5. Honor the snooze interval, dump count, and quit signal.
6. Return errors through `MonitorError` and release resources through RAII.

`DumpCoordinator` serializes dump generation and coordinates optional restrack
sidecars. Do not call a dump backend directly from a trigger.

## 3. Respect platform interfaces

Process discovery and metrics belong behind `ProcessDiscovery` and
`ProcessMetrics`. Dump generation belongs behind `DumpBackend`. Add a platform
capability in `config.rs` when a trigger cannot exist on every supported system.

## 4. Test the behavior

Add focused Rust unit tests for parsing, threshold behavior, and termination.
Then add or update an unchanged-style shell scenario under
`tests/integration/scenarios` or `tests/integration/scenarios_mac`.

Run:

```bash
cargo fmt --all --check
cargo clippy --workspace --all-targets -- -D warnings
cargo test --workspace
cargo xtask test-integration <scenario-name>
```

Broaden to the complete platform suite when the focused scenario passes.
