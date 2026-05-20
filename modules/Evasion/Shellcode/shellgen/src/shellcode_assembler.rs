use crate::syscalls::{WinAPICallerWrapper};
// Import the necessary AST types from main or a shared module
pub use crate::main::ActionType;


/// The central compiler engine that interprets AST nodes and converts them into raw bytes.
pub struct ShellcodeGenerator<'a> {
    context: &'a str, // Context for logging: "network", "fileio", etc.
}

impl<'a> ShellcodeGenerator<'a> {
    pub fn new(context: &'a str) -> Self {
        Self { context }
    }

    /// Takes a sequence of structured actions and compiles them into raw bytes.
    pub fn generate_payload(&self, action_sequence: &[ActionNode]) -> Result<Vec<u8>, String> {
        let mut payload: Vec<u8> = vec![];
        println!("\n[=== Compiler Start ===]");
        println!("-> Target Context: {}", self.context);

        // Use the concrete Windows API wrapper for all syscall generation
        let api_caller = WinAPICallerWrapper { inner: crate::syscalls::WindowsApiCaller {} };


        for (i, action) in action_sequence.iter().enumerate() {
            println!("-> Compiling Action {} ({})", i + 1, action.action_type);
            let result = match action.action_type {
                ActionType::FileRead => self.generate_file_read(action, &api_caller)?,
                ActionType::NetworkConnect => self.generate_network_connect(action, &api_caller)?,
                ActionType::ProcessSpawn => self.generate_process_spawn(action, &api_caller)?,
                ActionType::CredentialDump => self.generate_credential_dump(action, &api_caller)?
            };
            payload.extend(result);
        }
        println!("[=== Compiler End ===]");
        Ok(payload)
    }

    /// Generates shellcode bytes for reading a file (Uses CreateFile, ReadFile).
    fn generate_file_read(&self, action: &ActionNode, api: &WinAPICallerWrapper) -> Result<Vec<u8>, String> {
        // 1. Load path string into memory (requires LOAD_STRING primitive).
        let _path_bytes = api.set_handle("File Path", 0xDEADBEEF); 
        
        // 2. Generate 'CreateFile' syscall sequence
        let open_api_bytes = api.generate_code("CreateFileA")?;
        println!("  [Code Gen]: Calling CreateFileA API to get file handle...");

        // 3. Generate 'ReadFile' loop
        let read_api_bytes = api.generate_code("ReadFile")?; 
        println!("  [Code Gen]: Loop reading data using ReadFile() and writing buffer.");
        
        Ok(open_api_bytes.into_iter().chain(read_api_bytes).collect())
    }

    /// Generates shellcode bytes for network communication (socket creation/connection).
    fn generate_network_connect(&self, action: &ActionNode, api: &WinAPICallerWrapper) -> Result<Vec<u8>, String> {
        // 1. Generate 'socket()' syscall sequence.
        let socket_bytes = api.generate_code("socket")?; // Syscall number for socket()
        println!("  [Code Gen]: Creating raw TCP/UDP socket handle...");

        // 2. Generate 'connect()' sequence using provided IP and Port.
        let connect_bytes = api.generate_code("connect")?;
        println!("  [Code Gen]: Attempting connection to {}...", action.params.target);

        Ok(socket_bytes.into_iter().chain(connect_bytes).collect())
    }
    
    /// Generates shellcode bytes for executing a program (using CreateProcessA).
    fn generate_process_spawn(&self, action: &ActionNode, api: &WinAPICallerWrapper) -> Result<Vec<u8>, String> {
        // 1. Generate 'CreateProcess' API call sequence.
        let create_bytes = api.generate_code("CreateProcessA")?; 
        println!("  [Code Gen]: Preparing to execute process using CreateProcessA()...");
        
        Ok(vec![0xCC; 5]) // Dummy bytes
    }

    /// Generates shellcode bytes tailored for dumping credentials (e.g., reading LSASS).
    fn generate_credential_dump(&self, action: &ActionNode, api: &WinAPICallerWrapper) -> Result<Vec<u8>, String> {
        println!("  [Code Gen]: Calling OpenProcess() to get handle to the target PID...");
        // Simulates getting a process handle first.
        let _handle = api.set_handle("PROCESS", 0x1234);

        // Then uses ReadProcessMemory to dump data from that handle.
        let read_bytes = api.generate_code("ReadProcessMemory")?;
        println!("  [Code Gen]: Executing ReadProcessMemory() to extract memory region...");
        Ok(read_bytes)
    }
}


// ================================================================
// COMMAND INTERPRETATION (AST Definition)
// ================================================================

/// Defines the types of actions we can compile into shellcode.
#[derive(Debug)]
pub enum ActionType {
    FileRead,
    NetworkConnect,
    ProcessSpawn,
    CredentialDump, 
}

/// Represents a structured command parsed from text input. This is the AST node.
pub struct ActionNode {
    pub action_type: ActionType,
    pub params: CommandParams, 
}


struct CommandParams {
    target: &'static str,
    command: &'static str,
}

// ================================================================
// MAIN FUNCTION (Execution flow is the same)
// ================================================================
