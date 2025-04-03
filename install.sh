#!/bin/bash

# methaur installer script
set -e

# Check if running as root
if [ "$EUID" -eq 0 ]; then
  echo "Please don't run as root"
  exit 1
fi

# Check for required commands
for cmd in cmake gcc make curl; do
  if ! command -v $cmd &> /dev/null; then
    echo "Error: $cmd is required but not installed"
    exit 1
  fi
done

# Check for required libraries
echo "Checking for required libraries..."
if ! pkg-config --exists json-c; then
  echo "Error: json-c library not found"
  echo "Please install json-c development package"
  echo "Example: sudo pacman -S json-c"
  exit 1
fi

# Create directory structure
echo "Creating directory structure..."
mkdir -p build
mkdir -p src
mkdir -p cmake

# Copy files to appropriate locations
echo "Setting up project files..."

# Create source directory and copy main file
if [ ! -f "src/methaur.c" ]; then
  cat > src/methaur.c << 'EOF'
/* methaur.c content goes here */
/* Please replace this with the actual content of the methaur.c file */
EOF
fi

# Create CMakeLists.txt
if [ ! -f "CMakeLists.txt" ]; then
  cat > CMakeLists.txt << 'EOF'
# CMakeLists.txt content goes here
# Please replace this with the actual content of the CMakeLists.txt file
EOF
fi

# Create cmake uninstall template
if [ ! -f "cmake/cmake_uninstall.cmake.in" ]; then
  mkdir -p cmake
  cat > cmake/cmake_uninstall.cmake.in << 'EOF'
# cmake_uninstall.cmake.in content goes here
# Please replace this with the actual content of the cmake/cmake_uninstall.cmake.in file
EOF
fi

# Build and install
echo "Building methaur..."
cd build
cmake ..
make -j$(nproc)

echo "Installing methaur..."
sudo make install

echo "methaur has been successfully installed!"
echo "You can now use 'methaur' from the command line."