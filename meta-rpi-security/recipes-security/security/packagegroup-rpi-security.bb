SUMMARY = "Basic security packages"

PACKAGE_ARCH = "${MACHINE_ARCH}"

inherit packagegroup

RDEPENDS:${PN} = " \
    openssh \
    openssl \
    ca-certificates \
"
