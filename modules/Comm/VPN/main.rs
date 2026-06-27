// VPN Communication Module for EDR Agent
//
// This crate compiles vpn_comm.c into a shared library (.so/.dll)
// that the EDR agent loads as a plugin at runtime.

#![crate_type = "cdylib"]

// The C implementation in vpn_comm.c exports edr_plugin_entry
// It's compiled and linked by build.rs and re-exported here

// Import the C function (with different internal name to avoid conflict)
extern "C" {
    #[link_name = "edr_plugin_entry"]
    fn _c_edr_plugin_entry(out_manifest: *mut std::ffi::c_void) -> i32;
}

// Re-export with #[no_mangle] as the main entry point
// This ensures the symbol is visible in the DLL
#[no_mangle]
pub extern "C" fn edr_plugin_entry(out_manifest: *mut std::ffi::c_void) -> i32 {
    unsafe { _c_edr_plugin_entry(out_manifest) }
}
