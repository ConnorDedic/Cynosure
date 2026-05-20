//! Abstract syscall interface for shellcode generation.
//! 
//! Provides a trait-based design where each syscall can be:
//! - Stubb (via stub modules) for evasion
//! - Direct fallback when no stub available

use crate::syscall::{Syscall, SyscallStub};

/// Trait defining the syscall abstraction layer
pub trait SyscallInterface {
    /// Generate shellcode bytes for a given syscall number
    fn generate(&self, num: u32) -> Vec<u8>;
    
    /// Check if this syscall is stubbed (evasion active)
    fn is_stubbed(&self) -> bool;
}

/// Default implementation using direct syscalls (no evasion)
#[derive(Clone)]
pub struct DirectSyscall {
    pub number: u32,
}

impl SyscallInterface for DirectSyscall {
    fn generate(&self, _num: u32) -> Vec<u8> {
        // x86_64 Linux direct syscall stub
        // mov rax, <NR>; syscall
        let mut bytes = vec![0x48]; // 64-bit prefix
        bytes.push(0xb8);           // mov rax, imm32
        
        let num_low = (self.number & 0xFFFF) as u8;
        let num_high = ((self.number >> 16) & 0xFFFF) as u8;
        
        bytes.push(num_low);
        bytes.push(0xc3);           // ret (placeholder for syscall return)
        
        bytes
    }
    
    fn is_stubbed(&self) -> bool {
        false
    }
}

/// Stub syscall implementation with potential evasion hooks
#[derive(Clone)]
pub struct StubbSyscall {
    pub number: u32,
    pub stub: Box<dyn SyscallStub>,
}

impl SyscallInterface for StubbSyscall {
    fn generate(&self, _num: u32) -> Vec<u8> {
        // Stubbed syscall with potential interference hooks
        let mut bytes = vec![0x48]; // 64-bit prefix
        
        if let Some(stub_bytes) = self.stub.generate() {
            bytes.extend_from_slice(&stub_bytes);
        } else {
            // Fallback to direct call if stub generation fails
            bytes.push(0xb8);
            let num_low = (self.number & 0xFFFF) as u8;
            bytes.push(num_low);
            bytes.push(0xc3);
        }
        
        bytes
    }
    
    fn is_stubbed(&self) -> bool {
        true
    }
}

/// Registry for syscall implementations (direct or stubbed)
pub struct SyscallRegistry {
    direct: Vec<DirectSyscall>,
    stubbed: Vec<StubbSyscall>,
}

impl SyscallRegistry {
    pub fn new() -> Self {
        // Register common Linux x86_64 syscalls
        let common = vec![
            0,   // read
            1,   // write
            2,   // openat
            3,   // close
            5,   // mprotect (evasion: prevent DEP)
            9,   // mmap
            22,  // ioctl
            86,  // prlimit64
        ];
        
        let mut registry = SyscallRegistry {
            direct: common.iter().map(|&n| DirectSyscall { number: n as u32 }).collect(),
            stubbed: Vec::new(),
        };
        
        // Load any available stub modules
        if let Some(stubs) = crate::load_syscall_stubs() {
            registry.stubbed.extend(stubs.into_iter().map(|s| StubbSyscall {
                number: s.number,
                stub: Box::new(s),
            }));
        }
        
        registry
    }
    
    /// Get syscall implementation (stubbed if available, else direct)
    pub fn get(&self, num: u32) -> &dyn SyscallInterface {
        // Check stubbed first
        for s in &self.stubbed {
            if s.number == num {
                return s;
            }
        }
        
        // Fallback to direct
        for d in &self.direct {
            if d.number == num {
                return d;
            }
        }
        
        // Unknown syscall - use generic fallback
        return &DirectSyscall { number: num };
    }
    
    /// Generate shellcode for a syscall, preferring stubbed versions
    pub fn generate_shellcode(&self, num: u32) -> Vec<u8> {
        let impl_ = self.get(num);
        impl_.generate(num)
    }
}
