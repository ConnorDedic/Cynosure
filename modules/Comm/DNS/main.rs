/*
 * DNS Communication Module - Rust Wrapper
 * Exports C functions as DLL interface
 */

extern "C" {
    pub fn dns_comm_init(c2_domain: *const u8, server_ip: *const u8) -> i32;
    pub fn dns_send_data(data: *const u8, len: usize) -> i32;
    pub fn dns_recv_command(buffer: *mut u8, buffer_size: usize) -> i32;
    pub fn dns_comm_status() -> *const u8;
    pub fn dns_comm_test() -> i32;
}

#[no_mangle]
pub extern "C" fn dns_comm_init_wrapper(c2_domain: *const u8, server_ip: *const u8) -> i32 {
    unsafe { dns_comm_init(c2_domain, server_ip) }
}

#[no_mangle]
pub extern "C" fn dns_send_data_wrapper(data: *const u8, len: usize) -> i32 {
    unsafe { dns_send_data(data, len) }
}

#[no_mangle]
pub extern "C" fn dns_recv_command_wrapper(buffer: *mut u8, buffer_size: usize) -> i32 {
    unsafe { dns_recv_command(buffer, buffer_size) }
}

#[no_mangle]
pub extern "C" fn dns_comm_status_wrapper() -> *const u8 {
    unsafe { dns_comm_status() }
}

#[no_mangle]
pub extern "C" fn dns_comm_test_wrapper() -> i32 {
    unsafe { dns_comm_test() }
}
