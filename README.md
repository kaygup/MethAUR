# methaur

A lightweight, C-based AUR helper for Arch Linux, inspired by yay.

## Features

- Search for packages in the AUR
- Install packages from the AUR or official repositories
- Remove installed packages
- Interactive selection interface similar to yay
- Fast and lightweight C implementation

## Requirements

- C compiler (gcc/clang)
- cmake
- make
- curl and libcurl development headers
- json-c library and development headers
- Arch Linux (pacman)

## Installation

### From source

1. Clone the repository:
```bash
git clone https://github.com/kaygup/methaur.git
cd methaur
```
2. build and install:
```bash
mkdir build && cd build
cmake ..
make
sudo make install
```

## Usage

```
Usage: methaur [options] [package]
Options:
  -S, --sync       Search and install package (default action)
  -R, --remove     Remove package
  -h, --help       Show this help message

Examples:
  methaur firefox     Search and choose firefox packages to install
  methaur -S firefox  Same as above
  methaur -R firefox  Remove firefox package
```

## Directory Structure

```
methaur/
├── CMakeLists.txt
├── cmake/
│   └── cmake_uninstall.cmake.in
├── install.sh
├── README.md
└── src/
    └── methaur.c
```

## How It Works

1. **Searching for packages**: methaur uses the AUR RPC API to search for packages matching your query.
2. **Installing packages**: 
   - Checks if the package exists in official repositories first
   - If not found, downloads the package from AUR
   - Extracts the package and builds it using makepkg
   - Installs the package
3. **Removing packages**: Uses pacman to remove installed packages

## Contributing

Contributions are welcome! Please feel free to submit a Pull Request.

## License

This project is licensed under the MIT License - see the LICENSE file for details.
