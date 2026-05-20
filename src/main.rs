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
    widgets::{Block, BorderType, Borders, Clear, List, ListItem, ListState, Paragraph},
    Terminal,
};
use std::{io, time::Duration};

mod loader;
use loader::discover_tree;


// ================================================================
// DATA STRUCTURES
// ================================================================

/// A loaded module inside a running implant.
#[derive(Clone)]
struct LoadedModule {
    name:    &'static str,
    kind:    &'static str, // "recon" | "exfil" | "persist" | "tunnel"
    version: &'static str,
    active:  bool,         // currently executing / hooked
}

/// A live implant session on a compromised host.
#[derive(Clone)]
struct ImplantSession {
    // Identity
    id:       &'static str,
    name:     &'static str, // implant binary name / alias

    // Network
    ip:       &'static str,
    subnet:   &'static str,
    beacon:   &'static str, // "ip:port" of C2 listener
    protocol: &'static str, // "HTTPS" | "DNS" | "SMB" | ...

    // Host context
    hostname: &'static str,
    user:     &'static str,
    os:       &'static str,
    arch:     &'static str,
    pid:      u32,

    // Status
    status:    &'static str, // "Active" | "Idle" | "Lost"
    last_seen: &'static str,
    uptime:    &'static str,
    elevated:  bool,         // running as admin / root?

    // Loaded capabilities
    modules: Vec<LoadedModule>,
}

