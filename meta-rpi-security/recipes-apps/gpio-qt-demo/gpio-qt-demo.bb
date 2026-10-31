SUMMARY = "Qt GPIO Demo"
LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/MIT;md5=0835ade698e0bcf8506ecda2f7b4f302"

SRC_URI = "file://main.cpp \
           file://gpio-qt-demo.pro \
           file://vehicle-data.h \
           file://gpio-qt-demo.service \
           file://vehicle-shm.h"

S = "${WORKDIR}"

inherit qmake5 systemd

DEPENDS += "qtbase"

SYSTEMD_SERVICE:${PN} = "gpio-qt-demo.service"
SYSTEMD_AUTO_ENABLE:${PN} = "enable"

do_install() {
    install -d ${D}${bindir}
    install -m 0755 gpio-qt-demo ${D}${bindir}/gpio-qt-demo

    install -d ${D}${systemd_system_unitdir}
    install -m 0644 ${WORKDIR}/gpio-qt-demo.service \
        ${D}${systemd_system_unitdir}/gpio-qt-demo.service
    
}

FILES:${PN} += " \
    ${bindir}/gpio-qt-demo \
    ${systemd_system_unitdir}/gpio-qt-demo.service \
"

RDEPENDS:${PN} += "qtbase-plugins"
