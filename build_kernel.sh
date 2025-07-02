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
ROOT_DIR="${PWD}"
RUSTFLAGS="-C target-feature=-crt-static"
PKGS="busybox busybox-extras libxkbcommon eudev libinput libgcc musl mtdev libevdev openssl dropbear dropbear-ssh dropbear-scp openssh-sftp-server fontconfig"
EXTRA_PKGS=(python3 py3-numpy)
INITRD_DIR="${PWD}/initrd"
PKGS_DIR="${PWD}/pkgs"
PKGS_ARCHIVE_DIR="${PKGS_DIR}/archive"
PKGS_ARCHIVE="pkgs.sqsh"
ARCH="aarch64"

# CHECKS #
[ ! -e "private.pem" ] && openssl genrsa -out private.pem 2048 && openssl rsa -in private.pem -out public.pem -outform PEM -pubout
[ ! -e "quill-init" ] && git clone https://github.com/PorQ-Pine/quill-init
[ ! -e "alpine-chroot-install" ] && git clone https://github.com/alpinelinux/alpine-chroot-install

# INIT PROGRAM COMPILATION #
if [ "${INIT_DEBUG}" == 1 ]; then
	pushd quill-init; env RUSTFLAGS="${RUSTFLAGS}" cargo build --release --features "debug free_roam"; popd
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
# find "${INITRD_DIR}" -type f -exec upx {} \;

# EXTRA PACKAGES SETUP #
APK_URI="$(cat alpine-chroot-install/alpine-chroot-install | sed -n '/: ${APK_TOOLS_URI/p' | awk -F '"' '{ print $2 }')"
rm -rf "${PKGS_DIR}"
mkdir -p "${PKGS_ARCHIVE_DIR}" && pushd "${PKGS_ARCHIVE_DIR}"
wget "${APK_URI}" -O ../apk && chmod +x ../apk
../apk fetch -R ${EXTRA_PKGS[@]}
mksquashfs . "../${PKGS_ARCHIVE}" -no-compression && openssl dgst -sha256 -sign "${ROOT_DIR}/private.pem" -out "../${PKGS_ARCHIVE}.dgst" "../${PKGS_ARCHIVE}"
popd

# KERNEL COMPILATION #
[ -z "${THREADS}" ] && THREADS=1
git rev-parse --short HEAD > initrd_base/.commit
rm -rf initrd_base/lib
[ -z "${DIRTY}" ] && make distclean
make pinenote_defconfig
sed -i 's/\(CONFIG_CMDLINE=".*\)\("\)/\1 '"$(cat public.pem | base64 | tr -d '\n')"'"/' .config
make -j${THREADS}
make modules_install INSTALL_MOD_PATH="$PWD/initrd_base/"
make # This is not a typo