fn stub_sessions() -> Vec<ImplantSession> {
    vec![
        ImplantSession {
            id: "s-001", name: "phantom-x86",
            ip: "10.0.0.12", subnet: "10.0.0.0/24",
            beacon: "10.0.0.12:4444", protocol: "HTTPS",
            hostname: "WS-FINANCE-04", user: "jsmith",
            os: "Windows 10 22H2", arch: "x86_64", pid: 4812,
            status: "Active", last_seen: "2s ago", uptime: "3h 22m",
            elevated: true,
            modules: vec![
                LoadedModule { name: "keylogger",    kind: "exfil",   version: "2.1.0", active: true  },
                LoadedModule { name: "port-scanner", kind: "recon",   version: "1.4.2", active: false },
                LoadedModule { name: "cred-dumper",  kind: "exfil",   version: "3.0.1", active: false },
                LoadedModule { name: "rev-tunnel",   kind: "tunnel",  version: "1.0.8", active: true  },
            ],
        },
        ImplantSession {
            id: "s-002", name: "phantom-x86",
            ip: "10.0.0.13", subnet: "10.0.0.0/24",
            beacon: "10.0.0.13:4444", protocol: "HTTPS",
            hostname: "SRV-DEVOPS-01", user: "svc_deploy",
            os: "Ubuntu 22.04 LTS", arch: "x86_64", pid: 1337,
            status: "Idle", last_seen: "1m ago", uptime: "11h 05m",
            elevated: false,
            modules: vec![
                LoadedModule { name: "port-scanner", kind: "recon",   version: "1.4.2", active: false },
                LoadedModule { name: "file-stealer", kind: "exfil",   version: "2.2.0", active: false },
            ],
        },
        ImplantSession {
            id: "s-003", name: "wraith-arm",
            ip: "192.168.1.55", subnet: "192.168.1.0/24",
            beacon: "192.168.1.55:443", protocol: "DNS",
            hostname: "RPI-CTRLNODE", user: "pi",
            os: "Raspberry Pi OS (64-bit)", arch: "aarch64", pid: 889,
            status: "Active", last_seen: "5s ago", uptime: "6d 14h",
            elevated: true,
            modules: vec![
                LoadedModule { name: "net-enum",    kind: "recon",   version: "1.1.0", active: true  },
                LoadedModule { name: "persistence", kind: "persist", version: "2.0.3", active: true  },
                LoadedModule { name: "rev-tunnel",  kind: "tunnel",  version: "1.0.8", active: false },
            ],
        },
        ImplantSession {
            id: "s-004", name: "spectre-win",
            ip: "172.16.0.9", subnet: "172.16.0.0/16",
            beacon: "172.16.0.9:8080", protocol: "HTTP",
            hostname: "DC-CORP-01", user: "Administrator",
            os: "Windows Server 2019", arch: "x86_64", pid: 6640,
            status: "Lost", last_seen: "10m ago", uptime: "—",
            elevated: true,
            modules: vec![
                LoadedModule { name: "ad-enum",     kind: "recon",   version: "1.3.0", active: false },
                LoadedModule { name: "cred-dumper", kind: "exfil",   version: "3.0.1", active: false },
                LoadedModule { name: "persistence", kind: "persist", version: "2.0.3", active: false },
            ],
        },
        ImplantSession {
            id: "s-005", name: "spectre-win",
            ip: "172.16.0.10", subnet: "172.16.0.0/16",
            beacon: "172.16.0.10:8080", protocol: "SMB",
            hostname: "WS-LEGAL-11", user: "agreen",
            os: "Windows 11 23H2", arch: "x86_64", pid: 3320,
            status: "Idle", last_seen: "3m ago", uptime: "1h 48m",
            elevated: false,
            modules: vec![
                LoadedModule { name: "keylogger",  kind: "exfil", version: "2.1.0", active: false },
                LoadedModule { name: "screenshot", kind: "exfil", version: "1.0.0", active: false },
            ],
        },
        ImplantSession {
            id: "s-006", name: "spectre-win",
            ip: "172.16.0.11", subnet: "172.16.0.0/16",
            beacon: "172.16.0.11:8080", protocol: "SMB",
            hostname: "WS-IT-03", user: "SYSTEM",
            os: "Windows 10 21H2", arch: "x86_64", pid: 4,
            status: "Active", last_seen: "1s ago", uptime: "2d 01h",
            elevated: true,
            modules: vec![
                LoadedModule { name: "keylogger",    kind: "exfil",   version: "2.1.0", active: true  },
                LoadedModule { name: "port-scanner", kind: "recon",   version: "1.4.2", active: false },
                LoadedModule { name: "cred-dumper",  kind: "exfil",   version: "3.0.1", active: true  },
                LoadedModule { name: "ad-enum",      kind: "recon",   version: "1.3.0", active: false },
                LoadedModule { name: "rev-tunnel",   kind: "tunnel",  version: "1.0.8", active: true  },
                LoadedModule { name: "persistence",  kind: "persist", version: "2.0.3", active: true  },
            ],
        },
        ImplantSession {
            id: "s-007", name: "null-mac",
            ip: "10.10.10.7", subnet: "10.10.10.0/24",
            beacon: "10.10.10.7:9001", protocol: "HTTPS",
            hostname: "MBP-CEO", user: "c.rhodes",
            os: "macOS Sonoma 14.4", arch: "arm64", pid: 2291,
            status: "Idle", last_seen: "8m ago", uptime: "4h 33m",
            elevated: false,
            modules: vec![
                LoadedModule { name: "keylogger",    kind: "exfil", version: "2.1.0", active: false },
                LoadedModule { name: "screenshot",   kind: "exfil", version: "1.0.0", active: false },
                LoadedModule { name: "file-stealer", kind: "exfil", version: "2.2.0", active: false },
            ],
        },
    ]
}

/// Commands available in the launcher overlay.
const COMMANDS: &[(&str, &str)] = &[
    ("shell",        "Spawn interactive command shell"),
    ("upload",       "Upload file to target"),
    ("download",     "Download file from target"),
    ("run-module",   "Execute a loaded module"),
    ("load-module",  "Inject new module into implant"),
    ("screenshot",   "Capture desktop screenshot"),
    ("ps",           "List running processes"),
    ("netstat",      "Show active connections"),
    ("sysinfo",      "Dump full system information"),
    ("kill-session", "Terminate and clean up session"),
];


// ================================================================
// APPLICATION STATE
// ================================================================

#[derive(PartialEq, Clone)]
enum Panel {
    // Module tree world
    Category, Subcategory, Module,
    // Implant world
    Sessions, SessionDetail, ImplantModules, ImplantCommand,
}

