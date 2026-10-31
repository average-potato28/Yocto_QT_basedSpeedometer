require recipes-core/images/core-image-base.bb

IMAGE_INSTALL:append = " \
    qtbase \
    qtbase-plugins \
    qtdeclarative \
    packagegroup-rpi-security \
    gpio-qt-demo \
    can-app \
    i2c-tools \
    libgpiod \
    libgpiod-tools \
    systemd-analyze \
    boot-tuning \
"
IMAGE_FEATURES:remove = "splash"

# Mask services the app doesn't use (what you disabled by hand on the device)
mask_unused_services() {
    install -d ${IMAGE_ROOTFS}${sysconfdir}/systemd/system
    for u in ofono.service avahi-daemon.service avahi-daemon.socket \
             rpcbind.service rpcbind.socket alsa-restore.service alsa-state.service; do
        ln -sf /dev/null ${IMAGE_ROOTFS}${sysconfdir}/systemd/system/$u
    done
}
ROOTFS_POSTPROCESS_COMMAND += "mask_unused_services;"