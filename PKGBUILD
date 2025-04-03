# Maintainer: Your Name <your.email@example.com>

pkgname=methaur
pkgver=0.1.0
pkgrel=1
pkgdesc="A lightweight AUR helper written in C"
arch=('x86_64')
url="https://github.com/yourusername/methaur"
license=('MIT')
depends=('curl' 'json-c' 'readline' 'git')
makedepends=('cmake' 'gcc')
source=("$pkgname-$pkgver.tar.gz::$url/archive/v$pkgver.tar.gz")
sha256sums=('SKIP')

build() {
    cd "$pkgname-$pkgver"
    mkdir -p build
    cd build
    cmake ..
    make
}

package() {
    cd "$pkgname-$pkgver/build"
    make DESTDIR="$pkgdir" install
}