impl Panel {
    fn in_module_world(&self) -> bool {
        matches!(self, Panel::Category | Panel::Subcategory | Panel::Module)
    }
    fn in_implant_world(&self) -> bool {
        matches!(self, Panel::Sessions | Panel::SessionDetail | Panel::ImplantModules | Panel::ImplantCommand)
    }
}

struct App {
    // Module tree
    categories: Vec<loader::Category>,
    cat_state:  ListState,
    sub_state:  ListState,
    mod_state:  ListState,

    // Implant sessions
    sessions:   Vec<ImplantSession>,
    sess_state: ListState,
    imod_state: ListState, // loaded-module list cursor
    cmd_state:  ListState, // command launcher cursor

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

        let sessions = stub_sessions();
        let mut sess_state = ListState::default();
        if !sessions.is_empty() { sess_state.select(Some(0)); }

        let mut imod_state = ListState::default();
        if !sessions.is_empty() && !sessions[0].modules.is_empty() {
            imod_state.select(Some(0));
        }

        let mut cmd_state = ListState::default();
        cmd_state.select(Some(0));

        Self {
            categories,
            cat_state, sub_state, mod_state,
            sessions,
            sess_state, imod_state, cmd_state,
            focus: Panel::Category,
            should_quit: false,
        }
    }

    // --- Accessors ---
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

    // --- Input ---
    fn on_key(&mut self, key: KeyCode) {
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
                KeyCode::Tab => self.focus = Panel::Category,
                KeyCode::Char('q') | KeyCode::Esc => self.should_quit = true,
                _ => {}
            },
            Panel::SessionDetail => match key {
                KeyCode::Char('m') => { if self.imod_len() > 0 { self.focus = Panel::ImplantModules; } }
                KeyCode::Char('c') => { self.cmd_state.select(Some(0)); self.focus = Panel::ImplantCommand; }
                KeyCode::Esc | KeyCode::Left | KeyCode::Char('h') => self.focus = Panel::Sessions,
                KeyCode::Tab => self.focus = Panel::Category,
                KeyCode::Char('q') => self.should_quit = true,
                _ => {}
            },
            Panel::ImplantModules => match key {
                KeyCode::Up   | KeyCode::Char('k') => { let n = self.imod_len(); nav_up(&mut self.imod_state, n); }
                KeyCode::Down | KeyCode::Char('j') => { let n = self.imod_len(); nav_dn(&mut self.imod_state, n); }
                KeyCode::Esc | KeyCode::Left | KeyCode::Char('h') => self.focus = Panel::SessionDetail,
                KeyCode::Tab => self.focus = Panel::Category,
                KeyCode::Char('q') => self.should_quit = true,
                _ => {}
            },
            Panel::ImplantCommand => match key {
                KeyCode::Up   | KeyCode::Char('k') => { nav_up(&mut self.cmd_state, COMMANDS.len()); }
                KeyCode::Down | KeyCode::Char('j') => { nav_dn(&mut self.cmd_state, COMMANDS.len()); }
                KeyCode::Enter => { /* TODO: dispatch selected command */ }
                KeyCode::Esc | KeyCode::Left | KeyCode::Char('h') | KeyCode::Char('q') => self.focus = Panel::SessionDetail,
                KeyCode::Tab => self.focus = Panel::Category,
                _ => {}
            },
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
// BORDER HELPERS
// ================================================================

/// World-container border: Double+Cyan when active, Plain+DarkGray when not.
fn world_border(active: bool) -> (BorderType, Style) {
    if active {
        (BorderType::Double, Style::default().fg(Color::Cyan).add_modifier(Modifier::BOLD))
    } else {
        (BorderType::Plain, Style::default().fg(Color::DarkGray))
    }
}

/// Individual panel border: Green when focused, DarkGray otherwise.
fn panel_border(focused: bool) -> Style {
    if focused { Style::default().fg(Color::Green) } else { Style::default().fg(Color::DarkGray) }
}

/// Shrink a Rect inward by `margin` cells on every side.
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


// ================================================================
// LOGO + DRAW ROOT
// ================================================================

