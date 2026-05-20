//! Shellcode Generator Library
//! 
//! Generates x86_64 Linux shellcode from high-level commands,
//! using an abstract syscall interface that can be stubbed for evasion.

pub mod syscall;
pub mod syscall_interface;
pub mod cmd_encoder;

use crate::cmd_encoder::{CommandEncoder, EncodedCommand};
use crate::syscall_interface::{SyscallRegistry, SyscallInterface};
use crate::syscall::StubModuleInfo;

/// Shellcode generator with configurable module list
pub struct ShellcodeGenerator {
    encoder: CommandEncoder,
    loaded_modules: Vec<StubModuleInfo>,  // List of currently active modules
    direct_fallback_count: usize,  // Track fallback usage
}

impl ShellcodeGenerator {
    /// Create a new shellcode generator with default (direct) syscalls
    pub fn new() -> Self {
        let registry = SyscallRegistry::new();
        ShellcodeGenerator {
            encoder: CommandEncoder::with_registry(registry),
            loaded_modules: Vec::new(),
            direct_fallback_count: 0,
        }
    }
    
    /// Create a new shellcode generator with custom syscall registry
    pub fn with_registry(registry: SyscallRegistry) -> Self {
        ShellcodeGenerator {
            encoder: CommandEncoder::with_registry(registry),
            loaded_modules: Vec::new(),
            direct_fallback_count: 0,
        }
    }
    
    /// Add a stub module to the list of loaded modules (for stub selection)
    pub fn add_module(&mut self, name: &str, syscalls: &[u32], version: u8) {
        let info = StubModuleInfo::new(name, syscalls, version);
        self.loaded_modules.push(info);
    }
    
    /// Set the list of loaded modules at once
    pub fn set_loaded_modules(&mut self, modules: &[&str]) {
        for m in modules.iter() {
            // Default stubbed syscalls (mprotect=5, mmap=9, prlimit64=86)
            let syscalls = match *m {
                "stub_mprotect" => &[5u32],
                "stub_mmap" => &[9u32],
                "stub_prlimit64" => &[86u32],
                _ => continue,  // Skip unknown modules
            };
            let info = StubModuleInfo::new(m, syscalls, 1);
            self.loaded_modules.push(info);
        }
    }
    
    /// Register a syscall stub module dynamically
    pub fn register_stub(&mut self, name: &str, syscalls: &[u32], version: u8) {
        let info = StubModuleInfo::new(name, syscalls, version);
        self.loaded_modules.push(info);
    }
    
    /// Generate shellcode for a command string
    /// Returns hex-encoded shellcode or an error
    pub fn generate_shellcode(&mut self, cmd_str: &str) -> Result<String, String> {
        let encoded = self.encoder.encode(cmd_str)
            .map_err(|e| format!("Failed to parse command: {}", e))?;
        
        // Convert bytes to hex string for output
        let shellcode_hex: Vec<String> = encoded.shellcode.iter()
            .map(|b| format!("{:02x}", b))
            .collect();
        
        Ok(shellcode_hex.join(""))
    }
    
    /// Generate multiple commands' shellcode at once
    pub fn generate_multiple(&mut self, commands: &[&str]) -> Result<Vec<String>, String> {
        let mut results = Vec::new();
        
        for cmd in commands {
            let hex = self.generate_shellcode(cmd)?;
            results.push(hex);
        }
        
        Ok(results)
    }
    
    /// Generate shellcode and return the EncodedCommand for inspection
    pub fn generate_with_details(&mut self, cmd_str: &str) -> Result<EncodedCommand, String> {
        let encoded = self.encoder.encode(cmd_str)?;
        Ok(encoded)
    }
    
    /// Get statistics about generated shellcode
    pub fn get_stats(&self) -> ShellcodeStats {
        let total_modules = self.loaded_modules.len();
        let stubbed_syscalls = self.loaded_modules.iter()
            .filter(|m| m.name.starts_with("stub_"))
            .count();
        
        ShellcodeStats {
            loaded_modules: total_modules,
            stubbed_syscalls: stubbed_syscalls,
            direct_fallbacks: self.direct_fallback_count,
        }
    }
    
    /// Get the current syscall registry for inspection (for debugging)
    pub fn get_registry(&self) -> &SyscallRegistry {
        // Access internal registry via getter method in CommandEncoder
        todo!("Registry access needs restructuring")
    }
}

/// Statistics about shellcode generation
#[derive(Debug)]
pub struct ShellcodeStats {
    pub loaded_modules: usize,
    pub stubbed_syscalls: usize,
    pub direct_fallbacks: usize,
}
