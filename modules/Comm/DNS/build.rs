fn main() {
    // Compile dns_comm.c for Windows
    cc::Build::new()
        .file("dns_comm.c")
        .define("_WIN32", None)
        .flag("-O2")
        .flag("-s")  // Strip symbols
        .compile("dns_comm");

    // Link Windows DNS libraries
    println!("cargo:rustc-link-lib=ws2_32");
    println!("cargo:rustc-link-lib=dnsapi");
    println!("cargo:rustc-link-lib=iphlpapi");
}