const LOGO: &str = r#" ██████╗██╗   ██╗███╗   ██╗ ██████╗ ███████╗██╗   ██╗██████╗ ███████╗     ██████╗██████╗ 
██╔════╝╚██╗ ██╔╝████╗  ██║██╔═══██╗██╔════╝██║   ██║██╔══██╗██╔════╝    ██╔════╝╚════██╗
██║      ╚████╔╝ ██║██╗ ██║██║   ██║███████╗██║   ██║██████╔╝█████╗      ██║      █████╔╝
██║       ╚██╔╝  ██║╚██╗██║██║   ██║╚════██║██║   ██║██╔══██╗██╔══╝      ██║     ██╔═══╝ 
╚██████╗   ██║   ██║ ╚████║╚██████╔╝███████║╚██████╔╝██║  ██║███████╗    ╚██████╗███████╗
 ╚═════╝   ╚═╝   ╚═╝  ╚═══╝ ╚═════╝ ╚══════╝ ╚═════╝ ╚═╝  ╚═╝╚══════╝     ╚═════╝╚══════╝

 "#;

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

    frame.render_widget(
        Paragraph::new(Line::from(vec![
            Span::styled("  ● ", Style::default().fg(Color::Green)),
            Span::styled("Status: ",          Style::default().fg(Color::DarkGray)),
            Span::styled("All Systems Nominal  ", Style::default().fg(Color::White)),
            Span::styled("| Sessions: ",      Style::default().fg(Color::DarkGray)),
            Span::styled(format!("{} active ", active_count),  Style::default().fg(Color::Green)),
            Span::styled(format!("{} idle ", idle_count),      Style::default().fg(Color::Yellow)),
            Span::styled(format!("{} lost  ", lost_count),     Style::default().fg(Color::Red)),
            Span::styled("| Elevated: ",      Style::default().fg(Color::DarkGray)),
            Span::styled(format!("{}  ", elevated_count),      Style::default().fg(Color::Magenta)),
            Span::styled("| Modules: ",       Style::default().fg(Color::DarkGray)),
            Span::styled(format!("{} loaded", total_modules),  Style::default().fg(Color::Cyan)),
        ]))
        .block(Block::default().borders(Borders::ALL)
            .border_style(Style::default().fg(Color::DarkGray))
            .title(Span::styled(" System Status ", Style::default().fg(Color::Cyan)))),
        root[1],
    );

    // --- Body ---
    let body = Layout::default()
        .direction(Direction::Horizontal)
        .constraints([Constraint::Percentage(55), Constraint::Percentage(45)])
        .split(root[2]);

    draw_module_tree(frame, app, body[0]);
    draw_implant_world(frame, app, body[1]);

    // --- Context-sensitive footer ---
    let footer_spans: Vec<Span> = match app.focus {
        Panel::SessionDetail => vec![
            Span::styled(" ↑↓/jk ", Style::default().fg(Color::Yellow)),  Span::raw("Select  "),
            Span::styled(" m ",     Style::default().fg(Color::Cyan).add_modifier(Modifier::BOLD)),   Span::raw("Modules  "),
            Span::styled(" c ",     Style::default().fg(Color::Green).add_modifier(Modifier::BOLD)),  Span::raw("Commands  "),
            Span::styled(" h/Esc ", Style::default().fg(Color::Yellow)),   Span::raw("Back  "),
            Span::styled(" Tab ",   Style::default().fg(Color::Magenta).add_modifier(Modifier::BOLD)), Span::raw("Switch World  "),
            Span::styled(" q ",     Style::default().fg(Color::Red).add_modifier(Modifier::BOLD)),    Span::raw("Quit"),
        ],
        Panel::ImplantCommand => vec![
            Span::styled(" ↑↓/jk ", Style::default().fg(Color::Yellow)),  Span::raw("Select  "),
            Span::styled(" Enter ", Style::default().fg(Color::Green).add_modifier(Modifier::BOLD)),  Span::raw("Dispatch  "),
            Span::styled(" Esc/q ", Style::default().fg(Color::Yellow)),   Span::raw("Close  "),
        ],
        _ => vec![
            Span::styled(" ↑↓/jk ", Style::default().fg(Color::Yellow)),  Span::raw("Navigate  "),
            Span::styled("→/Enter ", Style::default().fg(Color::Yellow)),  Span::raw("Drill in  "),
            Span::styled("←/Esc ",  Style::default().fg(Color::Yellow)),   Span::raw("Back  "),
            Span::styled(" Tab ",   Style::default().fg(Color::Magenta).add_modifier(Modifier::BOLD)), Span::raw("Switch World  "),
            Span::styled(" q ",     Style::default().fg(Color::Red).add_modifier(Modifier::BOLD)),    Span::raw("Quit"),
        ],
    };
    frame.render_widget(
        Paragraph::new(Line::from(footer_spans)).style(Style::default().fg(Color::DarkGray)),
        root[3],
    );

    // --- Command launcher overlay (always drawn last, on top) ---
    if app.focus == Panel::ImplantCommand {
        draw_command_overlay(frame, app, size);
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
        .constraints([Constraint::Length(20), Constraint::Length(24), Constraint::Min(0)])
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

    // Module + detail split
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
        _ => draw_session_list(frame, app, inner),
    }
}

