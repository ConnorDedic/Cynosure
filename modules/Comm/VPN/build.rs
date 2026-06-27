fn main() {
    // Determine target to apply platform-specific flags
    let target = std::env::var("TARGET").unwrap_or_default();
    let is_windows = target.contains("windows") || cfg!(target_os = "windows");

    let mut builder = cc::Build::new();
    builder
        .file("vpn_comm.c")
        .include("../../../src/implant")  // Include EDR headers
        .opt_level(2);

    // On Windows targets, ensure symbols are exported
    if is_windows {
        // MinGW-specific: ensure all symbols are exported by default
        builder.flag("-fvisibility=default");
    }

    builder.compile("vpn_comm");

    // Print linker flags for Windows
    if is_windows {
        println!("cargo:rustc-link-arg=-Wl,--export-all-symbols");
        println!("cargo:rustc-link-arg=-Wl,--subsystem,windows");
    }
}
