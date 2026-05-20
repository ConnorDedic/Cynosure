use std::path::{Path, PathBuf};
use std::collections::HashMap;

/// Represents a discoverable module from the modules directory.
#[derive(Clone)]
pub struct Module {
    /// Category (e.g., "Comm", "Evasion")
    pub category: String,
    /// Subcategory or feature name
    pub feature: String,
    /// Full path to the module folder
    pub path: PathBuf,
    /// Description of what this module does
    pub description: Option<String>,
}

impl Module {
    /// Creates a new module instance.
    fn new(category: &str, feature: &str, path: PathBuf) -> Self {
        Module {
            category: category.to_string(),
            feature: feature.to_string(),
            path,
            description: None,
        }
    }

    /// Returns a formatted string representing the module's name (e.g., "Comm / DNS").
    pub fn display_name(&self) -> String {
        format!("{} / {}", self.category, self.feature)
    }

    /// Attempts to read module metadata from a dedicated metadata.txt file within the module path.
    /// Returns None if the file does not exist or reading fails.
    pub fn read_metadata<P: AsRef<Path>>(&mut self, path: P) -> Option<String> {
        let meta_path = self.path.join("metadata.txt");
        if meta_path.exists() {
            std::fs::read_to_string(&meta_path).ok().map(String::from)
        } else {
            None
        }
    }

    /// Discovers all submodule directories within a given category path, populating Module structs.
    pub fn discover_category<P: AsRef<Path>>(category_path: P) -> Vec<Self> {
        let category_path = category_path.as_ref();
        if !category_path.exists() || !category_path.is_dir() {
            return vec![];
        }

        let mut modules = vec![];

        // Iterate over all subdirectories in the category folder
        for entry in std::fs::read_dir(category_path).ok().into_iter().flatten() {
            let path = entry.path();
            if path.is_dir() {
                let feature_name = path.file_name()
                    .and_then(|n| n.to_str())
                    .unwrap_or("Unknown")
                    .to_string();

                // Construct module using the category name derived from the parent directory
                let module = Module::new(&category_path.file_name().unwrap_or_default().to_string_lossy(), 
                                        &feature_name, path);
                modules.push(module);
            }
        }

        modules.sort_by(|a, b| a.display_name().cmp(&b.display_name()));
        modules
    }

    /// Discovers all modules across the entire module root directory.
    /// It traverses top-level categories and calls discover_category for submodules.
    pub fn discover_all<P: AsRef<Path>>(modules_root: P) -> Vec<Self> {
        let modules_root = modules_root.as_ref();

        if !modules_root.exists() || !modules_root.is_dir() {
            return vec![];
        }

        let mut all_modules = vec![];

        // Collect all category directories (not files)
        for entry in std::fs::read_dir(modules_root).ok().flatten() {
            let path = entry.path();
            
            if !path.is_dir() {
                continue;
            }

            let category_name = match path.file_name() {
                Some(n) => n.to_string_lossy().to_string(),
                None => continue,
            };

            // Skip hidden directories (starting with .)
            if category_name.starts_with('.') {
                continue;
            }

            // Discover submodules in this category
            let mut modules = Module::discover_category(&path);
            
            // Attempt to read metadata for each module found in this category
            for module in &mut modules {
                if let Some(desc) = module.read_metadata(&module.path) {
                    module.description = Some(desc);
                }
            }

            all_modules.extend(modules);
        }

        all_modules.sort_by(|a, b| a.display_name().cmp(&b.display_name()));
        all_modules
    }
}


/// Module registry to hold discovered modules and provide lookup functionality.
pub struct ModuleRegistry {
    /// All loaded modules in the system.
    pub modules: Vec<Module>,
    /// Modules grouped by category for efficient lookups (e.g., HashMap<"Comm", [Module]>)
    pub by_category: HashMap<String, Vec<Module>>,
}

impl ModuleRegistry {
    /// Creates a new, empty ModuleRegistry instance.
    fn new() -> Self {
        let mut registry = RegistryBuilder::new();
        registry.load_all_from_default_path().unwrap_or_default()
    }

    /// Load modules from the default modules directory path. 
    /// This function performs discovery and initializes all internal mappings.
    pub fn load_all_from_default_path<P: AsRef<Path>>(modules_root: P) -> Result<Self, Box<dyn std::error::Error>> {
        // Step 1: Discover all modules
        let all_modules = Module::discover_all(modules_root)?;
        
        // Step 2: Group by category for quick access
        let mut by_category: HashMap<String, Vec<Module>> = HashMap::new();
        
        for module in &all_modules {
            by_category.entry(module.category.clone())
                .or_insert_with(Vec::new)
                .push(module.clone());
        }

        Ok(ModuleRegistry {
            modules: all_modules,
            by_category,
        })
    }

    /// Get a reference to all registered modules.
    pub fn get_all(&self) -> &[Module] {
        &self.modules
    }

    /// Get modules filtered by a specific category name (e.g., "Comm").
    pub fn filter_by_category(&self, category: &str) -> Vec<&Module> {
        self.by_category.get(category)
            .map(|v| v.as_slice())
            .unwrap_or(&[])
            .to_vec()
    }

    /// Get a module by its feature name (case-insensitive). Returns None if not found.
    pub fn find_by_feature(&self, feature: &str) -> Option<&Module> {
        self.modules.iter().find(|m| m.feature.to_lowercase() == feature.to_lowercase())
    }

    /// Check if a specific module exists based on category and feature name combination.
    pub fn has_module(&self, category: &str, feature: &str) -> bool {
        self.find_by_feature(feature).map(|m| m.category == category).unwrap_or(false)
    }

    /// Get the total count of loaded modules in the registry.
    pub fn len(&self) -> usize {
        self.modules.len()
    }

    /// Clears all loaded modules and resets the registry state (useful for hot-reloading).
    pub fn clear(&mut self) {
        self.modules.clear();
        self.by_category.clear();
    }

    /// Reloads module information from the file system, recalculating the entire registry structure.
    pub fn reload<P: AsRef<Path>>(modules_root: P) -> Result<Self, Box<dyn std::error::Error>> {
        let mut registry = ModuleRegistry::new();
        // Re-run discovery to get current file state
        registry.modules = Module::discover_all(modules_root)?;
        
        // Rebuild category map with the latest module data
        for module in &registry.modules {
            registry.by_category.entry(module.category.clone())
                .or_insert_with(Vec::new)
                .push(module.clone());
        }

        Ok(registry)
    }
}

/// Builder pattern helper for creating and initializing a ModuleRegistry.
pub struct RegistryBuilder;

impl RegistryBuilder {
    /// Initializes the builder.
    pub fn new() -> Self {
        Self
    }

    /// Executes the full module loading process from the specified root path.
    pub fn load_all_from_default_path<P: AsRef<Path>>(self, modules_root: P) -> Result<ModuleRegistry, Box<dyn std::error::Error>> {
        ModuleRegistry::load_all_from_default_path(modules_root)
    }
}