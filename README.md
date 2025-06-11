

 ▄████▄▓██   ██▓ ███▄    █  ▒█████    ██████  █    ██  ██▀███  ▓█████ 
▒██▀ ▀█ ▒██  ██▒ ██ ▀█   █ ▒██▒  ██▒▒██    ▒  ██  ▓██▒▓██ ▒ ██▒▓█   ▀ 
▒▓█    ▄ ▒██ ██░▓██  ▀█ ██▒▒██░  ██▒░ ▓██▄   ▓██  ▒██░▓██ ░▄█ ▒▒███   
▒▓▓▄ ▄██▒░ ▐██▓░▓██▒  ▐▌██▒▒██   ██░  ▒   ██▒▓▓█  ░██░▒██▀▀█▄  ▒▓█  ▄ 
▒ ▓███▀ ░░ ██▒▓░▒██░   ▓██░░ ████▓▒░▒██████▒▒▒▒█████▓ ░██▓ ▒██▒░▒████▒
░ ░▒ ▒  ░ ██▒▒▒ ░ ▒░   ▒ ▒ ░ ▒░▒░▒░ ▒ ▒▓▒ ▒ ░░▒▓▒ ▒ ▒ ░ ▒▓ ░▒▓░░░ ▒░ ░
  ░  ▒  ▓██ ░▒░ ░ ░░   ░ ▒░  ░ ▒ ▒░ ░ ░▒  ░ ░░░▒░ ░ ░   ░▒ ░ ▒░ ░ ░  ░
░       ▒ ▒ ░░     ░   ░ ░ ░ ░ ░ ▒  ░  ░  ░   ░░░ ░ ░   ░░   ░    ░   
░ ░     ░ ░              ░     ░ ░        ░     ░        ░        ░  ░
░       ░ ░                                                           






## Adaptive C2 Channel with AI Obfuscation

This project develops an intelligent Command and Control (C2) system designed to dynamically alter its communication patterns, aiming to evade detection by security systems. By integrating basic AI/ML principles, the C2 channel will learn from "detection feedback" and adapt its obfuscation methods in real time, enhancing its resilience and stealth in simulated red team scenarios.
Project Outline

#### The project progresses through distinct development phases:

    Phase 1: Foundation - Basic C2 Communication (Go)
        Establishes fundamental, un-obfuscated client-server communication using Go.
    Phase 2: Introducing Obfuscation (Go)
        Implements static obfuscation techniques (e.g., Base64, XOR) within the Go C2 client and server.
    Phase 3: AI/Adaptive Decision Making (Python)
        Develops an intelligence layer in Python using simple AI/ML techniques (Finite State Machines, Rule-Based Systems, Basic Statistical Tracking) to guide obfuscation choices based on simulated detection.
    Phase 4: Integration & Dynamic Adaptation (Python + Go + Bash)
        Connects the Python AI with the Go C2, allowing the obfuscation method to change dynamically during a session. Bash scripts orchestrate the entire setup.


