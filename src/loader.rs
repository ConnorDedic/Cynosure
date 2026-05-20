use std::path::PathBuf;

#[derive(Debug, Clone)]
pub struct ModuleDefinition {
    pub name: String,
    pub category: String,
    pub subcategory: String,
    pub path: PathBuf,
}

#[derive(Debug, Clone)]
pub struct Category {
    pub name: String,
    pub subcategories: Vec<Subcategory>,
}

#[derive(Debug, Clone)]
pub struct Subcategory {
    pub name: String,
    pub modules: Vec<ModuleDefinition>,
}

/// Walk modules/{Category}/{Subcategory}/{Module}/main.rs
pub fn discover_tree() -> Vec<Category> {
    let base = PathBuf::from(std::env!("CARGO_MANIFEST_DIR")).join("modules");
    if !base.is_dir() {
        return Vec::new();
    }

    let mut categories: Vec<Category> = Vec::new();

    let mut cat_dirs: Vec<_> = std::fs::read_dir(&base)
        .unwrap()
        .filter_map(|e| e.ok())
        .filter(|e| e.path().is_dir())
        .collect();
    cat_dirs.sort_by_key(|e| e.file_name());

    for cat_entry in cat_dirs {
        let cat_path = cat_entry.path();
        let cat_name = cat_entry.file_name().to_string_lossy().to_string();

        let mut subcategories: Vec<Subcategory> = Vec::new();

        let mut sub_dirs: Vec<_> = std::fs::read_dir(&cat_path)
            .unwrap_or_else(|_| std::fs::read_dir(".").unwrap())
            .filter_map(|e| e.ok())
            .filter(|e| e.path().is_dir())
            .collect();
        sub_dirs.sort_by_key(|e| e.file_name());

        for sub_entry in sub_dirs {
            let sub_path = sub_entry.path();
            let sub_name = sub_entry.file_name().to_string_lossy().to_string();

            let mut modules: Vec<ModuleDefinition> = Vec::new();

            let mut mod_dirs: Vec<_> = std::fs::read_dir(&sub_path)
                .unwrap_or_else(|_| std::fs::read_dir(".").unwrap())
                .filter_map(|e| e.ok())
                .filter(|e| e.path().is_dir() && e.path().join("main.rs").exists())
                .collect();
            mod_dirs.sort_by_key(|e| e.file_name());

            for mod_entry in mod_dirs {
                let mod_path = mod_entry.path();
                let mod_name = mod_entry.file_name().to_string_lossy().to_string();
                modules.push(ModuleDefinition {
                    name: mod_name,
                    category: cat_name.clone(),
                    subcategory: sub_name.clone(),
                    path: mod_path,
                });
            }

            if !modules.is_empty() {
                subcategories.push(Subcategory {
                    name: sub_name,
                    modules,
                });
            }
        }

        if !subcategories.is_empty() {
            categories.push(Category {
                name: cat_name,
                subcategories,
            });
        }
    }

    categories
}
