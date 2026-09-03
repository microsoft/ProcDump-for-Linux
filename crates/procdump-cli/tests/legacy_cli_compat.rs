use std::process::{Command, Output};

fn expected_help() -> Vec<u8> {
    let fixture = if cfg!(target_os = "macos") {
        include_str!("../../../tests/cli-compat/legacy-macos-help.txt")
    } else {
        include_str!("../../../tests/cli-compat/legacy-linux-help.txt")
    };
    fixture
        .replace(
            "@VERSION@",
            option_env!("PROCDUMP_VERSION").unwrap_or(env!("CARGO_PKG_VERSION")),
        )
        .replace("@OPTION_INDENT@", "   ")
        .replace("@EOL@", "\n")
        .into_bytes()
}

fn run(arguments: &[&str]) -> Output {
    Command::new(env!("CARGO_BIN_EXE_procdump"))
        .args(arguments)
        .output()
        .unwrap()
}

fn assert_legacy_help(output: Output) {
    assert_eq!(output.status.code(), Some(255));
    assert_eq!(output.stdout, expected_help());
    assert_eq!(output.stderr, b"");
}

fn normalize_error_timestamp(output: &[u8]) -> Vec<u8> {
    let mut output = output.to_vec();
    if let Some(window) = output.windows(19).position(|window| {
        window[0] == b'['
            && window[3] == b':'
            && window[6] == b':'
            && &window[9..19] == b" - ERROR]:"
    }) {
        output[window + 1..window + 9].copy_from_slice(b"TIMEHERE");
    }
    output
}

#[test]
fn no_arguments_match_legacy_output_byte_for_byte() {
    assert_legacy_help(run(&[]));
}

#[test]
fn dash_question_mark_matches_legacy_output_byte_for_byte() {
    assert_legacy_help(run(&["-?"]));
}

#[test]
fn slash_question_mark_matches_legacy_output_byte_for_byte() {
    assert_legacy_help(run(&["/?"]));
}

#[test]
fn missing_option_value_matches_legacy_output_byte_for_byte() {
    assert_legacy_help(run(&["-n"]));
}

#[test]
fn semantic_error_matches_legacy_except_for_timestamp_digits() {
    let output = run(&["-log", "invalid", "42"]);
    let help = expected_help();
    let usage = help
        .windows(b"\nCapture Usage: ".len())
        .position(|window| window == b"\nCapture Usage: ")
        .unwrap();
    let mut expected = help[..usage].to_vec();
    expected.extend_from_slice(b"[TIMEHERE - ERROR]: Invalid diagnostics stream specified.\n");
    expected.extend_from_slice(&help[usage..]);

    assert_eq!(output.status.code(), Some(255));
    assert_eq!(normalize_error_timestamp(&output.stdout), expected);
    assert_eq!(output.stderr, b"");
}

#[cfg(all(
    target_os = "linux",
    any(target_arch = "x86_64", target_arch = "aarch64")
))]
#[test]
fn default_corex_invocation_does_not_require_gcore_on_path() {
    let output = Command::new(env!("CARGO_BIN_EXE_procdump"))
        .arg(i32::MAX.to_string())
        .env("PATH", "")
        .output()
        .unwrap();
    let stdout = String::from_utf8_lossy(&output.stdout);

    assert_eq!(output.status.code(), Some(255));
    assert!(!stdout.contains("failed to locate gcore"), "{stdout}");
}
