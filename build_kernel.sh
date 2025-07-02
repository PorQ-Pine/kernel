#!/bin/bash -x

# https://unix.stackexchange.com/a/216174
umount_recursive() {
	for dir in $(grep "${1}" /proc/mounts | cut -f2 -d" " | sort -r); do
		sudo umount "${dir}" 2> /dev/null
		(( ${?} )) && sudo umount -n "${dir}"
	done
}

setup_alpine_chroot() {
	# Sorry, it seems like running it as root is the only way to make it work...
	umount_recursive "${1}"
	sudo rm -rf "${1}"
	sudo alpine-chroot-install/alpine-chroot-install -d "${1}" -p "${2}" -a "${3}"
	umount_recursive "${1}"
	sudo chown -R "${USER}:${USER}" "${1}"
	sudo chmod 555 "${1}/bin/bbsuid"
	rm "${1}/env.sh" "${1}/destroy" "${1}/enter-chroot" "${1}/etc/motd"
	sed -i 's/Welcome.*/Welcome to Quill OS recovery/g' "${1}/etc/issue"
}

sign() {
	openssl dgst -sha256 -sign "${ROOT_DIR}/private.pem" -out "${1}.dgst" "${1}"
}

#### BEGIN CONSTANTS ####
	cd "$(dirname "$0")"
	RUSTFLAGS="-C target-feature=-crt-static"
	ARCH="aarch64"

	ROOT_DIR="${PWD}"
	INITRD_DIR="${ROOT_DIR}/initrd"
	INITRD_BASE_DIR="${ROOT_DIR}/initrd_base"
	DATA_DIR="${ROOT_DIR}/data"
	RECOVERYFS_DIR="${DATA_DIR}/recoveryfs"
	RECOVERYFS_ARCHIVE="${DATA_DIR}/recoveryfs.squashfs"
	QUILL_INIT_DIR="quill-init"
	INIT_DIR="${QUILL_INIT_DIR}/qinit"
	RECOVERY_DIR="${QUILL_INIT_DIR}/qrecovery"

	INITRD_PKGS="busybox busybox-extras libxkbcommon eudev udev-init-scripts libinput libgcc musl mtdev libevdev openssl dropbear dropbear-ssh dropbear-scp openssh-sftp-server fontconfig openrc"
	RECOVERYFS_PKGS="${INITRD_PKGS} python3 py3-numpy mesa-gbm"
#### END CONSTANTS ####

#### BEGIN CHECKS ####
	[ ! -e "private.pem" ] && openssl genrsa -out private.pem 2048 && openssl rsa -in private.pem -out public.pem -outform PEM -pubout
	[ ! -e "quill-init" ] && git clone https://github.com/PorQ-Pine/quill-init
	[ ! -e "alpine-chroot-install" ] && git clone https://github.com/alpinelinux/alpine-chroot-install
#### END CHECKS ####

#### BEGIN INIT PROGRAMS COMPILATION ####
	pushd "${INIT_DIR}" && env RUSTFLAGS="${RUSTFLAGS}" cargo build --release --features "${INIT_FEATURES}" && popd
	pushd "${RECOVERY_DIR}" && env RUSTFLAGS="${RUSTFLAGS}" cargo build --release --features "${INIT_FEATURES}" && popd

	cp "${INIT_DIR}/target/release/qinit" "${INITRD_BASE_DIR}/etc/init.d/qinit"
#### END INIT PROGRAMS COMPILATION ####

#### BEGIN ALPINE ROOTFS SETUP ####
	setup_alpine_chroot "${INITRD_DIR}" "${INITRD_PKGS}" "${ARCH}"
#### END ALPINE ROOTFS SETUP ####

#### BEGIN RECOVERYFS SETUP ####
	setup_alpine_chroot "${RECOVERYFS_DIR}" "${RECOVERYFS_PKGS}" "${ARCH}"
	sudo chown -R "${USER}:${USER}" "${DATA_DIR}"
	cp "${RECOVERY_DIR}/target/release/qrecovery" "${RECOVERYFS_DIR}/sbin/qrecovery"
	mkdir -p "${RECOVERYFS_DIR}/usr/share/fonts" && cp -rv "${INITRD_BASE_DIR}/usr/share/fonts" "${RECOVERYFS_DIR}/usr/share"
	rm -f "${RECOVERYFS_ARCHIVE}"
	mksquashfs "${RECOVERYFS_DIR}" "${RECOVERYFS_ARCHIVE}" -comp zstd -b 32768 -Xcompression-level 22 && sign "${RECOVERYFS_ARCHIVE}"
#### END RECOVERYFS SETUP ####

#### BEGIN KERNEL COMPILATION ####
	[ -z "${THREADS}" ] && THREADS=1
	git rev-parse --short HEAD > initrd_base/.commit
	rm -rf initrd_base/lib
	[ -z "${DIRTY}" ] && make distclean
	make pinenote_defconfig
	sed -i 's/\(CONFIG_CMDLINE=".*\)\("\)/\1 '"$(cat public.pem | base64 | tr -d '\n')"'"/' .config
	make -j${THREADS}
	make modules_install INSTALL_MOD_PATH="$PWD/initrd_base/"
	make # This is not a typo
#### END KERNEL COMPILATION ####
