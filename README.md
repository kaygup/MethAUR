# methaur

An AUR helper written in C, heavily inspired by yay
## Requirements

- C compiler (gcc/clang)
- cmake
- make
- curl and libcurl development headers
- json-c library and development headers
- Arch Linux (pacman)

## Installation

1. Clone the repository:
```bash
git clone https://github.com/kaygup/MethAUR.git
cd MethAUR
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
  (also works without options)
```

