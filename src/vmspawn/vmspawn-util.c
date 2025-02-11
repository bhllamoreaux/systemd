/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include <unistd.h>
#include <linux/vhost.h>
#include <sys/ioctl.h>

#include "architecture.h"
#include "conf-files.h"
#include "errno-util.h"
#include "fd-util.h"
#include "fileio.h"
#include "json.h"
#include "log.h"
#include "macro.h"
#include "memory-util.h"
#include "path-lookup.h"
#include "path-util.h"
#include "random-util.h"
#include "recurse-dir.h"
#include "siphash24.h"
#include "socket-util.h"
#include "sort-util.h"
#include "string-table.h"
#include "string-util.h"
#include "strv.h"
#include "vmspawn-util.h"

OvmfConfig* ovmf_config_free(OvmfConfig *config) {
        if (!config)
                return NULL;

        free(config->path);
        free(config->format);
        free(config->vars);
        free(config->vars_format);
        return mfree(config);
}

DEFINE_STRING_TABLE_LOOKUP(qemu_network_stack, QemuNetworkStack);

int qemu_check_kvm_support(void) {
        if (access("/dev/kvm", F_OK) >= 0)
                return true;
        if (errno == ENOENT) {
                log_debug_errno(errno, "/dev/kvm not found. Not using KVM acceleration.");
                return false;
        }
        if (ERRNO_IS_PRIVILEGE(errno)) {
                log_debug_errno(errno, "Permission denied to access /dev/kvm. Not using KVM acceleration.");
                return false;
        }

        return -errno;
}

int qemu_check_vsock_support(void) {
        _cleanup_close_ int fd = -EBADF;
        /* Just using access() will just check if the device node exists, but not whether a
         * device driver is behind it (this is a common case since systemd-tmpfiles creates
         * the device node on boot, typically).
         *
         * Hence we open() the path to see if there's actually something behind.
         *
         * If not this should return ENODEV.
         */

        fd = open("/dev/vhost-vsock", O_RDWR|O_CLOEXEC);
        if (fd >= 0)
                return true;
        if (ERRNO_IS_DEVICE_ABSENT(errno)) {
                log_debug_errno(errno, "/dev/vhost-vsock device doesn't exist. Not adding a vsock device to the virtual machine.");
                return false;
        }
        if (ERRNO_IS_PRIVILEGE(errno)) {
                log_debug_errno(errno, "Permission denied to access /dev/vhost-vsock. Not adding a vsock device to the virtual machine.");
                return false;
        }

        return -errno;
}

/* holds the data retrieved from the QEMU firmware interop JSON data */
typedef struct FirmwareData {
        char **features;
        char *firmware;
        char *firmware_format;
        char *vars;
        char *vars_format;
} FirmwareData;

static bool firmware_data_supports_sb(const FirmwareData *fwd) {
        assert(fwd);

        return strv_contains(fwd->features, "secure-boot");
}

static FirmwareData* firmware_data_free(FirmwareData *fwd) {
        if (!fwd)
                return NULL;

        strv_free(fwd->features);
        free(fwd->firmware);
        free(fwd->firmware_format);
        free(fwd->vars);
        free(fwd->vars_format);

        return mfree(fwd);
}
DEFINE_TRIVIAL_CLEANUP_FUNC(FirmwareData*, firmware_data_free);

static int firmware_executable(const char *name, JsonVariant *v, JsonDispatchFlags flags, void *userdata) {
        static const JsonDispatch table[] = {
                { "filename", JSON_VARIANT_STRING, json_dispatch_string, offsetof(FirmwareData, firmware),        JSON_MANDATORY },
                { "format",   JSON_VARIANT_STRING, json_dispatch_string, offsetof(FirmwareData, firmware_format), JSON_MANDATORY },
                {}
        };

        return json_dispatch(v, table, flags, userdata);
}

