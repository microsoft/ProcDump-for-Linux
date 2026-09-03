# CLI compatibility fixtures

These fixtures preserve the command-line contract from the final C/C++
implementation at the `legacy-cpp-final` Git tag.

The test suite enforces:

* complete Linux and macOS banner/help output byte-for-byte;
* stdout, stderr, and exit status for no arguments, `-?`, and `/?`;
* every accepted dash and slash switch spelling, including case-insensitive
  parsing and dash-only `-usegcore`;
* fixed-width configuration summaries for default, advanced, exception,
  signal, and macOS configurations;
* every informational runtime message after substituting representative values;
* the timestamp and log-level prefix with a fixed timestamp.

`@VERSION@` represents the build-supplied product version. `@EOL@` and
`@OPTION_INDENT@` make otherwise invisible final-newline and indentation bytes
explicit in fixtures.

Values that necessarily vary between runs, including timestamps, process IDs,
paths, counters, metric values, and operating-system error text, are tested at
their pure formatting boundary with fixed representative values. Extended
`-log` debug traces are implementation diagnostics and are excluded because
their C/C++ source files and line numbers do not exist in the Rust port.

Changing a fixture requires an intentional CLI compatibility decision.