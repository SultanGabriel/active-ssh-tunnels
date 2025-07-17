#!/bin/bash
# Chief Tunnel Officer - Installation Script

set -e

echo "========================================"
echo "Chief Tunnel Officer - Installation"
echo "========================================"
echo ""

# Check for required tools
check_dependencies() {
    echo "Checking dependencies..."
    
    if ! command -v gcc &> /dev/null; then
        echo "Error: gcc is required but not installed."
        echo "Install it using your package manager:"
        echo "  Ubuntu/Debian: sudo apt-get install build-essential"
        echo "  CentOS/RHEL:   sudo yum groupinstall 'Development Tools'"
        echo "  macOS:         xcode-select --install"
        exit 1
    fi
    
    if ! command -v make &> /dev/null; then
        echo "Error: make is required but not installed."
        exit 1
    fi
    
    echo "✓ Build tools found"
}

# Install system dependencies
install_system_deps() {
    echo ""
    echo "Installing system dependencies..."
    
    if [[ "$OSTYPE" == "linux-gnu"* ]]; then
        if command -v apt-get &> /dev/null; then
            sudo apt-get update
            sudo apt-get install -y libncurses5-dev
        elif command -v yum &> /dev/null; then
            sudo yum install -y ncurses-devel
        elif command -v pacman &> /dev/null; then
            sudo pacman -S --noconfirm ncurses
        else
            echo "Warning: Unknown Linux distribution. Install ncurses-dev manually."
        fi
    elif [[ "$OSTYPE" == "darwin"* ]]; then
        if command -v brew &> /dev/null; then
            brew install ncurses
        else
            echo "Warning: Homebrew not found. Install ncurses manually."
        fi
    fi
    
    echo "✓ System dependencies installed"
}

# Build the project
build_project() {
    echo ""
    echo "Building Chief Tunnel Officer..."
    
    make setup
    make release
    
    echo "✓ Build completed"
}

# Create service user (Linux only)
create_service_user() {
    if [[ "$OSTYPE" == "linux-gnu"* ]] && [[ $EUID -eq 0 ]]; then
        echo ""
        echo "Creating service user..."
        
        if ! id -u tunnel &>/dev/null; then
            useradd -r -s /bin/false -d /opt/tunnel-manager tunnel
            echo "✓ Service user 'tunnel' created"
        else
            echo "✓ Service user 'tunnel' already exists"
        fi
    fi
}

# Install system-wide (requires root)
install_system_wide() {
    if [[ $EUID -ne 0 ]]; then
        echo ""
        echo "To install system-wide, run this script with sudo:"
        echo "  sudo ./install.sh"
        echo ""
        echo "Or install manually:"
        echo "  sudo mkdir -p /opt/tunnel-manager"
        echo "  sudo cp tunnel_manager config.json /opt/tunnel-manager/"
        echo "  sudo cp tunnel-manager.service /etc/systemd/system/"
        echo "  sudo systemctl enable tunnel-manager"
        return
    fi
    
    echo ""
    echo "Installing system-wide..."
    
    # Create installation directory
    mkdir -p /opt/tunnel-manager
    mkdir -p /opt/tunnel-manager/logs
    
    # Copy files
    cp tunnel_manager /opt/tunnel-manager/
    cp config.json /opt/tunnel-manager/
    cp README.md /opt/tunnel-manager/
    
    # Set permissions
    chown -R tunnel:tunnel /opt/tunnel-manager
    chmod +x /opt/tunnel-manager/tunnel_manager
    
    # Install systemd service
    cp tunnel-manager.service /etc/systemd/system/
    systemctl daemon-reload
    systemctl enable tunnel-manager
    
    echo "✓ System-wide installation completed"
    echo ""
    echo "Service commands:"
    echo "  sudo systemctl start tunnel-manager"
    echo "  sudo systemctl status tunnel-manager"
    echo "  sudo systemctl logs -f tunnel-manager"
}

# Main installation
main() {
    check_dependencies
    install_system_deps
    build_project
    create_service_user
    install_system_wide
    
    echo ""
    echo "========================================"
    echo "Installation completed successfully!"
    echo "========================================"
    echo ""
    echo "Next steps:"
    echo "1. Edit /opt/tunnel-manager/config.json"
    echo "2. Start the service: sudo systemctl start tunnel-manager"
    echo "3. Check status: sudo systemctl status tunnel-manager"
    echo ""
    echo "Chief Tunnel Officer is ready for duty!"
}

# Run main function
main "$@"