static int firmware_nvram_template(const char *name, JsonVariant *v, JsonDispatchFlags flags, void *userdata) {
        static const JsonDispatch table[] = {
                { "filename", JSON_VARIANT_STRING, json_dispatch_string, offsetof(FirmwareData, vars),        JSON_MANDATORY },
                { "format",   JSON_VARIANT_STRING, json_dispatch_string, offsetof(FirmwareData, vars_format), JSON_MANDATORY },
                {}
        };

        return json_dispatch(v, table, flags, userdata);
}

static int firmware_mapping(const char *name, JsonVariant *v, JsonDispatchFlags flags, void *userdata) {
        static const JsonDispatch table[] = {
                { "device",         JSON_VARIANT_STRING, NULL,                    0, JSON_MANDATORY },
                { "executable",     JSON_VARIANT_OBJECT, firmware_executable,     0, JSON_MANDATORY },
                { "nvram-template", JSON_VARIANT_OBJECT, firmware_nvram_template, 0, JSON_MANDATORY },
                {}
        };

        return json_dispatch(v, table, flags, userdata);
}

static int get_firmware_search_dirs(char ***ret) {
        int r;

        assert(ret);

        /* Search in:
         * - $XDG_CONFIG_HOME/qemu/firmware
         * - /etc/qemu/firmware
         * - /usr/share/qemu/firmware
         *
         * Prioritising entries in "more specific" directories */

        _cleanup_free_ char *user_firmware_dir = NULL;
        r = xdg_user_config_dir(&user_firmware_dir, "/qemu/firmware");
        if (r < 0)
                return r;

        _cleanup_strv_free_ char **l = NULL;
        l = strv_new(user_firmware_dir, "/etc/qemu/firmware", "/usr/share/qemu/firmware");
        if (!l)
                return log_oom_debug();

        *ret = TAKE_PTR(l);
        return 0;
}

int list_ovmf_config(char ***ret) {
        _cleanup_strv_free_ char **search_dirs = NULL;
        int r;

        assert(ret);

        r = get_firmware_search_dirs(&search_dirs);
        if (r < 0)
                return r;

        r = conf_files_list_strv(
                        ret,
                        ".json",
                        /* root= */ NULL,
                        CONF_FILES_FILTER_MASKED|CONF_FILES_REGULAR,
                        (const char *const*) search_dirs);
        if (r < 0)
                return log_debug_errno(r, "Failed to list firmware files: %m");

        return 0;
}

static int load_firmware_data(const char *path, FirmwareData **ret) {
        int r;

        assert(path);
        assert(ret);

        _cleanup_(json_variant_unrefp) JsonVariant *json = NULL;
        r = json_parse_file(
                        /* f= */ NULL,
                        path,
                        /* flags= */ 0,
                        &json,
                        /* ret_line= */ NULL,
                        /* ret_column= */ NULL);
        if (r < 0)
                return r;

        static const JsonDispatch table[] = {
                { "description",     JSON_VARIANT_STRING, NULL,               0,                                JSON_MANDATORY },
                { "interface-types", JSON_VARIANT_ARRAY,  NULL,               0,                                JSON_MANDATORY },
                { "mapping",         JSON_VARIANT_OBJECT, firmware_mapping,   0,                                JSON_MANDATORY },
                { "targets",         JSON_VARIANT_ARRAY,  NULL,               0,                                JSON_MANDATORY },
                { "features",        JSON_VARIANT_ARRAY,  json_dispatch_strv, offsetof(FirmwareData, features), JSON_MANDATORY },
                { "tags",            JSON_VARIANT_ARRAY,  NULL,               0,                                JSON_MANDATORY },
                {}
        };

        _cleanup_(firmware_data_freep) FirmwareData *fwd = NULL;
        fwd = new0(FirmwareData, 1);
        if (!fwd)
                return -ENOMEM;

        r = json_dispatch(json, table, JSON_ALLOW_EXTENSIONS, fwd);
        if (r < 0)
                return r;

        *ret = TAKE_PTR(fwd);
        return 0;
}

