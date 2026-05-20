use std::collections::HashMap;
use std::fs;
use std::path::{Path, PathBuf};

#[derive(Clone)]
pub struct MenuItem {
    pub label: &'static str,
    pub submenu: Vec<&'static str>,
}

#[derive(Clone)]
pub struct ModuleInfo {
    pub name: String,
    pub category: String,
    pub status: ModuleStatus,
}

#[derive(Clone)]
pub enum ModuleStatus {
    Loaded,
    Unloaded,
    Error(String),
}

// MLOutbox implementation for ML machine outbox
struct MLOutbox {
    name: String,
    messages: Vec<String>,
}

impl MLOutbox {
    fn new(name: String) -> Self {
        Self {
            name,
            messages: Vec::new(),
        }
    }

    fn clear_messages(&mut self) {
        self.messages.clear();
    }

    fn add_message(&mut self, message: String) {
        self.messages.push(message);
    }

    fn get_outbox_content(&self) -> String {
        if self.messages.is_empty() {
            format!("{} - No messages", self.name)
        } else {
            let mut content = format!("{}\n", self.name);
            for msg in &self.messages {
                content.push_str(format!("- {}\n", msg).as_str());
            }
            content.trim().to_string()
        }
    }

    fn append_message(&mut self, module: &dyn ModuleHandler) -> Result<String, String> {
        // Simulate appending a message from the loaded module
        let msg = format!("Message from {} to {}", module.name(), self.name);
        self.add_message(msg.clone());
        Ok(msg)
    }
}

pub trait ModuleHandler {
    fn name(&self) -> &str;
    fn category(&self) -> &str;
    fn load(&mut self) -> Result<(), String>;
    fn unload(&mut self);
    fn status(&self) -> ModuleStatus;
    // New method for submenu interaction
    fn handle_submenu_interaction(&mut self, item: &str) -> Option<String> {
        None
    }
}

struct CommModule {
    loaded: bool,
}

impl ModuleHandler for CommModule {
    fn name(&self) -> &str { "Comm" }
    fn category(&self) -> &str { "Communication" }
    fn load(&mut self) -> Result<(), String> {
        if !self.loaded {
            println!("[+] Loading module: {}", self.name());
            self.loaded = true;
        }
        Ok(())
    }
    fn unload(&mut self) {
        println!("[ ] Unloading module: {}", self.name());
        self.loaded = false;
    }
    fn status(&self) -> ModuleStatus {
        if self.loaded { ModuleStatus::Loaded } else { ModuleStatus::Unloaded }
    }
    fn handle_submenu_interaction(&mut self, _item: &str) -> Option<String> {
        None
    }
}

struct EvasionModule {
    loaded: bool,
}

impl ModuleHandler for EvasionModule {
    fn name(&self) -> &str { "Evasion" }
    fn category(&self) -> &str { "Evasion" }
    fn load(&mut self) -> Result<(), String> {
        if !self.loaded {
            println!("[+] Loading module: {}", self.name());
            self.loaded = true;
        }
        Ok(())
    }
    fn unload(&mut self) {
        println!("[ ] Unloading module: {}", self.name());
        self.loaded = false;
    }
    fn status(&self) -> ModuleStatus {
        if self.loaded { ModuleStatus::Loaded } else { ModuleStatus::Unloaded }
    }
    fn handle_submenu_interaction(&mut self, _item: &str) -> Option<String> {
        None
    }
}

struct MLModule {
    loaded: bool,
    machine_status: String,
    outbox: MLOutbox,
}

impl ModuleHandler for MLModule {
    fn name(&self) -> &str { "ML Machine" }
    fn category(&self) -> &str { "Machine Learning Status" }
    
    fn load(&mut self) -> Result<(), String> {
        if !self.loaded {
            println!("[+] Loading module: {}", self.name());
            self.machine_status = "Active".to_string();
            self.outbox = MLOutbox::new("ML Outbox".to_string());
            self.loaded = true;
        }
        Ok(())
    }
    
    fn unload(&mut self) {
        println!("[ ] Unloading module: {}", self.name());
        self.loaded = false;
        self.machine_status = "Inactive".to_string();
        // Clear outbox on unload
        self.outbox.clear_messages();
    }
    
    fn status(&self) -> ModuleStatus {
        if self.loaded {
            ModuleStatus::Loaded
        } else {
            ModuleStatus::Unloaded
        }
    }

    // Handle submenu interaction for ML module
    fn handle_submenu_interaction(&mut self, item: &str) -> Option<String> {
        match item {
            "Model Health" => Some(format!("{} - {}", self.name(), self.machine_status.clone())),
            "Inference Queue" => Some("Queue ready for inference tasks".to_string()),
            "Outbox" => Some(self.outbox.get_outbox_content().clone()),
            _ => None,
        }
    }
}

