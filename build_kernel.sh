#!/bin/sh

[ -z "${THREADS}" ] && THREADS=1
git rev-parse --short HEAD > initrd_base/.commit
rm -rf initrd_base/lib
make distclean
make pinenote_defconfig
make -j${THREADS}
make modules_install INSTALL_MOD_PATH="$PWD/initrd_base/"
make # This is not a typo
