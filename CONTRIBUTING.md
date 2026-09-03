# Contributing

Before submitting a pull request, sign the Microsoft Contributor License
Agreement. The automated CLA check will guide you through the process once.

Keep pull requests focused, link the relevant issue, avoid unrelated formatting,
and add tests for behavior changes.

## Development workflow

Build and validate from the repository root:

```bash
cargo fmt --all --check
cargo clippy --workspace --all-targets -- -D warnings
cargo test --workspace
```

For behavior changes, run the narrowest unchanged integration scenario first:

```bash
cargo xtask test-integration high_cpu
```

Before merge, run the complete platform suite where possible:

```bash
cargo xtask test-integration
```

See [BUILD.md](BUILD.md) for prerequisites and cross-target commands.

## Repository ownership

* Shared behavior and the supported Rust API belong in `crates/procdump`.
* Platform-specific process access belongs behind the `ProcessDiscovery` and
  `ProcessMetrics` traits.
* Platform-specific dump generation belongs behind `DumpBackend`.
* The CLI belongs in `crates/procdump-cli`.
* The static C ABI belongs in `crates/procdump-capi`.
* Linux eBPF and the injected CLR profiler are optional native components under
  `crates/procdump/native/`; userspace orchestration remains Rust.

Preserve the existing one-thread-per-trigger monitor architecture unless a design
change is explicitly approved.

## Rust style

* Use rustfmt rather than manual formatting conventions.
* Keep unsafe code in small, auditable platform or FFI boundaries.
* Prefer typed errors and RAII cleanup for resources and process state.
* Use whole, descriptive names and avoid unnecessary abstractions.
* Keep platform conditionals close to the owning implementation.

## Integration tests

Compatibility scenarios live in `tests/integration/scenarios` on Linux and
`tests/integration/scenarios_mac` on macOS. A scenario returns zero on success and
nonzero on failure.

The C and C++ files under `tests/integration` are fixtures and ABI consumers, not
alternative ProcDump implementations. Changes to the C ABI should update
`crates/procdump-capi/include/ProcDumpLib.h` and the corresponding scenarios.
