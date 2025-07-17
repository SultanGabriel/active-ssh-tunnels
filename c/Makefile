# Chief Tunnel Officer - SSH Tunnel Manager
# State-of-the-art C program for persistent SSH tunnel management

CC = gcc
CFLAGS = -Wall -Wextra -std=c99 -pthread -O2 -g
LDFLAGS = -pthread

# Target executable
TARGET = tunnel_manager

# Source files
SOURCES = main.c
TEST_SOURCES = test.c

# cJSON library (embedded)
CJSON_DIR = cjson
CJSON_SRC = $(CJSON_DIR)/cJSON.c
CJSON_OBJ = $(CJSON_DIR)/cJSON.o

# All sources including cJSON
ALL_SOURCES = $(SOURCES) $(CJSON_SRC)

# Object files
OBJECTS = $(SOURCES:.c=.o) $(CJSON_OBJ)
TEST_OBJECTS = $(TEST_SOURCES:.c=.o)

# Executables
TEST_TARGET = test_tunnel_manager

# External libraries
LIBS = 

# Platform-specific settings
UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Linux)
    LIBS += -lncurses
    CFLAGS += -DLINUX
endif
ifeq ($(UNAME_S),Darwin)
    LIBS += -lncurses
    CFLAGS += -DMACOS
endif
ifeq ($(OS),Windows_NT)
    LIBS += -lws2_32
    CFLAGS += -DWINDOWS
    TARGET := $(TARGET).exe
endif

# Include directories
INCLUDES = -I$(CJSON_DIR) -I.

# Default target
all: $(TARGET)

# Create target executable
$(TARGET): $(OBJECTS)
	@echo "Linking $(TARGET)..."
	$(CC) $(OBJECTS) -o $(TARGET) $(LDFLAGS) $(LIBS)
	@echo "Build complete: $(TARGET)"

# Test target
test: $(TEST_TARGET)

$(TEST_TARGET): $(TEST_OBJECTS)
	@echo "Linking $(TEST_TARGET)..."
	$(CC) $(TEST_OBJECTS) -o $(TEST_TARGET) $(LDFLAGS)
	@echo "Test build complete: $(TEST_TARGET)"

# Compile source files
%.o: %.c
	@echo "Compiling $<..."
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

# Setup cJSON library
setup-cjson:
	@echo "Setting up cJSON library..."
	@mkdir -p $(CJSON_DIR)
	@if [ ! -f $(CJSON_DIR)/cJSON.c ]; then \
		echo "Downloading cJSON..."; \
		curl -L -o $(CJSON_DIR)/cJSON.c https://raw.githubusercontent.com/DaveGamble/cJSON/master/cJSON.c; \
		curl -L -o $(CJSON_DIR)/cJSON.h https://raw.githubusercontent.com/DaveGamble/cJSON/master/cJSON.h; \
		echo "cJSON downloaded successfully"; \
	else \
		echo "cJSON already exists"; \
	fi

# Install dependencies (Linux/macOS)
install-deps:
	@echo "Installing dependencies..."
ifeq ($(UNAME_S),Linux)
	@echo "Linux detected - install ncurses-dev if needed:"
	@echo "  Ubuntu/Debian: sudo apt-get install libncurses5-dev"
	@echo "  CentOS/RHEL:   sudo yum install ncurses-devel"
	@echo "  Arch:          sudo pacman -S ncurses"
endif
ifeq ($(UNAME_S),Darwin)
	@echo "macOS detected - install ncurses if needed:"
	@echo "  brew install ncurses"
endif
ifeq ($(OS),Windows_NT)
	@echo "Windows detected - ensure MinGW/MSYS2 is installed"
endif

# Create example config
config:
	@echo "Creating example config.json..."
	@echo '{' > config.json
	@echo '  "tunnels": [' >> config.json
	@echo '    {' >> config.json
	@echo '      "name": "db-prod",' >> config.json
	@echo '      "host": "db.example.com",' >> config.json
	@echo '      "port": 22,' >> config.json
	@echo '      "user": "admin",' >> config.json
	@echo '      "local_port": 3307,' >> config.json
	@echo '      "remote_host": "127.0.0.1",' >> config.json
	@echo '      "remote_port": 3306,' >> config.json
	@echo '      "reconnect_delay": 5' >> config.json
	@echo '    },' >> config.json
	@echo '    {' >> config.json
	@echo '      "name": "web-staging",' >> config.json
	@echo '      "host": "staging.example.com",' >> config.json
	@echo '      "port": 22,' >> config.json
	@echo '      "user": "deploy",' >> config.json
	@echo '      "local_port": 8080,' >> config.json
	@echo '      "remote_host": "localhost",' >> config.json
	@echo '      "remote_port": 80,' >> config.json
	@echo '      "reconnect_delay": 3' >> config.json
	@echo '    }' >> config.json
	@echo '  ]' >> config.json
	@echo '}' >> config.json
	@echo "Example config.json created"

