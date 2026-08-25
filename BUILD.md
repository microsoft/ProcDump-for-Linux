# Build ProcDump

ProcDump is a Cargo workspace supporting Linux and macOS.

## Toolchain

Install stable Rust with `rustfmt` and `clippy`:

```bash
curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh
rustup component add rustfmt clippy
```

## Linux prerequisites

Ubuntu/Debian:

```bash
sudo apt update
sudo apt install -y build-essential clang pkg-config libelf-dev zlib1g-dev gdb
```

The complete integration suite also requires a .NET SDK/runtime and passwordless
or interactive `sudo` access. Cargo builds libbpf and generates the eBPF skeleton;
`bpftool` is useful for diagnostics but is not part of the userspace build.

## macOS prerequisites

Install Xcode command-line tools, Rust, and a working `gdb`/`gcore` installation:

```bash
xcode-select --install
```

## Build and test

Run from the repository root:

```bash
cargo build --workspace
cargo fmt --all --check
cargo clippy --workspace --all-targets -- -D warnings
cargo test --workspace
```

Release artifacts:

```bash
cargo build --release --bin procdump
cargo build --release -p procdump-capi
```

This produces:

* `target/release/procdump`
* `target/release/libprocdump.a`
* Public C header: `crates/procdump-capi/include/ProcDumpLib.h`

## Integration tests

Cargo stages the release binary, static library, native fixtures, and unchanged
shell scenarios before running them:

```bash
cargo xtask stage-tests
cargo xtask test-integration high_cpu
cargo xtask test-integration
```

Run Cargo as your normal user. `xtask` elevates only the staged compatibility
runner when required.

On Linux the runner selects `tests/integration/scenarios`; on macOS it selects
`tests/integration/scenarios_mac`.

## Cross-target checks

The supported Rust code can be checked for the other platform without executing
its scenarios:

```bash
rustup target add x86_64-unknown-linux-gnu aarch64-apple-darwin x86_64-apple-darwin
cargo clippy -p procdump-core --target x86_64-unknown-linux-gnu -- -D warnings
cargo clippy --workspace --target aarch64-apple-darwin -- -D warnings
cargo clippy --workspace --target x86_64-apple-darwin -- -D warnings
```

Native integration scenarios still need to run on the corresponding operating
system and architecture.
