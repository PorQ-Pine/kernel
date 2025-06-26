#!/bin/bash -x

# https://unix.stackexchange.com/a/216174
umount_recursive() {
	for dir in $(grep "${1}" /proc/mounts | cut -f2 -d" " | sort -r); do
		sudo umount "${dir}" 2> /dev/null
		(( ${?} )) && sudo umount -n "${dir}"
	done
}

# CONSTANTS #
cd "$(dirname "$0")"
RUSTFLAGS="-C target-feature=-crt-static"
PKGS="busybox busybox-extras libxkbcommon eudev libinput libgcc musl mtdev libevdev openssl"
INITRD_DIR="${PWD}/initrd"
ARCH="aarch64"

# CHECKS #
[ ! -e "private.pem" ] && openssl genrsa -out private.pem 2048 && openssl rsa -in private.pem -out public.pem -outform PEM -pubout
[ ! -e "quill-init" ] && git clone https://github.com/PorQ-Pine/quill-init
[ ! -e "alpine-chroot-install" ] && git clone https://github.com/alpinelinux/alpine-chroot-install

# INIT PROGRAM COMPILATION #
if [ "${INIT_DEBUG}" == 1 ]; then
	pushd quill-init; env RUSTFLAGS="${RUSTFLAGS}" cargo build --release --features debug; popd
else
	pushd quill-init; env RUSTFLAGS="${RUSTFLAGS}" cargo build --release; popd
fi
cp quill-init/target/release/quill-init initrd_base/etc/init.d/quill-init

# ALPINE ROOTFS SETUP #
# Sorry, it seems like running it as root is the only way to make it work...
umount_recursive "${INITRD_DIR}"
sudo rm -rf "${INITRD_DIR}"
sudo alpine-chroot-install/alpine-chroot-install -d "${INITRD_DIR}" -p "${PKGS}" -a "${ARCH}"
umount_recursive "${INITRD_DIR}"
sudo chown -R "${USER}:${USER}" "${INITRD_DIR}"
sudo chmod 555 "${INITRD_DIR}/bin/bbsuid"
rm "${INITRD_DIR}/env.sh" "${INITRD_DIR}/destroy" "${INITRD_DIR}/enter-chroot" "${INITRD_DIR}/etc/motd"
sed -i 's/Welcome.*/Welcome to Quill OS recovery/g' "${INITRD_DIR}/etc/issue"

# KERNEL COMPILATION #
[ -z "${THREADS}" ] && THREADS=1
git rev-parse --short HEAD > initrd_base/.commit
rm -rf initrd_base/lib
make distclean
make pinenote_defconfig
sed -i 's/\(CONFIG_CMDLINE=".*\)\("\)/\1 '"$(cat public.pem | base64 | tr -d '\n')"'"/' .config
make -j${THREADS}
make modules_install INSTALL_MOD_PATH="$PWD/initrd_base/"
make # This is not a typo