pub struct ModuleLoader {
    module_path: PathBuf,
    modules: HashMap<String, Box<dyn ModuleHandler>>,
}

impl ModuleLoader {
    pub fn new(module_path: &Path) -> Self {
        let modules = HashMap::new();
        Self {
            module_path: module_path.to_path_buf(),
            modules,
        }
    }

    pub fn scan(&mut self) {
        println!("[*] Scanning for modules in: {:?}", self.module_path);
        
        if !self.module_path.exists() {
            println!("[!] Module directory not found.");
            return;
        }

        let mut load_order = Vec::new();
        
        // Scan directories
        for entry in fs::read_dir(&self.module_path).unwrap_or_default() {
            let entry = match entry {
                Ok(e) => e,
                Err(e) => {
                    println!("[!] Error reading directory: {}", e);
                    continue;
                }
            };

            let path = entry.path();
            if path.is_dir() {
                let name = path.file_name().unwrap_or_default().to_string_lossy().to_string();
                
                // Create handler for each module category
                match name.as_str() {
                    "Comm" => {
                        let comm = Box::new(CommModule { loaded: false });
                        self.modules.insert(name.clone(), comm);
                        load_order.push(name.clone());
                    }
                    "Evasion" => {
                        let evasion = Box::new(EvasionModule { loaded: false });
                        self.modules.insert(name.clone(), evasion);
                        load_order.push(name.clone());
                    }
                    "ML" => {
                        let ml = Box::new(MLModule { loaded: false });
                        self.modules.insert(name.clone(), ml);
                        load_order.push(name.clone());
                    }
                    _ => {}
                }
            }
        }

        println!("[*] Found {} modules to load", load_order.len());
        
        // Load all discovered modules
        for name in load_order {
            if let Some(module) = self.modules.get_mut(&name) {
                if let Err(e) = module.load() {
                    println!("[!] Failed to load {}: {}", name, e);
                }
            }
        }
    }

    pub fn get_menu_items(&self) -> Vec<MenuItem> {
        let mut items = Vec::new();

        // Comm menu
        if self.modules.get("Comm").is_some() {
            items.push(MenuItem {
                label: "[ Comm ]",
                submenu: vec![
                    "→ Open Channel",
                    "→ Listener Config",
                    "→ Active Sessions",
                    "→ Beacon Settings",
                ],
            });
        }

        // Evasion menu
        if self.modules.get("Evasion").is_some() {
            items.push(MenuItem {
                label: "[ Evasion ]",
                submenu: vec![
                    "→ Obfuscation",
                    "→ AV Bypass",
                    "→ Sandbox Detection",
                    "→ Process Hollowing",
                ],
            });
        }

        // ML Machine Status menu (with outbox)
        if self.modules.get("ML").is_some() {
            items.push(MenuItem {
                label: "[ ML Machine Status ]",
                submenu: vec![
                    "→ Model Health",
                    "→ Inference Queue",
                    "→ Outbox",
                ],
            });
        }

        // Recon menu (static for now)
        items.push(MenuItem {
            label: "[ Recon ]",
            submenu: vec![
                "→ Host Discovery",
                "→ Port Scan",
                "→ Service Enum",
                "→ OSINT Gather",
            ],
        });

        // Logs menu (static for now)
        items.push(MenuItem {
            label: "[ Logs ]",
            submenu: vec![
                "→ Event Log",
                "→ Session History",
                "→ Export Report",
            ],
        });

        // Exit menu
        items.push(MenuItem {
            label: "[ EXIT ]",
            submenu: vec![],
        });

        items
    }

    pub fn load_module(&mut self, name: &str) -> bool {
        if let Some(module) = self.modules.get_mut(name) {
            match module.load() {
                Ok(_) => println!("[+] Module loaded: {}", name),
                Err(e) => println!("[!] Failed to load {}: {}", name, e),
            }
            true
        } else {
            println!("[!] Module not found: {}", name);
            false
        }
    }

    pub fn unload_module(&mut self, name: &str) -> bool {
        if let Some(module) = self.modules.get_mut(name) {
            module.unload();
            true
        } else {
            println!("[!] Module not found: {}", name);
            false
        }
    }

    pub fn is_module_loaded(&self, name: &str) -> bool {
        if let Some(module) = self.modules.get(name) {
            matches!(module.status(), ModuleStatus::Loaded)
        } else {
            false
        }
    }

    pub fn get_module_status(&self, name: &str) -> String {
        if let Some(module) = self.modules.get(name) {
            match module.status() {
                ModuleStatus::Loaded => format!("{} ({})", name, module.category()),
                ModuleStatus::Unloaded => name.to_string(),
                ModuleStatus::Error(e) => format!("{}: {}", name, e),
            }
        } else {
            name.to_string()
        }
    }
}