static int ovmf_config_make(FirmwareData *fwd, OvmfConfig **ret) {
        assert(fwd);
        assert(ret);

        _cleanup_free_ OvmfConfig *config = NULL;
        config = new(OvmfConfig, 1);
        if (!config)
                return -ENOMEM;

        *config = (OvmfConfig) {
                .path = TAKE_PTR(fwd->firmware),
                .format = TAKE_PTR(fwd->firmware_format),
                .vars = TAKE_PTR(fwd->vars),
                .vars_format = TAKE_PTR(fwd->vars_format),
                .supports_sb = firmware_data_supports_sb(fwd),
        };

        *ret = TAKE_PTR(config);
        return 0;
}

int load_ovmf_config(const char *path, OvmfConfig **ret) {
        _cleanup_(firmware_data_freep) FirmwareData *fwd = NULL;
        int r;

        assert(path);
        assert(ret);

        r = load_firmware_data(path, &fwd);
        if (r < 0)
                return r;

        return ovmf_config_make(fwd, ret);
}

int find_ovmf_config(int search_sb, OvmfConfig **ret) {
        _cleanup_(ovmf_config_freep) OvmfConfig *config = NULL;
        _cleanup_strv_free_ char **conf_files = NULL;
        int r;

        assert(ret);

        /* Search in:
         * - $XDG_CONFIG_HOME/qemu/firmware
         * - /etc/qemu/firmware
         * - /usr/share/qemu/firmware
         *
         * Prioritising entries in "more specific" directories
         */

        r = list_ovmf_config(&conf_files);
        if (r < 0)
                return r;

        STRV_FOREACH(file, conf_files) {
                _cleanup_(firmware_data_freep) FirmwareData *fwd = NULL;

                r = load_firmware_data(*file, &fwd);
                if (r < 0) {
                        log_debug_errno(r, "Failed to load JSON file '%s', skipping: %m", *file);
                        continue;
                }

                if (strv_contains(fwd->features, "enrolled-keys")) {
                        log_debug("Skipping %s, firmware has enrolled keys which has been known to cause issues.", *file);
                        continue;
                }

                /* exclude firmware which doesn't match our Secure Boot requirements */
                if (search_sb >= 0 && !!search_sb != firmware_data_supports_sb(fwd)) {
                        log_debug("Skipping %s, firmware doesn't fit required Secure Boot configuration.", *file);
                        continue;
                }

                r = ovmf_config_make(fwd, &config);
                if (r < 0)
                        return r;

                log_debug("Selected firmware definition %s.", *file);
                break;
        }

        if (!config)
                return -ENOENT;

        if (ret)
                *ret = TAKE_PTR(config);

        return 0;
}

int find_qemu_binary(char **ret_qemu_binary) {
        int r;

        /*
         * On success the path to the qemu binary will be stored in `req_qemu_binary`
         *
         * If the qemu binary cannot be found -ENOENT will be returned.
         * If the native architecture is not supported by qemu -EOPNOTSUPP will be returned;
         */

        static const char *architecture_to_qemu_table[_ARCHITECTURE_MAX] = {
                [ARCHITECTURE_ARM64]       = "aarch64",     /* differs from our name */
                [ARCHITECTURE_ARM]         = "arm",
                [ARCHITECTURE_ALPHA]       = "alpha",
                [ARCHITECTURE_X86_64]      = "x86_64",      /* differs from our name */
                [ARCHITECTURE_X86]         = "i386",        /* differs from our name */
                [ARCHITECTURE_LOONGARCH64] = "loongarch64",
                [ARCHITECTURE_MIPS64_LE]   = "mips",        /* differs from our name */
                [ARCHITECTURE_MIPS_LE]     = "mips",        /* differs from our name */
                [ARCHITECTURE_PARISC]      = "hppa",        /* differs from our name */
                [ARCHITECTURE_PPC64_LE]    = "ppc",         /* differs from our name */
                [ARCHITECTURE_PPC64]       = "ppc",         /* differs from our name */
                [ARCHITECTURE_PPC]         = "ppc",
                [ARCHITECTURE_RISCV32]     = "riscv32",
                [ARCHITECTURE_RISCV64]     = "riscv64",
                [ARCHITECTURE_S390X]       = "s390x",
        };

        FOREACH_STRING(s, "qemu", "qemu-kvm") {
                r = find_executable(s, ret_qemu_binary);
                if (r == 0)
                        return 0;

                if (r != -ENOENT)
                        return r;
        }

        const char *arch_qemu = architecture_to_qemu_table[native_architecture()];
        if (!arch_qemu)
                return log_debug_errno(SYNTHETIC_ERRNO(EOPNOTSUPP), "Architecture %s not supported by qemu", architecture_to_string(native_architecture()));

        _cleanup_free_ char *qemu_arch_specific = NULL;
        qemu_arch_specific = strjoin("qemu-system-", arch_qemu);
        if (!qemu_arch_specific)
                return -ENOMEM;

        return find_executable(qemu_arch_specific, ret_qemu_binary);
}

