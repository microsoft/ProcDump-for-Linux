use procdump::{WriteDumpError, WriteDumpOptions, write_dump};
use std::path::Path;

#[test]
fn invalid_pid_returns_typed_error_with_compatible_text() {
    let error = write_dump(0, "/tmp/core", WriteDumpOptions::default()).unwrap_err();

    assert!(matches!(error, WriteDumpError::InvalidArgument));
    assert_eq!(
        error.to_string(),
        "Invalid argument: a valid processId and dumpPath are required."
    );
}

#[test]
fn invalid_directory_is_reported_before_process_inspection() {
    let directory =
        std::env::temp_dir().join(format!("procdump-missing-directory-{}", std::process::id()));
    let path = directory.join("core");
    let error = write_dump(
        std::process::id() as i32,
        &path,
        WriteDumpOptions::default(),
    )
    .unwrap_err();

    assert!(matches!(error, WriteDumpError::InvalidDirectory(ref value) if value == &directory));
}

#[test]
fn options_are_composable_without_exposing_fields() {
    let options = WriteDumpOptions::default()
        .overwrite(true)
        .core_dump_mask(0x7f)
        .use_gcore(true);

    assert!(!format!("{options:?}").is_empty());
    assert!(Path::new("/tmp/core").is_absolute());
}
