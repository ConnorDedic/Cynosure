//! Syscall stub traits and implementations for shellcode evasion.
//! 
//! Provides the foundational syscall abstraction that can be extended
//! by individual stub modules (e.g., prlimit64, mmap, mprotect).

/// Trait for syscall stub modules
pub trait SyscallStub: Send + Sync {
    /// Generate stub bytes to intercept/modify the syscall
    fn generate(&self) -> Option<Vec<u8>>;
    
    /// Get the syscall number this stub handles
    fn syscall_number(&self) -> u32;
}

/// Generic fallback syscall stub (no interception, just padding)
#[derive(Debug, Clone)]
pub struct GenericStub {
    pub syscall_num: u32,
    pub hook_code: Vec<u8>,
}

impl SyscallStub for GenericStub {
    fn generate(&self) -> Option<Vec<u8>> {
        if self.hook_code.is_empty() {
            None
        } else {
            Some(self.hook_code.clone())
        }
    }
    
    fn syscall_number(&self) -> u32 {
        self.syscall_num
    }
}

/// Syscall stub module info (for dynamic registration)
#[derive(Debug, Clone)]
pub struct StubModuleInfo {
    pub name: String,
    pub syscall_numbers: Vec<u32>,
    pub version: u8,
}

impl StubModuleInfo {
    /// Create a new stub module info entry
    pub fn new(name: &str, syscalls: &[u32], version: u8) -> Self {
        StubModuleInfo {
            name: name.to_string(),
            syscall_numbers: syscalls.iter().map(|&n| n).collect(),
            version,
        }
    }
    
    /// Check if this module handles a given syscall number
    pub fn handles_syscall(&self, num: u32) -> bool {
        self.syscall_numbers.contains(&num)
    }
}

/// Load syscall stub modules from the evasion/stub directory
pub fn load_syscall_stubs() -> Option<Vec<GenericStub>> {
    // This will be wired to modules/Evasion/Syscall/ in Cynosure
    None  // Placeholder - actual loading happens via module system
}

/// Register a syscall stub module dynamically
pub fn register_stub_module(name: &str, syscalls: &[u32], version: u8) -> StubModuleInfo {
    StubModuleInfo::new(name, syscalls, version)
}
