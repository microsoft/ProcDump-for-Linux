# ProcDump-rs

`ProcDump-rs` is the Cargo-only Rust implementation of ProcDump for Linux and
macOS. The legacy integration scenarios remain the compatibility contract and
are copied into a Cargo staging directory without modification.

Platform process discovery and metrics are implemented behind shared Rust
interfaces, with procfs on Linux and libproc/Mach on macOS. Dump generation is
also behind a shared backend interface:

- Linux native processes use the Rust corex ELF writer by default.
- Linux managed processes use the .NET diagnostics IPC dump protocol.
- macOS and the explicit Linux `-usegcore` fallback use `gcore`.

The Linux eBPF kernel program and injected CLR profiler remain native code and
are built by Cargo. Their userspace loading, monitoring, EventPipe handling,
reporting, orchestration, and dump writing are Rust.

## Build and unit tests

```bash
cargo build --workspace
cargo test --workspace
cargo clippy --workspace --all-targets -- -D warnings
```

On a non-macOS host, both supported Darwin targets can still be type-checked:

```bash
rustup target add aarch64-apple-darwin x86_64-apple-darwin
cargo clippy --workspace --all-targets --target aarch64-apple-darwin -- -D warnings
cargo clippy --workspace --all-targets --target x86_64-apple-darwin -- -D warnings
```

## Integration loop

Stage the release CLI, `libprocdump.a`, native test application, C API test
driver, and unchanged shell scenarios:

```bash
cargo xtask stage-tests
```

Run one scenario while implementing a behavior:

```bash
cargo xtask test-scenario ondemand
cargo xtask test-scenario high_cpu
cargo xtask test-scenario lib_api_basic_dump
```

Run the complete platform suite as your normal user. `xtask` builds and stages
with the user-local Rust toolchain, then prompts through `sudo` only for the
legacy runner:

```bash
cargo xtask test-integration
cargo xtask test-integration high_cpu
```

Do not prefix Cargo itself with `sudo`; root does not inherit rustup's
user-local Cargo path.

On Linux this selects `tests/integration/scenarios`; on macOS it selects
`tests/integration/scenarios_mac`. The original scripts, timeout behavior, GDB
content validation, dump-size comparison, and platform selection are retained.

## Native prerequisites

- Rust stable with `rustfmt` and `clippy`
- A C and C++ compiler for the existing integration fixtures
- `gcore` and `gdb` for compatibility validation and the explicit fallback
- .NET SDK/runtime for managed scenarios
- Root privileges for the complete legacy integration runner

The eBPF resource tracker additionally requires Clang, `pkg-config`, and the
libelf and zlib development packages. Cargo builds the libbpf userspace runtime
and generates the eBPF skeleton bindings.