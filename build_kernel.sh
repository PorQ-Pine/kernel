#!/bin/bash -x

[ ! -e "private.pem" ] && openssl genrsa -out private.pem 2048 && openssl rsa -in private.pem -out public.pem -outform PEM -pubout

if [ "${INIT_DEBUG}" == 1 ]; then
	pushd quill-init; cargo build --release --features debug; popd
else
	pushd quill-init; cargo build --release; popd
fi

cp quill-init/target/release/quill-init initrd_base/etc/init.d/quill-init

[ -z "${THREADS}" ] && THREADS=1
git rev-parse --short HEAD > initrd_base/.commit
rm -rf initrd_base/lib
make distclean
make pinenote_defconfig
sed -i 's/\(CONFIG_CMDLINE=".*\)\("\)/\1 '"$(cat public.pem | base64 | tr -d '\n')"'"/' .config
make -j${THREADS}
make modules_install INSTALL_MOD_PATH="$PWD/initrd_base/"
make # This is not a typo
