SUMMARY = "CAN vehicle data application"
DESCRIPTION = "Reads CAN bus data and provides vehicle telemetry to the dashboard application"
LICENSE = "CLOSED"

SRC_URI = " \
    file://main.cpp \
    file://can-app.service \
    file://vehicle-data.h  \
"

S = "${WORKDIR}"

DEPENDS = ""

inherit pkgconfig systemd

SYSTEMD_SERVICE:${PN} = "can-app.service"
SYSTEMD_AUTO_ENABLE = "enable"

do_compile() {
    ${CXX} ${CXXFLAGS} ${LDFLAGS} \
        ${S}/main.cpp \
        -o ${S}/can-app
}

do_install() {
    install -d ${D}${bindir}
    install -m 0755 ${S}/can-app ${D}${bindir}/can-app

    install -d ${D}${systemd_system_unitdir}
    install -m 0644 ${S}/can-app.service \
        ${D}${systemd_system_unitdir}/can-app.service
}

FILES:${PN} += " \
    ${bindir}/can-app \
    ${systemd_system_unitdir}/can-app.service \
"
