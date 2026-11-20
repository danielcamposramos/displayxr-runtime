// Copyright 2026 Daniel Campos Ramos
// SPDX-License-Identifier: BSL-1.0
// A same-user local boundary from KRdp's paired RDP session. No network listener.
#include "target_rdp_head.h"
#include "xrt/xrt_config_os.h"
#include "os/os_threading.h"
#include "util/u_device.h"
#include "util/u_logging.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(XRT_OS_LINUX_DESKTOP)
#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

struct rdp_head {
    struct xrt_device base;
    struct os_mutex mutex;
    int listener, peer;
    char path[108];
    uint8_t ids[64];
    bool registered, valid;
    uint64_t capture, origin;
    struct xrt_quat orientation;
};
static uint64_t get(const uint8_t *b, unsigned offset, unsigned width)
{
    uint64_t v = 0;
    for (unsigned i = 0; i < width; ++i) v |= (uint64_t)b[offset + i] << (8 * i);
    return v;
}
static uint64_t boottime(void)
{
    struct timespec t;
    if (clock_gettime(CLOCK_BOOTTIME, &t) != 0) return 0;
    return (uint64_t)t.tv_sec * 1000000000ULL + (uint64_t)t.tv_nsec;
}
static void disconnect_peer(struct rdp_head *d)
{
    if (d->peer >= 0) close(d->peer);
    d->peer = -1;
    d->registered = d->valid = false;
    d->capture = 0;
}
static bool packet(struct rdp_head *d, const uint8_t *b, uint64_t now)
{
    if (memcmp(b, "SDVR", 4) || get(b, 4, 2) != 1) return false;
    for (unsigned i = 150; i < 160; ++i) if (b[i]) return false;
    unsigned kind = (unsigned)get(b, 6, 2);
    if (kind == 0) {
        if (d->registered) return false;
        for (unsigned i = 0; i < 4; ++i) {
            bool present = false;
            for (unsigned j = 0; j < 16; ++j) present |= b[8 + i * 16 + j] != 0;
            if (!present) return false;
        }
        memcpy(d->ids, b + 8, 64);
        d->registered = true;
        d->valid = false;
        return true;
    }
    if (!d->registered || memcmp(d->ids, b + 8, 64)) return false;
    if (kind == 2) { d->valid = false; return true; }
    if (kind != 1) return false;
    uint64_t capture = get(b, 72, 8), uncertainty = get(b, 88, 8);
    if (get(b, 144, 4) != 1 || !capture || capture <= d->capture || uncertainty > 50000000ULL ||
        (capture > now && capture - now > uncertainty) || (now >= capture && now - capture > 100000000ULL)) {
        d->valid = false;
        return true;
    }
    double q[4], norm = 0;
    for (unsigned i = 0; i < 4; ++i) {
        uint64_t bits = get(b, 104 + 8 * i, 8);
        memcpy(&q[i], &bits, sizeof(bits));
        if (!isfinite(q[i])) return false;
        norm += q[i] * q[i];
    }
    if (fabs(norm - 1.0) > 0.001) return false;
    d->orientation = (struct xrt_quat){(float)q[0], (float)q[1], (float)q[2], (float)q[3]};
    d->capture = capture;
    d->origin = get(b, 96, 8);
    d->valid = true;
    return true;
}
static void drain(struct rdp_head *d)
{
    if (d->peer < 0) {
        int peer = accept4(d->listener, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (peer < 0) return;
        struct ucred cred;
        socklen_t size = sizeof(cred);
        if (getsockopt(peer, SOL_SOCKET, SO_PEERCRED, &cred, &size) != 0 || cred.uid != geteuid()) { close(peer); return; }
        d->peer = peer;
    }
    uint64_t now = boottime();
    for (unsigned i = 0; i < 16; ++i) {
        uint8_t bytes[161];
        ssize_t n = recv(d->peer, bytes, sizeof(bytes), MSG_DONTWAIT);
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
        if (n != 160 || !packet(d, bytes, now)) { disconnect_peer(d); break; }
    }
    if (now >= d->capture && now - d->capture > 100000000ULL) d->valid = false;
}
static xrt_result_t pose(struct xrt_device *xdev, enum xrt_input_name name, int64_t at, struct xrt_space_relation *out)
{
    (void)at; // No guessed angular velocity or receipt-time substitution.
    struct rdp_head *d = (struct rdp_head *)xdev;
    *out = (struct xrt_space_relation)XRT_SPACE_RELATION_ZERO;
    out->pose.orientation.w = 1;
    if (name != XRT_INPUT_GENERIC_HEAD_POSE) return XRT_ERROR_INPUT_UNSUPPORTED;
    os_mutex_lock(&d->mutex);
    drain(d);
    if (d->valid) {
        out->pose.orientation = d->orientation;
        out->relation_flags = XRT_SPACE_RELATION_ORIENTATION_VALID_BIT | XRT_SPACE_RELATION_ORIENTATION_TRACKED_BIT;
    }
    os_mutex_unlock(&d->mutex);
    return XRT_SUCCESS;
}
static xrt_result_t update(struct xrt_device *xdev) { (void)xdev; return XRT_SUCCESS; }
static void destroy(struct xrt_device *xdev)
{
    struct rdp_head *d = (struct rdp_head *)xdev;
    disconnect_peer(d);
    if (d->listener >= 0) { close(d->listener); unlink(d->path); }
    os_mutex_destroy(&d->mutex);
    u_device_free(xdev);
}
struct xrt_device *t_rdp_head_create(void)
{
    const char *runtime = getenv("XDG_RUNTIME_DIR");
    if (!runtime || !*runtime) return NULL;
    char directory[108];
    if (snprintf(directory, sizeof(directory), "%s/stereodroidvr", runtime) >= (int)sizeof(directory)) return NULL;
    if (mkdir(directory, 0700) != 0 && errno != EEXIST) return NULL;
    struct stat info;
    if (lstat(directory, &info) != 0 || !S_ISDIR(info.st_mode) || info.st_uid != geteuid() || (info.st_mode & 077) != 0) return NULL;
    struct rdp_head *d = U_DEVICE_ALLOCATE(struct rdp_head, U_DEVICE_ALLOC_TRACKING_NONE, 1, 0);
    if (!d) return NULL;
    os_mutex_init(&d->mutex);
    d->listener = d->peer = -1;
    d->base.name = XRT_DEVICE_GENERIC_HMD;
    d->base.device_type = XRT_DEVICE_TYPE_HMD;
    d->base.update_inputs = update;
    d->base.get_tracked_pose = pose;
    d->base.destroy = destroy;
    d->base.inputs[0].name = XRT_INPUT_GENERIC_HEAD_POSE;
    d->base.inputs[0].active = true;
    snprintf(d->base.str, sizeof(d->base.str), "StereoDroidVR paired orientation");
    snprintf(d->base.serial, sizeof(d->base.serial), "StereoDroidVR");
    if (snprintf(d->path, sizeof(d->path), "%s/pose", directory) >= (int)sizeof(d->path)) { destroy(&d->base); return NULL; }
    int fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) { destroy(&d->base); return NULL; }
    struct sockaddr_un address = {.sun_family = AF_UNIX};
    memcpy(address.sun_path, d->path, strlen(d->path) + 1);
    // Never unlink a live/stale listener owned by another runtime instance.
    if (bind(fd, (struct sockaddr *)&address, sizeof(address)) != 0) { close(fd); destroy(&d->base); return NULL; }
    d->listener = fd;
    if (chmod(d->path, 0600) != 0 || listen(fd, 1) != 0) { destroy(&d->base); return NULL; }
    U_LOG_W("StereoDroidVR: awaiting a paired KRdp source; no pose is valid yet");
    return &d->base;
}
#else
struct xrt_device *t_rdp_head_create(void) { return NULL; }
#endif
