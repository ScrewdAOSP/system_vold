/*
 * Copyright (C) 2016 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "KeyBuffer.h"
#include "MetadataCrypt.h"

#include <string>
#include <thread>
#include <vector>
#include <algorithm>

#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <linux/dm-ioctl.h>

#include <android-base/logging.h>
#include <android-base/unique_fd.h>
#include <cutils/properties.h>
#include <fs_mgr.h>

#include "EncryptInplace.h"
#include "KeyStorage.h"
#include "KeyUtil.h"
#include "secontext.h"
#include "Utils.h"
#include "VoldUtil.h"

extern struct fstab *fstab;
#define DM_CRYPT_BUF_SIZE 4096
#define TABLE_LOAD_RETRIES 10
#define DEFAULT_KEY_TARGET_TYPE "default-key"

using android::vold::KeyBuffer;

static const std::string kDmNameUserdata = "userdata";

static bool mount_via_fs_mgr(const char* mount_point, const char* blk_device) {
    // fs_mgr_do_mount runs fsck. Use setexeccon to run trusted
    // partitions in the fsck domain.
    if (setexeccon(secontextFsck())) {
        PLOG(ERROR) << "Failed to setexeccon";
        return false;
    }
    auto mount_rc = fs_mgr_do_mount(fstab, const_cast<char*>(mount_point),
                                    const_cast<char*>(blk_device), nullptr);
    if (setexeccon(nullptr)) {
        PLOG(ERROR) << "Failed to clear setexeccon";
        return false;
    }
    if (mount_rc != 0) {
        LOG(ERROR) << "fs_mgr_do_mount failed with rc " << mount_rc;
        return false;
    }
    LOG(DEBUG) << "Mounted " << mount_point;
    return true;
}

static bool read_key(bool create_if_absent, KeyBuffer* key) {
    auto data_rec = fs_mgr_get_crypt_entry(fstab);
    if (!data_rec) {
        LOG(ERROR) << "Failed to get data_rec";
        return false;
    }
    if (!data_rec->key_dir) {
        LOG(ERROR) << "Failed to get key_dir";
        return false;
    }
    LOG(DEBUG) << "key_dir: " << data_rec->key_dir;
    if (!android::vold::pathExists(data_rec->key_dir)) {
        if (mkdir(data_rec->key_dir, 0777) != 0) {
            PLOG(ERROR) << "Unable to create: " << data_rec->key_dir;
            return false;
        }
        LOG(DEBUG) << "Created: " << data_rec->key_dir;
    }
    std::string key_dir = data_rec->key_dir;
    auto dir = key_dir + "/key";
    auto temp = key_dir + "/tmp";
    if (!android::vold::retrieveKey(create_if_absent, dir, temp, key)) return false;
    return true;
}

static KeyBuffer default_key_params(const std::string& real_blkdev, const KeyBuffer& key) {
    KeyBuffer hex_key;
    if (android::vold::StrToHex(key, hex_key) != android::OK) {
        LOG(ERROR) << "Failed to turn key to hex";
        return KeyBuffer();
    }
    auto res = KeyBuffer() + "AES-256-XTS " + hex_key + " " + real_blkdev.c_str() + " 0";
    LOG(DEBUG) << "crypt_params: " << std::string(res.data(), res.size());
    return res;
}

static bool get_number_of_sectors(const std::string& real_blkdev, uint64_t *nr_sec) {
    android::base::unique_fd dev_fd(TEMP_FAILURE_RETRY(open(
        real_blkdev.c_str(), O_RDONLY | O_CLOEXEC, 0)));
    if (dev_fd == -1) {
        PLOG(ERROR) << "Unable to open " << real_blkdev << " to measure size";
        return false;
    }
    unsigned long res;
    // TODO: should use BLKGETSIZE64
    get_blkdev_size(dev_fd.get(), &res);
    if (res == 0) {
        PLOG(ERROR) << "Unable to measure size of " << real_blkdev;
        return false;
    }
    *nr_sec = res;
    return true;
}

static struct dm_ioctl* dm_ioctl_init(char *buffer, size_t buffer_size,
                                      const std::string& dm_name) {
    if (buffer_size < sizeof(dm_ioctl)) {
        LOG(ERROR) << "dm_ioctl buffer too small";
        return nullptr;
    }

    memset(buffer, 0, buffer_size);
    struct dm_ioctl* io = (struct dm_ioctl*) buffer;
    io->data_size = buffer_size;
    io->data_start = sizeof(struct dm_ioctl);
    io->version[0] = 4;
    io->version[1] = 0;
    io->version[2] = 0;
    io->flags = 0;
    dm_name.copy(io->name, sizeof(io->name));
    return io;
}

static bool create_crypto_blk_dev(const std::string& dm_name, uint64_t nr_sec,
                                  const std::string& target_type, const KeyBuffer& crypt_params,
                                  std::string* crypto_blkdev) {
    android::base::unique_fd dm_fd(TEMP_FAILURE_RETRY(open(
        "/dev/device-mapper", O_RDWR | O_CLOEXEC, 0)));
    if (dm_fd == -1) {
        PLOG(ERROR) << "Cannot open device-mapper";
        return false;
    }
    alignas(struct dm_ioctl) char buffer[DM_CRYPT_BUF_SIZE];
    auto io = dm_ioctl_init(buffer, sizeof(buffer), dm_name);
    if (!io || ioctl(dm_fd.get(), DM_DEV_CREATE, io) != 0) {
        PLOG(ERROR) << "Cannot create dm-crypt device " << dm_name;
        return false;
    }

    // Get the device status, in particular, the name of its device file
    io = dm_ioctl_init(buffer, sizeof(buffer), dm_name);
    if (ioctl(dm_fd.get(), DM_DEV_STATUS, io) != 0) {
        PLOG(ERROR) << "Cannot retrieve dm-crypt device status " << dm_name;
        return false;
    }
    *crypto_blkdev = std::string() + "/dev/block/dm-" + std::to_string(
        (io->dev & 0xff) | ((io->dev >> 12) & 0xfff00));

    io = dm_ioctl_init(buffer, sizeof(buffer), dm_name);
    size_t paramix = io->data_start + sizeof(struct dm_target_spec);
    size_t nullix = paramix + crypt_params.size();
    size_t endix = (nullix + 1 + 7) & 8; // Add room for \0 and align to 8 byte boundary

    if (endix > sizeof(buffer)) {
        LOG(ERROR) << "crypt_params too big for DM_CRYPT_BUF_SIZE";
        return false;
    }

    io->target_count = 1;
    auto tgt = (struct dm_target_spec *) (buffer + io->data_start);
    tgt->status = 0;
    tgt->sector_start = 0;
    tgt->length = nr_sec;
    target_type.copy(tgt->target_type, sizeof(tgt->target_type));
    memcpy(buffer + paramix, crypt_params.data(),
            std::min(crypt_params.size(), sizeof(buffer) - paramix));
    buffer[nullix] = '\0';
    tgt->next = endix;

    for (int i = 0; ; i++) {
        if (ioctl(dm_fd.get(), DM_TABLE_LOAD, io) == 0) {
            break;
        }
        if (i+1 >= TABLE_LOAD_RETRIES) {
            PLOG(ERROR) << "DM_TABLE_LOAD ioctl failed";
            return false;
        }
        PLOG(INFO) << "DM_TABLE_LOAD ioctl failed, retrying";
        usleep(500000);
    }

    // Resume this device to activate it
    io = dm_ioctl_init(buffer, sizeof(buffer), dm_name);
    if (ioctl(dm_fd.get(), DM_DEV_SUSPEND, io)) {
        PLOG(ERROR) << "Cannot resume dm-crypt device " << dm_name;
        return false;
    }
    return true;
}

#define DATA_PREP_TIMEOUT 1000
static bool prep_data_fs(void)
{
    // NOTE: post_fs_data results in init calling back around to vold, so all
    // callers to this method must be async

    /* Do the prep of the /data filesystem */
    property_set("vold.post_fs_data_done", "0");
    property_set("vold.decrypt", "trigger_post_fs_data");
    LOG(DEBUG) << "Waiting for post_fs_data_done";

    /* Wait a max of 50 seconds, hopefully it takes much less */
    for (int i = 0; ; i++) {
        char p[PROPERTY_VALUE_MAX];

        property_get("vold.post_fs_data_done", p, "0");
        if (*p == '1') {
            LOG(INFO) << "Successful data prep";
            return true;
        }
        if (i + 1 == DATA_PREP_TIMEOUT) {
            LOG(ERROR) << "post_fs_data timed out";
            return false;
        }
        usleep(50000);
    }
}