int vsock_fix_child_cid(int vhost_device_fd, unsigned *machine_cid, const char *machine) {
        /* this is an arbitrary value picked from /dev/urandom */
        static const uint8_t sip_key[HASH_KEY_SIZE] = {
                0x03, 0xad, 0xf0, 0xa4,
                0x59, 0x2c, 0x77, 0x11,
                0xda, 0x39, 0x0c, 0xba,
                0xf5, 0x4c, 0x80, 0x52
        };
        struct siphash machine_hash_state, state;
        int r;

        /* uint64_t is required here for the ioctl call, but valid CIDs are only 32 bits */
        uint64_t cid = *ASSERT_PTR(machine_cid);

        assert(machine);
        assert(vhost_device_fd >= 0);

        /* Fix the CID of the AF_VSOCK socket passed to qemu
         *
         * If the user has passed us a CID (machine_cid != VMADDR_CID_ANY), then attempt to bind to that CID
         * and error if we cannot.
         *
         * Otherwise hash the machine name to get a random CID and attempt to bind to that.
         * If it is occupied add more information into the hash and try again.
         * If after 64 attempts this hasn't worked fallback to truly random CIDs.
         * If after another 64 attempts this hasn't worked then give up and return EADDRNOTAVAIL.
         */

        if (cid != VMADDR_CID_ANY) {
                r = ioctl(vhost_device_fd, VHOST_VSOCK_SET_GUEST_CID, &cid);
                if (r < 0)
                        return log_debug_errno(errno, "Failed to set CID for child vsock with user provided CID %" PRIu64 ": %m", cid);
                return 0;
        }

        siphash24_init(&machine_hash_state, sip_key);
        siphash24_compress_string(machine, &machine_hash_state);
        for (unsigned i = 0; i < 64; i++) {
                state = machine_hash_state;
                siphash24_compress_safe(&i, sizeof i, &state);
                uint64_t hash = siphash24_finalize(&state);

                cid = 3 + (hash % (UINT_MAX - 4));
                r = ioctl(vhost_device_fd, VHOST_VSOCK_SET_GUEST_CID, &cid);
                if (r >= 0) {
                        *machine_cid = cid;
                        return 0;
                }
                if (errno != EADDRINUSE)
                        return -errno;
        }

        for (unsigned i = 0; i < 64; i++) {
                cid = 3 + random_u64_range(UINT_MAX - 4);
                r = ioctl(vhost_device_fd, VHOST_VSOCK_SET_GUEST_CID, &cid);
                if (r >= 0) {
                        *machine_cid = cid;
                        return 0;
                }

                if (errno != EADDRINUSE)
                        return -errno;
        }

        return log_debug_errno(SYNTHETIC_ERRNO(EADDRNOTAVAIL), "Failed to assign a CID to the guest vsock");
}