# Setup complete project
setup: setup-cjson config
	@echo ""
	@echo "=========================================="
	@echo "Chief Tunnel Officer - Setup Complete"
	@echo "=========================================="
	@echo ""
	@echo "Next steps:"
	@echo "1. Edit config.json with your SSH tunnels"
	@echo "2. Run 'make' to build"
	@echo "3. Run './$(TARGET)' to start"
	@echo ""
	@echo "Commands available:"
	@echo "  make setup      - Download dependencies and create config"
	@echo "  make           - Build the tunnel manager"
	@echo "  make install-deps - Show dependency installation instructions"
	@echo "  make clean     - Clean build files"
	@echo "  make run       - Build and run with default config"
	@echo "  make debug     - Build with debug symbols"
	@echo ""

# Clean build files
clean:
	@echo "Cleaning build files..."
	rm -f $(OBJECTS) $(TEST_OBJECTS) $(TARGET) $(TEST_TARGET)
	@echo "Clean complete"

# Clean everything including cJSON
distclean: clean
	@echo "Cleaning everything..."
	rm -rf $(CJSON_DIR) config.json logs/
	@echo "Distclean complete"

# Debug build
debug: CFLAGS += -DDEBUG -g3 -O0
debug: $(TARGET)

# Release build
release: CFLAGS += -DNDEBUG -O3
release: clean $(TARGET)

# Run tests
test-run: $(TEST_TARGET)
	@echo "Running unit tests..."
	./$(TEST_TARGET)

# Run the program
run: $(TARGET)
	@echo "Starting Chief Tunnel Officer..."
	./$(TARGET)

# Install systemd service (Linux only)
install-service:
ifeq ($(UNAME_S),Linux)
	@echo "Creating systemd service..."
	@echo "[Unit]" > tunnel-manager.service
	@echo "Description=Chief Tunnel Officer - SSH Tunnel Manager" >> tunnel-manager.service
	@echo "After=network.target" >> tunnel-manager.service
	@echo "" >> tunnel-manager.service
	@echo "[Service]" >> tunnel-manager.service
	@echo "Type=simple" >> tunnel-manager.service
	@echo "User=$$USER" >> tunnel-manager.service
	@echo "WorkingDirectory=$$PWD" >> tunnel-manager.service
	@echo "ExecStart=$$PWD/$(TARGET)" >> tunnel-manager.service
	@echo "Restart=always" >> tunnel-manager.service
	@echo "RestartSec=5" >> tunnel-manager.service
	@echo "" >> tunnel-manager.service
	@echo "[Install]" >> tunnel-manager.service
	@echo "WantedBy=multi-user.target" >> tunnel-manager.service
	@echo ""
	@echo "Service file created: tunnel-manager.service"
	@echo "To install: sudo cp tunnel-manager.service /etc/systemd/system/"
	@echo "To enable:  sudo systemctl enable tunnel-manager"
	@echo "To start:   sudo systemctl start tunnel-manager"
else
	@echo "Systemd service only available on Linux"
endif

# Help
help:
	@echo "Chief Tunnel Officer - SSH Tunnel Manager"
	@echo "========================================"
	@echo ""
	@echo "Available targets:"
	@echo "  setup          - Download cJSON and create example config"
	@echo "  all (default)  - Build the tunnel manager"
	@echo "  test           - Build unit tests"
	@echo "  test-run       - Build and run unit tests"
	@echo "  clean          - Remove build files"
	@echo "  distclean      - Remove everything (build files, cJSON, config)"
	@echo "  debug          - Build with debug symbols"
	@echo "  release        - Build optimized release version"
	@echo "  run            - Build and run the tunnel manager"
	@echo "  config         - Create example config.json"
	@echo "  install-deps   - Show dependency installation instructions"
	@echo "  install-service- Create systemd service (Linux only)"
	@echo "  help           - Show this help"
	@echo ""
	@echo "Example usage:"
	@echo "  make setup     # First time setup"
	@echo "  make           # Build"
	@echo "  make test-run  # Run unit tests"
	@echo "  make run       # Run"

.PHONY: all clean distclean debug release run test test-run setup setup-cjson config install-deps install-service help
