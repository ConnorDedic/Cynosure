# Cynosure EDR Agent Implant Build Configuration
# Targets: Windows (PE64), Linux (ELF), macOS (Mach-O)

# Default target
.PHONY: all clean windows linux

# Output directory
OUTPUT_DIR := output
IMPLANT_DIR := src/implant

# Windows (x86_64-w64-mingw32) build
WINDOWS_CC := x86_64-w64-mingw32-gcc
WINDOWS_CFLAGS := -O2 -s -pthread
WINDOWS_LIBS := -lgdi32 -luser32 -lws2_32 -lkernel32 -lshell32 -lpthread -lpsapi
WINDOWS_DEFS := -DCB_IP=\"10.3.23.23\" -DCB_PORT=4444

# Linux (native gcc) build
LINUX_CC := gcc
LINUX_CFLAGS := -O2 -s
LINUX_LIBS := -lpthread -ldl

# Default: Windows
all: windows

# Create output directory
$(OUTPUT_DIR):
	mkdir -p $(OUTPUT_DIR)

# Windows PE64 executable
windows: $(OUTPUT_DIR)
	$(WINDOWS_CC) $(WINDOWS_CFLAGS) \
		-I $(IMPLANT_DIR) \
		$(WINDOWS_DEFS) \
		-o $(OUTPUT_DIR)/implant.exe \
		$(IMPLANT_DIR)/edr_agent.c \
		$(IMPLANT_DIR)/edr_dispatcher.c \
		$(WINDOWS_LIBS)
	@echo "[+] Windows implant built: $(OUTPUT_DIR)/implant.exe"
	@ls -lh $(OUTPUT_DIR)/implant.exe

# Linux ELF64 executable (native)
linux: $(OUTPUT_DIR)
	$(LINUX_CC) $(LINUX_CFLAGS) \
		-I $(IMPLANT_DIR) \
		-o $(OUTPUT_DIR)/implant.elf \
		$(IMPLANT_DIR)/edr_agent.c \
		$(IMPLANT_DIR)/edr_dispatcher.c \
		$(LINUX_LIBS)
	@echo "[+] Linux implant built: $(OUTPUT_DIR)/implant.elf"
	@ls -lh $(OUTPUT_DIR)/implant.elf

# Clean build artifacts
clean:
	rm -rf $(OUTPUT_DIR)/implant.exe $(OUTPUT_DIR)/implant.elf

.PHONY: all clean windows linux