static void async_kick_off() {
    LOG(DEBUG) << "Asynchronously restarting framework";
    sleep(2); // TODO: this mirrors cryptfs, but can it be made shorter?
    property_set("vold.decrypt", "trigger_load_persist_props");
    if (!prep_data_fs()) return;
    /* startup service classes main and late_start */
    property_set("vold.decrypt", "trigger_restart_framework");
}

bool e4crypt_mount_metadata_encrypted() {
    LOG(DEBUG) << "e4crypt_mount_default_encrypted";
    KeyBuffer key;
    if (!read_key(false, &key)) return false;
    auto data_rec = fs_mgr_get_crypt_entry(fstab);
    if (!data_rec) {
        LOG(ERROR) << "Failed to get data_rec";
        return false;
    }
    uint64_t nr_sec;
    if (!get_number_of_sectors(data_rec->blk_device, &nr_sec)) return false;
    std::string crypto_blkdev;
    if (!create_crypto_blk_dev(kDmNameUserdata, nr_sec, DEFAULT_KEY_TARGET_TYPE,
        default_key_params(data_rec->blk_device, key), &crypto_blkdev)) return false;
    // FIXME handle the corrupt case

    LOG(DEBUG) << "Restarting filesystem for metadata encryption";
    mount_via_fs_mgr(data_rec->mount_point, crypto_blkdev.c_str());
    std::thread(&async_kick_off).detach();
    return true;
}

