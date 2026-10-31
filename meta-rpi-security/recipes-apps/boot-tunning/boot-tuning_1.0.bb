SUMMARY = "Boot-time tuning: module blacklist and usb0 static address"
LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/MIT;md5=0835ade698e0bcf8506ecda2f7b4f302"

SRC_URI = "file://no-vc-media.conf file://10-usb0.network"
S = "${WORKDIR}"

do_install() {
    install -d ${D}${sysconfdir}/modprobe.d ${D}${sysconfdir}/systemd/network
    install -m 0644 ${WORKDIR}/no-vc-media.conf ${D}${sysconfdir}/modprobe.d/
    install -m 0644 ${WORKDIR}/10-usb0.network ${D}${sysconfdir}/systemd/network/
}
FILES:${PN} = "${sysconfdir}/modprobe.d ${sysconfdir}/systemd/network"