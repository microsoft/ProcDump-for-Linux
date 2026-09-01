# ProcDump Rust API

This package provides the supported Rust API for ProcDump.

The default feature set supports immediate native and .NET dump generation.
Additional capabilities are additive:

* `monitor`: CPU, memory, thread, file descriptor, signal, and timer triggers
* `dotnet-triggers`: exception, GC, and performance-counter triggers
* `restrack`: eBPF allocation tracking and leak reports
* `full`: all capabilities used by the ProcDump CLI

The safe on-demand entry point returns the actual generated dump path:

```no_run
use procdump::{WriteDumpOptions, write_dump};

let path = write_dump(
    1234,
    "/tmp/my-process.core",
    WriteDumpOptions::default().overwrite(true),
)?;
println!("{}", path.display());
# Ok::<(), procdump::WriteDumpError>(())
```

Generating or monitoring process dumps still requires the operating-system
permissions needed to inspect the target process.