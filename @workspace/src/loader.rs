use std::path::{Path, PathBuf};

#[derive(Debug)]
pub struct ModuleDefinition {
    pub id: String,
    pub name: String,
    pub path: PathBuf,
}

/// Check if a directory contains Rust source files.
fn has_rust_files(path: &Path) -> bool {
    std::fs::read_dir(path).map_or(false, |entries| {
        entries.filter_map(|e| e.ok()).any(|entry| {
            let entry_path = entry.path();
            entry_path.extension().map_or(false, |ext| ext == "rs") ||
            entry_path.join("main.rs").exists()
        })
    })
}

/// Load all modules from the modules directory recursively.
pub fn load_modules_from_directory(base_path: &Path) -> Vec<ModuleDefinition> {
    let mut modules = Vec::new();
    
    // Walk through the entire modules directory without depth limit for full recursive discovery
    for entry in walkdir::WalkDir::new(base_path).min_depth(1) {
        if let Ok(entry) = entry {
            let path = entry.path();
            
            // Check any depth - just look for directories with Rust files at any level under modules/
            if path.is_dir() && has_rust_files(&path) {
                // Build a hierarchical name from the directory structure
                let mut parts: Vec<&str> = Vec::new();
                
                for component in path.iter() {
                    if let Some(name) = component.to_str() {
                        // Skip "modules" root
                        if name != "modules" && !name.starts_with('.') {
                            parts.push(name);
                        }
                    }
                }
                
                // Only process directories that are at least 2 levels deep (modules/Category/Subcategory/)
                if parts.len() >= 3 {
                    let module_name = parts[1..].join("/");
                    
                    modules.push(ModuleDefinition {
                        id: format!("comm_{}", module_name),
                        name: module_name,
                        path: path.to_path_buf(),
                    });
                }
            }
        }
    }
    
    // Sort alphabetically by name for consistent ordering
    modules.sort_by(|a, b| a.name.cmp(&b.name));
    
    modules
}

/// Main function to load modules from the actual modules directory in the current project.
pub fn discover_modules() -> Vec<ModuleDefinition> {
    // Get absolute path of project root using cargo manifest dir env var
    let base_path = PathBuf::from(std::env!("CARGO_MANIFEST_DIR")).join("modules");
    
    if !base_path.exists() || !base_path.is_dir() {
        eprintln!("Error: modules directory not found at {:?}", base_path);
        return Vec::new();
    }
    
    load_modules_from_directory(&base_path)
}
