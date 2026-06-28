# Cynosure C2

## Structure
- Command and Control
    - CLI UI
    - API for interfacing with modules and implants
- Implant
    - Windows only (for now)
    - Basic API if custom module isn't loaded
- ML Model
    - Reinforcement Learning
## Modules
- Comm
    - DNS
    - HTTPS
    - Github Image Steganography
- Evasion
    - Syscall
        - Direct
        - Indirect
        - Hell's Gate
    - Obfuscation
        - Encrypted Memory Loader

# Functional Requirements

1. Provide a C2 system with an interface, communication and upgradeability

2. Utilize a basic RL model that allows the C2 to be trained on which modules to use in a given situation 

3. Utilize a modular polymorphic implant allowing a user to control actions while allowing the C2 to control how the actions are performed

4. Demonstrate the C2 can make independent choices

5. Include 3 comm modules, and 3 shellcode execution modules

6. Be able to bypass defender


<img width="1917" height="1003" alt="Main C2 TUI" src="https://github.com/user-attachments/assets/c68476d0-f4a1-4bcf-a141-9282b45ecadd" />
- Main C2 TUI


<img width="1917" height="1003" alt="Shell Drilldown" src="https://github.com/user-attachments/assets/b97f0137-bf48-4fe6-b328-94c4864238f0" />
- Shell Drilldown


<img width="1917" height="1003" alt="Command Execution Menu" src="https://github.com/user-attachments/assets/2d94fee7-2c30-491a-9382-bf489d5a5da5" />
- Command Execution Menu
