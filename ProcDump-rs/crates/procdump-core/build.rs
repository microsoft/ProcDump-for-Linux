use std::env;
use std::path::PathBuf;
use std::process::Command;

fn main() {
    println!("cargo:rerun-if-env-changed=CXX");
    let target = env::var("TARGET").expect("Cargo must set TARGET");
    if !target.contains("linux") {
        return;
    }

    let manifest = PathBuf::from(env::var_os("CARGO_MANIFEST_DIR").unwrap());
    let workspace = manifest.ancestors().nth(2).unwrap();
    let profiler = workspace.join("native/profiler");
    let output = PathBuf::from(env::var_os("OUT_DIR").unwrap()).join("procdumpprofiler.so");
    let out_dir = PathBuf::from(env::var_os("OUT_DIR").unwrap());
    let sources = [
        "ClassFactory.cpp",
        "ProcDumpProfiler.cpp",
        "dllmain.cpp",
        "corprof_i.cpp",
        "easylogging++.cc",
    ];

    println!("cargo:rerun-if-changed={}", profiler.display());
    let compiler = env::var_os("CXX").unwrap_or_else(|| "clang++".into());
    let mut command = Command::new(compiler);
    command.arg("-shared");
    for source in sources {
        command.arg(profiler.join("src").join(source));
    }
    command
        .arg("-o")
        .arg(&output)
        .arg("-I")
        .arg(profiler.join("inc"))
        .args([
            "-DELPP_NO_DEFAULT_LOG_FILE",
            "-DELPP_THREAD_SAFE",
            "-g",
            "-pthread",
            "-Wno-pragma-pack",
            "-Wno-pointer-arith",
            "-Wno-conversion-null",
            "-Wno-write-strings",
            "-Wno-format-security",
            "-Wno-null-arithmetic",
            "-fPIC",
            "-fms-extensions",
            "-DPAL_STDCPP_COMPAT",
            "-DPLATFORM_UNIX",
            "-std=c++11",
        ]);
    add_architecture_flags(&mut command, &target);

    let status = command.status().expect("failed to start profiler compiler");
    assert!(
        status.success(),
        "failed to build retained ProcDump profiler"
    );

    let ebpf = workspace.join("native/ebpf");
    println!("cargo:rerun-if-changed={}", ebpf.display());
    let (target_arch, multiarch_include) = if target.starts_with("x86_64") {
        ("-D__TARGET_ARCH_x86", "/usr/include/x86_64-linux-gnu")
    } else if target.starts_with("aarch64") {
        ("-D__TARGET_ARCH_arm64", "/usr/include/aarch64-linux-gnu")
    } else {
        panic!("the retained eBPF program supports only x86_64 and aarch64");
    };
    libbpf_cargo::SkeletonBuilder::new()
        .source(ebpf.join("procdump_ebpf.bpf.c"))
        .clang_args([
            format!("-I{}", ebpf.display()),
            target_arch.into(),
            format!("-I{multiarch_include}"),
            "-D__KERNEL__".into(),
            "-D__BPF_TRACING__".into(),
            "-D__linux__".into(),
            "-Wno-unused-value".into(),
            "-Wno-pointer-sign".into(),
            "-Wno-compare-distinct-pointer-types".into(),
            "-Wno-address-of-packed-member".into(),
            "-Wno-unknown-warning-option".into(),
        ])
        .build_and_generate(out_dir.join("procdump_ebpf.skel.rs"))
        .expect("failed to build retained ProcDump eBPF program");
}

fn add_architecture_flags(command: &mut Command, target: &str) {
    if target.starts_with("x86_64") {
        command.args(["-DHOST_AMD64", "-DHOST_64BIT"]);
    } else if target.starts_with("aarch64") {
        command.args(["-DHOST_ARM64", "-DHOST_64BIT"]);
    } else if target.starts_with("i686") || target.starts_with("i586") {
        command.arg("-DHOST_X86");
    } else if target.starts_with("arm") {
        command.arg("-DHOST_ARM");
    } else {
        panic!("the retained .NET profiler does not support target {target}");
    }
}