bool e4crypt_enable_crypto() {
    LOG(DEBUG) << "e4crypt_enable_crypto";
    char encrypted_state[PROPERTY_VALUE_MAX];
    property_get("ro.crypto.state", encrypted_state, "");
    if (strcmp(encrypted_state, "")) {
        LOG(DEBUG) << "e4crypt_enable_crypto got unexpected starting state: " << encrypted_state;
        return false;
    }

    KeyBuffer key_ref;
    if (!read_key(true, &key_ref)) return false;

    auto data_rec = fs_mgr_get_crypt_entry(fstab);
    if (!data_rec) {
        LOG(ERROR) << "Failed to get data_rec";
        return false;
    }
    uint64_t nr_sec;
    if (!get_number_of_sectors(data_rec->blk_device, &nr_sec)) return false;

    std::string crypto_blkdev;
    if (!create_crypto_blk_dev(kDmNameUserdata, nr_sec, DEFAULT_KEY_TARGET_TYPE,
        default_key_params(data_rec->blk_device, key_ref), &crypto_blkdev)) return false;

    LOG(INFO) << "Beginning inplace encryption, nr_sec: " << nr_sec;
    off64_t size_already_done = 0;
    auto rc = cryptfs_enable_inplace(const_cast<char *>(crypto_blkdev.c_str()),
                                     data_rec->blk_device, nr_sec, &size_already_done, nr_sec, 0);
    if (rc != 0) {
        LOG(ERROR) << "Inplace crypto failed with code: " << rc;
        return false;
    }
    if (static_cast<uint64_t>(size_already_done) != nr_sec) {
        LOG(ERROR) << "Inplace crypto only got up to sector: " << size_already_done;
        return false;
    }
    LOG(INFO) << "Inplace encryption complete";

    property_set("ro.crypto.state", "encrypted");
    property_set("ro.crypto.type", "file");

    mount_via_fs_mgr(data_rec->mount_point, crypto_blkdev.c_str());
    property_set("vold.decrypt", "trigger_reset_main");
    std::thread(&async_kick_off).detach();
    return true;
}