/// Session list: IP, user@host, protocol, status, module count.
fn draw_session_list(frame: &mut ratatui::Frame, app: &mut App, area: Rect) {
    let focused = app.focus == Panel::Sessions;

    // Split into a 1-row header + scrollable list
    let split = Layout::default()
        .direction(Direction::Vertical)
        .constraints([Constraint::Length(1), Constraint::Min(0)])
        .split(area);

    // Header row
    frame.render_widget(
        Paragraph::new(Line::from(vec![
            Span::raw("      "), // space for ⬆ + ● indicators
            Span::styled(format!("{:<16}", "IP Address"),       Style::default().fg(Color::DarkGray).add_modifier(Modifier::BOLD)),
            Span::styled(format!("{:<20}", "User @ Hostname"),  Style::default().fg(Color::DarkGray).add_modifier(Modifier::BOLD)),
            Span::styled(format!("{:<7}",  "Proto"),            Style::default().fg(Color::DarkGray).add_modifier(Modifier::BOLD)),
            Span::styled(format!("{:<8}",  "Status"),           Style::default().fg(Color::DarkGray).add_modifier(Modifier::BOLD)),
            Span::styled("Mods",                                Style::default().fg(Color::DarkGray).add_modifier(Modifier::BOLD)),
        ])),
        split[0],
    );

    let items: Vec<ListItem> = app.sessions.iter().enumerate().map(|(i, s)| {
        let sel = Some(i) == app.sess_state.selected() && focused;
        let elev = if s.elevated {
            Span::styled("⬆ ", Style::default().fg(Color::Magenta))
        } else {
            Span::raw("  ")
        };
        let line = Line::from(vec![
            Span::raw("  "),
            elev,
            Span::styled("● ", Style::default().fg(status_color(s.status))),
            Span::styled(format!("{:<16}", s.ip),                              Style::default().fg(Color::White)),
            Span::styled(format!("{:<20}", format!("{}@{}", s.user, s.hostname)), Style::default().fg(Color::Cyan)),
            Span::styled(format!("{:<7}",  s.protocol),                        Style::default().fg(Color::DarkGray)),
            Span::styled(format!("{:<8}",  s.status),                          Style::default().fg(status_color(s.status))),
            Span::styled(format!("[{}]", s.modules.len()),                     Style::default().fg(Color::DarkGray)),
        ]);
        ListItem::new(line).style(if sel {
            Style::default().fg(Color::Black).bg(Color::Green).add_modifier(Modifier::BOLD)
        } else {
            Style::default().fg(Color::White)
        })
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

/// Session detail view: info card + loaded modules + action hints.
fn draw_session_detail_view(frame: &mut ratatui::Frame, app: &mut App, area: Rect) {
    let Some(sess) = app.selected_session().cloned() else { return; };

    let vsplit = Layout::default()
        .direction(Direction::Vertical)
        .constraints([Constraint::Length(12), Constraint::Min(0)])
        .split(area);

    // ── Info card ──
    let detail_focused = app.focus == Panel::SessionDetail;
    let elev_str   = if sess.elevated { "YES ⬆  (admin / root)" } else { "no" };
    let elev_color = if sess.elevated { Color::Magenta } else { Color::DarkGray };
    let status_sym = match sess.status { "Active" => "● ACTIVE", "Idle" => "◌ IDLE", _ => "✕ LOST" };

    let info_lines = vec![
        Line::from(vec![
            Span::styled(format!("  {:>12}  ", "ID"),           Style::default().fg(Color::DarkGray)),
            Span::styled(sess.id,                                Style::default().fg(Color::White).add_modifier(Modifier::BOLD)),
            Span::raw("   "),
            Span::styled(status_sym, Style::default().fg(status_color(sess.status)).add_modifier(Modifier::BOLD)),
        ]),
        Line::from(vec![
            Span::styled(format!("  {:>12}  ", "Implant"),      Style::default().fg(Color::DarkGray)),
            Span::styled(sess.name,                              Style::default().fg(Color::Cyan)),
        ]),
        Line::from(vec![
            Span::styled(format!("  {:>12}  ", "Hostname"),     Style::default().fg(Color::DarkGray)),
            Span::styled(sess.hostname,                          Style::default().fg(Color::White)),
        ]),
        Line::from(vec![
            Span::styled(format!("  {:>12}  ", "IP / Subnet"),  Style::default().fg(Color::DarkGray)),
            Span::styled(sess.ip,                                Style::default().fg(Color::Green)),
            Span::raw("  /  "),
            Span::styled(sess.subnet,                            Style::default().fg(Color::DarkGray)),
        ]),
        Line::from(vec![
            Span::styled(format!("  {:>12}  ", "Beacon"),       Style::default().fg(Color::DarkGray)),
            Span::styled(sess.beacon,                            Style::default().fg(Color::White)),
            Span::styled(format!("  [{}]", sess.protocol),      Style::default().fg(Color::DarkGray)),
        ]),
        Line::from(vec![
            Span::styled(format!("  {:>12}  ", "User"),         Style::default().fg(Color::DarkGray)),
            Span::styled(sess.user,                              Style::default().fg(Color::White)),
            Span::styled(format!("  (PID {})", sess.pid),       Style::default().fg(Color::DarkGray)),
        ]),
        Line::from(vec![
            Span::styled(format!("  {:>12}  ", "Elevated"),     Style::default().fg(Color::DarkGray)),
            Span::styled(elev_str, Style::default().fg(elev_color).add_modifier(Modifier::BOLD)),
        ]),
        Line::from(vec![
            Span::styled(format!("  {:>12}  ", "OS / Arch"),    Style::default().fg(Color::DarkGray)),
            Span::styled(sess.os,                                Style::default().fg(Color::White)),
            Span::styled(format!("  ({})", sess.arch),          Style::default().fg(Color::DarkGray)),
        ]),
        Line::from(vec![
            Span::styled(format!("  {:>12}  ", "Last Seen"),    Style::default().fg(Color::DarkGray)),
            Span::styled(sess.last_seen, Style::default().fg(status_color(sess.status))),
            Span::styled(format!("  up {}", sess.uptime),       Style::default().fg(Color::DarkGray)),
        ]),
    ];

    frame.render_widget(
        Paragraph::new(info_lines)
            .block(Block::default().borders(Borders::ALL)
                .border_style(panel_border(detail_focused))
                .title(Span::styled(
                    format!(" {} — {} ", sess.id, sess.hostname),
                    Style::default().fg(Color::Yellow).add_modifier(Modifier::BOLD),
                ))),
        vsplit[0],
    );

    // ── Bottom: loaded modules (left) + action hints (right) ──
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
        let dot = if m.active {
            Span::styled("● ", Style::default().fg(Color::Green))
        } else {
            Span::styled("○ ", Style::default().fg(Color::DarkGray))
        };
        let line = Line::from(vec![
            Span::raw("  "),
            dot,
            Span::styled(format!("{:<18}", m.name),    Style::default().fg(Color::White)),
            Span::styled(format!("{:<9}", m.kind),     Style::default().fg(kind_color(m.kind))),
            Span::styled(format!("v{}", m.version),    Style::default().fg(Color::DarkGray)),
        ]);
        ListItem::new(line).style(if sel {
            Style::default().fg(Color::Black).bg(Color::Green).add_modifier(Modifier::BOLD)
        } else {
            Style::default().fg(Color::White)
        })
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
    let lines = vec![
        Line::from(Span::styled("  Quick Actions", Style::default().fg(Color::DarkGray).add_modifier(Modifier::BOLD))),
        Line::from(Span::raw("")),
        Line::from(vec![
            Span::styled("  [c] ", Style::default().fg(Color::Green).add_modifier(Modifier::BOLD)),
            Span::styled("Command launcher", Style::default().fg(Color::White)),
        ]),
        Line::from(vec![
            Span::styled("  [m] ", Style::default().fg(Color::Cyan).add_modifier(Modifier::BOLD)),
            Span::styled("Browse modules", Style::default().fg(Color::White)),
        ]),
        Line::from(vec![
            Span::styled("  [h] ", Style::default().fg(Color::Yellow).add_modifier(Modifier::BOLD)),
            Span::styled("Back to list", Style::default().fg(Color::White)),
        ]),
        Line::from(Span::raw("")),
        Line::from(Span::styled("  Legend", Style::default().fg(Color::DarkGray).add_modifier(Modifier::BOLD))),
        Line::from(Span::raw("")),
        Line::from(vec![Span::styled("  ● ", Style::default().fg(Color::Green)),   Span::styled("module active",    Style::default().fg(Color::DarkGray))]),
        Line::from(vec![Span::styled("  ○ ", Style::default().fg(Color::DarkGray)), Span::styled("module idle",      Style::default().fg(Color::DarkGray))]),
        Line::from(vec![Span::styled("  ⬆ ", Style::default().fg(Color::Magenta)), Span::styled("elevated session", Style::default().fg(Color::DarkGray))]),
        Line::from(Span::raw("")),
        Line::from(Span::styled("  Kinds", Style::default().fg(Color::DarkGray).add_modifier(Modifier::BOLD))),
        Line::from(Span::raw("")),
        Line::from(vec![
            Span::styled("  recon   ", Style::default().fg(Color::Cyan)),
            Span::styled("persist", Style::default().fg(Color::Magenta)),
        ]),
        Line::from(vec![
            Span::styled("  exfil   ", Style::default().fg(Color::Yellow)),
            Span::styled("tunnel",  Style::default().fg(Color::Blue)),
        ]),
    ];
    frame.render_widget(
        Paragraph::new(lines)
            .block(Block::default().borders(Borders::ALL)
                .border_style(Style::default().fg(Color::DarkGray))
                .title(Span::styled(" Actions ", Style::default().fg(Color::Cyan)))),
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
        let line = Line::from(vec![
            Span::raw("  "),
            Span::styled(format!("{:<15}", cmd), base.patch(if sel { Style::default() } else { Style::default().fg(Color::Green) })),
            Span::raw(" "),
            Span::styled(*desc, base.patch(if sel { Style::default() } else { Style::default().fg(Color::DarkGray) })),
        ]);
        ListItem::new(line).style(base)
    }).collect();

    frame.render_stateful_widget(
        List::new(items)
            .block(Block::default()
                .borders(Borders::ALL)
                .border_type(BorderType::Double)
                .border_style(Style::default().fg(Color::Green).add_modifier(Modifier::BOLD))
                .title(Span::styled(title, Style::default().fg(Color::Green).add_modifier(Modifier::BOLD)))
                .title_bottom(Span::styled(
                    " Enter dispatch  ·  Esc / q close ",
                    Style::default().fg(Color::DarkGray),
                )))
            .highlight_symbol("▶ "),
        popup_area,
        &mut app.cmd_state,
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
        terminal.draw(|frame| draw(frame, &mut app))?;
        if event::poll(Duration::from_millis(50))? {
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