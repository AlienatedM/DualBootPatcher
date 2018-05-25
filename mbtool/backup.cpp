/*
 * Copyright (C) 2015-2018  Andrew Gunnerson <andrewgunnerson@gmail.com>
 *
 * This file is part of DualBootPatcher
 *
 * DualBootPatcher is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * DualBootPatcher is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with DualBootPatcher.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "backup.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include <dirent.h>
#include <getopt.h>
#include <sys/mount.h>

#include <archive.h>
#include <archive_entry.h>

#include "mbcommon/integer.h"
#include "mbcommon/string.h"
#include "mblog/logging.h"
#include "mbutil/archive.h"
#include "mbutil/copy.h"
#include "mbutil/delete.h"
#include "mbutil/directory.h"
#include "mbutil/file.h"
#include "mbutil/mount.h"
#include "mbutil/path.h"
#include "mbutil/selinux.h"
#include "mbutil/string.h"
#include "mbutil/time.h"

#include "installer_util.h"
#include "image.h"
#include "multiboot.h"
#include "roms.h"
#include "wipe.h"

#define LOG_TAG "mbtool/backup"


namespace mb
{

enum class BackupTarget : uint8_t
{
    System  = 1 << 0,
    Cache   = 1 << 1,
    Data    = 1 << 2,
    Boot    = 1 << 3,
    Config  = 1 << 4,
    All     = (1 << 5) - 1,
};
MB_DECLARE_FLAGS(BackupTargets, BackupTarget)
MB_DECLARE_OPERATORS_FOR_FLAGS(BackupTargets)

constexpr char BACKUP_MNT_DIR[]            = "/mb_mnt";

constexpr char BACKUP_NAME_PREFIX_SYSTEM[] = "system";
constexpr char BACKUP_NAME_PREFIX_CACHE[]  = "cache";
constexpr char BACKUP_NAME_PREFIX_DATA[]   = "data";
constexpr char BACKUP_NAME_BOOT_IMAGE[]    = "boot.img";
constexpr char BACKUP_NAME_CONFIG[]        = "config.json";
constexpr char BACKUP_NAME_THUMBNAIL[]     = "thumbnail.webp";

// Max file size for FAT32
constexpr uint64_t DEFAULT_ARCHIVE_SPLIT_SIZE = UINT32_MAX - 1;

using ScopedDIR = std::unique_ptr<DIR, decltype(closedir) *>;

enum class Result
{
    Succeeded,
    Failed,
    FilesMissing,
    BootImageUnpatched,
};

static struct CompressionMap
{
    util::CompressionType type;
    const char *name;
    const char *extension;
} g_compression_map[] = {
    { util::CompressionType::None, "none",  ".tar" },
    { util::CompressionType::Lz4,  "lz4",   ".tar.lz4" },
    { util::CompressionType::Gzip, "gzip",  ".tar.gz" },
    { util::CompressionType::Xz,   "xz",    ".tar.xz" },
    { util::CompressionType::None, nullptr, nullptr }
};

static BackupTargets parse_targets_string(const std::string &targets)
{
    auto targets_list = split_sv(targets, ",");
    BackupTargets result(0);

    for (auto const &target : targets_list) {
        if (target == "all") {
            result |= BackupTarget::All;
        } else if (target == "system") {
            result |= BackupTarget::System;
        } else if (target == "cache") {
            result |= BackupTarget::Cache;
        } else if (target == "data") {
            result |= BackupTarget::Data;
        } else if (target == "boot") {
            result |= BackupTarget::Boot;
        } else if (target == "config") {
            result |= BackupTarget::Config;
        } else {
            return 0;
        }
    }

    return result;
}

static bool parse_compression_type(const char *type,
                                   util::CompressionType &compression)
{
    for (auto i = g_compression_map; i->name; ++i) {
        if (strcmp(type, i->name) == 0) {
            compression = i->type;
            return true;
        }
    }
    return false;
}

static std::string get_compressed_backup_name(const std::string &name,
                                              util::CompressionType compression)
{
    for (auto i = g_compression_map; i->name; ++i) {
        if (compression == i->type) {
            return name + i->extension;
        }
    }
    return {};
}

static std::string find_compressed_backup(const std::string &backup_dir,
                                          const std::string &name,
                                          util::CompressionType &compression,
                                          bool &is_split)
{
    std::string unsplit_path;
    std::string split_path;

    for (auto i = g_compression_map; i->name; ++i) {
        unsplit_path = backup_dir;
        unsplit_path += "/";
        unsplit_path += name;
        unsplit_path += i->extension;

        split_path = unsplit_path;
        split_path += ".0";

        if (access(unsplit_path.c_str(), R_OK) == 0) {
            compression = i->type;
            is_split = false;
            return name + i->extension;
        } else if (access(split_path.c_str(), R_OK) == 0) {
            compression = i->type;
            is_split = true;
            return name + i->extension;
        }
    }
    return {};
}

static bool backup_directory(const std::string &output_file,
                             const std::string &directory,
                             const std::vector<std::string> &exclusions,
                             util::CompressionType compression,
                             uint64_t split_archive_size)
{
    ScopedDIR dp(opendir(directory.c_str()), closedir);
    if (!dp) {
        LOGE("%s: Failed to open directory: %s",
             directory.c_str(), strerror(errno));
        return false;
    }

    std::vector<std::string> contents;
    dirent *ent;
    errno = 0;

    while ((ent = readdir(dp.get()))) {
        if (strcmp(ent->d_name, ".") == 0
                || strcmp(ent->d_name, "..") == 0
                || std::find(exclusions.begin(), exclusions.end(), ent->d_name)
                        != exclusions.end()) {
            continue;
        }
        contents.push_back(ent->d_name);
    }

    if (errno) {
        LOGE("%s: Failed to read directory contents: %s",
             directory.c_str(), strerror(errno));
        return false;
    }

    return util::libarchive_tar_create(output_file, directory, contents,
                                       compression, split_archive_size);
}

static bool restore_directory(const std::string &input_file,
                              const std::string &directory,
                              const std::vector<std::string> &exclusions,
                              util::CompressionType compression,
                              bool is_split)
{
    if (!wipe_directory(directory, exclusions)) {
        return false;
    }

    return util::libarchive_tar_extract(input_file, directory, {}, compression,
                                        is_split);
}

static bool backup_image(const std::string &output_file,
                         const std::string &image,
                         const std::vector<std::string> &exclusions,
                         util::CompressionType compression,
                         uint64_t split_archive_size)
{
    if (auto r = util::mkdir_recursive(BACKUP_MNT_DIR, 0755);
            !r && r.error() != std::errc::file_exists) {
        LOGE("%s: Failed to create directory: %s",
             BACKUP_MNT_DIR, strerror(errno));
        return false;
    }

    fsck_ext4_image(image);

    if (auto ret = util::mount(
            image, BACKUP_MNT_DIR, "ext4", MS_RDONLY, ""); !ret) {
        LOGE("Failed to mount %s at %s: %s", image.c_str(), BACKUP_MNT_DIR,
             ret.error().message().c_str());
        return false;
    }

    bool ret = backup_directory(output_file, BACKUP_MNT_DIR, exclusions,
                                compression, split_archive_size);

    if (auto umount_ret = util::umount(BACKUP_MNT_DIR); !umount_ret) {
        LOGE("Failed to unmount %s: %s", BACKUP_MNT_DIR,
             umount_ret.error().message().c_str());
        return false;
    }

    rmdir(BACKUP_MNT_DIR);

    return ret;
}

static bool restore_image(const std::string &input_file,
                          const std::string &image,
                          uint64_t size,
                          const std::vector<std::string> &exclusions,
                          util::CompressionType compression,
                          bool is_split)
{
    if (auto r = util::mkdir_parent(image, S_IRWXU); !r) {
        LOGE("%s: Failed to create parent directory: %s",
             image.c_str(), r.error().message().c_str());
        return false;
    }

    struct stat sb;
    if (stat(image.c_str(), &sb) < 0) {
        if (errno == ENOENT) {
            auto result = create_ext4_image(image, size);
            if (result != CreateImageResult::Succeeded) {
                return false;
            }
        } else {
            LOGE("%s: Failed to stat: %s", image.c_str(), strerror(errno));
            return false;
        }
    }

    if (auto r = util::mkdir_recursive(BACKUP_MNT_DIR, 0755);
            !r && r.error() != std::errc::file_exists) {
        LOGE("%s: Failed to create directory: %s",
             BACKUP_MNT_DIR, strerror(errno));
        return false;
    }

    fsck_ext4_image(image);

    if (auto ret = util::mount(image, BACKUP_MNT_DIR, "ext4", 0, ""); !ret) {
        LOGE("Failed to mount %s at %s: %s", image.c_str(), BACKUP_MNT_DIR,
             ret.error().message().c_str());
        return false;
    }

    bool ret = restore_directory(input_file, BACKUP_MNT_DIR, exclusions,
                                 compression, is_split);

    if (auto umount_ret = util::umount(BACKUP_MNT_DIR); !umount_ret) {
        LOGE("Failed to unmount %s: %s", BACKUP_MNT_DIR,
             umount_ret.error().message().c_str());
        return false;
    }

    rmdir(BACKUP_MNT_DIR);

    return ret;
}

/*!
 * \brief Backup boot image of a ROM
 *
 * \param rom ROM
 * \param backup_dir Backup directory
 *
 * \return Result::Succeeded if the boot image was successfully backed up
 *         Result::Failed if an error occured
 *         Result::FilesMissing if the boot image doesn't exist
 */
static Result backup_boot_image(const std::shared_ptr<Rom> &rom,
                                const std::string &backup_dir)
{
    std::string boot_image_path(rom->boot_image_path());
    std::string boot_image_backup(backup_dir);
    boot_image_backup += '/';
    boot_image_backup += BACKUP_NAME_BOOT_IMAGE;

    struct stat sb;
    if (stat(boot_image_path.c_str(), &sb) == 0) {
        LOGI("=== Backing up %s ===", boot_image_path.c_str());
        if (auto r = util::copy_file(
                boot_image_path, boot_image_backup, 0); !r) {
            LOGE("%s", r.error().message().c_str());
            return Result::Failed;
        }
    } else {
        LOGW("=== %s does not exist ===", boot_image_path.c_str());
        return Result::FilesMissing;
    }

    return Result::Succeeded;
}

/*!
 * \brief Restore boot image for a ROM
 *
 * \param rom ROM
 * \param backup_dir Backup directory
 *
 * \return Result::Succeeded if the boot image was successfully restored
 *         Result::Failed if an error occured
 *         Result::FilesMissing if the boot image backup doesn't exist
 */
static Result restore_boot_image(const std::shared_ptr<Rom> &rom,
                                 const std::string &backup_dir)
{
    std::string boot_image_path(rom->boot_image_path());
    std::string boot_image_backup(backup_dir);
    boot_image_backup += '/';
    boot_image_backup += BACKUP_NAME_BOOT_IMAGE;

    struct stat sb;
    if (stat(boot_image_backup.c_str(), &sb) < 0) {
        LOGW("=== %s does not exist ===", boot_image_backup.c_str());
        return Result::FilesMissing;
    }

    LOGI("=== Restoring to %s ===", boot_image_path.c_str());

    std::vector<std::function<RamdiskPatcherFn>> rps;
    rps.push_back(rp_write_rom_id(rom->id));

    if (!InstallerUtil::patch_boot_image(
            boot_image_backup, boot_image_path, rps)) {
        LOGE("Failed to patch boot image");
        return Result::Failed;
    }

    // We explicitly don't update the checksums here. The user needs to know the
    // risk of restoring a backup that can be modified by any app.

    return Result::Succeeded;
}

/*!
 * \brief Backup configuration file and thumbnail for a ROM
 *
 * \param rom ROM
 * \param backup_dir Backup directory
 *
 * \return Result::Succeeded if the configs were successfully backed up
 *         Result::Failed if an error occured
 *         Result::FilesMissing if the configs don't exist
 */
static Result backup_configs(const std::shared_ptr<Rom> &rom,
                             const std::string &backup_dir)
{
    std::string config_path(rom->config_path());
    std::string thumbnail_path(rom->thumbnail_path());

    std::string config_backup(backup_dir);
    config_backup += '/';
    config_backup += BACKUP_NAME_CONFIG;
    std::string thumbnail_backup(backup_dir);
    thumbnail_backup += '/';
    thumbnail_backup += BACKUP_NAME_THUMBNAIL;

    Result ret = Result::Succeeded;

    struct stat sb;
    if (stat(config_path.c_str(), &sb) == 0) {
        LOGI("=== Backing up %s ===", config_path.c_str());
        if (auto r = util::copy_file(config_path, config_backup, 0); !r) {
            LOGE("%s", r.error().message().c_str());
            return Result::Failed;
        }
    } else {
        LOGW("=== %s does not exist ===", config_path.c_str());
        ret = Result::FilesMissing;
    }
    if (stat(thumbnail_path.c_str(), &sb) == 0) {
        LOGI("=== Backing up %s ===", thumbnail_path.c_str());
        if (auto r = util::copy_file(thumbnail_path, thumbnail_backup, 0); !r) {
            LOGE("%s", r.error().message().c_str());
            return Result::Failed;
        }
    } else {
        LOGW("=== %s does not exist ===", thumbnail_path.c_str());
        ret = Result::FilesMissing;
    }

    return ret;
}

/*!
 * \brief Restore configuration file and thumbnail for a ROM
 *
 * \param rom ROM
 * \param backup_dir Backup directory
 *
 * \return Result::Succeeded if the configs were successfully restored
 *         Result::Failed if an error occured
 *         Result::FilesMissing if the backups of the configs don't exist
 */
static Result restore_configs(const std::shared_ptr<Rom> &rom,
                              const std::string &backup_dir)
{
    std::string config_path(rom->config_path());
    std::string thumbnail_path(rom->thumbnail_path());

    std::string config_backup(backup_dir);
    config_backup += '/';
    config_backup += BACKUP_NAME_CONFIG;
    std::string thumbnail_backup(backup_dir);
    thumbnail_backup += '/';
    thumbnail_backup += BACKUP_NAME_THUMBNAIL;

    Result ret = Result::Succeeded;

    struct stat sb;
    if (stat(config_backup.c_str(), &sb) == 0) {
        LOGI("=== Restoring to %s ===", config_path.c_str());
        if (auto r = util::copy_file(config_backup, config_path, 0); !r) {
            LOGE("%s", r.error().message().c_str());
            return Result::Failed;
        }
    } else {
        LOGW("=== %s does not exist ===", config_backup.c_str());
        ret = Result::FilesMissing;
    }
    if (stat(thumbnail_backup.c_str(), &sb) == 0) {
        LOGI("=== Restoring to %s ===", thumbnail_path.c_str());
        if (auto r = util::copy_file(thumbnail_backup, thumbnail_path, 0); !r) {
            LOGE("%s", r.error().message().c_str());
            return Result::Failed;
        }
    } else {
        LOGW("=== %s does not exist ===", thumbnail_backup.c_str());
        ret = Result::FilesMissing;
    }

    return ret;
}

/*!
 * \brief Backup a partition for a ROM
 *
 * \param path Path to mountpoint/directory or image
 * \param backup_dir Backup directory
 * \param archive_name Backup archive name
 * \param is_image Whether \a path is an ext4 image
 * \param exclusions List of top-level directories to exclude from the backup
 * \param compression Compression type
 * \param split_archive_size Max size for each split file
 *
 * \return Result::Succeeded if the directory/image was successfully backed up
 *         Result::Failed if an error occured
 *         Result::FilesMissing if \a path does not exist
 */
static Result backup_partition(const std::string &path,
                               const std::string &backup_dir,
                               const std::string &archive_name,
                               bool is_image,
                               const std::vector<std::string> &exclusions,
                               util::CompressionType compression,
                               uint64_t split_archive_size)
{
    std::string archive(backup_dir);
    archive += '/';
    archive += archive_name;

    bool ret = false;

    struct stat sb;
    if (stat(path.c_str(), &sb) == 0) {
        LOGI("=== Backing up %s ===", path.c_str());
        if (is_image) {
            ret = backup_image(archive, path, exclusions, compression,
                               split_archive_size);
        } else {
            ret = backup_directory(archive, path, exclusions, compression,
                                   split_archive_size);
        }
    } else {
        LOGW("=== %s does not exist ===", path.c_str());
        return Result::FilesMissing;
    }

    return ret ? Result::Succeeded : Result::Failed;
}

/*!
 * \brief Restore a partition for a ROM
 *
 * \param path Path to mountpoint/directory or image
 * \param backup_dir Backup directory
 * \param archive_name Backup archive name
 * \param is_image Whether \a path is an ext4 image
 * \param exclusions List of top-level directories to exclude from the wipe
 *                   process before restoring
 * \param compression Compression type
 * \param is_split Whether the archive is split into multiple chunks
 *
 * \return Result::Succeeded if the directory/image was successfully restored
 *         Result::Failed if an error occured
 *         Result::FilesMissing if \a archive_name does not exist in
 *         \a backup_dir
 */
static Result restore_partition(const std::string &path,
                                const std::string &backup_dir,
                                const std::string &archive_name,
                                bool is_image,
                                uint64_t image_size,
                                const std::vector<std::string> &exclusions,
                                util::CompressionType compression,
                                bool is_split)
{
    std::string archive(backup_dir);
    archive += '/';
    archive += archive_name;

    std::string split_archive(archive);
    split_archive += ".0";

    bool ret = false;

    struct stat sb;
    if (stat(is_split ? split_archive.c_str() : archive.c_str(), &sb) == 0) {
        LOGI("=== Restoring to %s ===", path.c_str());
        if (is_image) {
            ret = restore_image(archive, path, image_size, exclusions,
                                compression, is_split);
        } else {
            ret = restore_directory(archive, path, exclusions, compression,
                                    is_split);
        }
    } else {
        LOGW("=== %s does not exist ===", archive.c_str());
        return Result::FilesMissing;
    }

    return ret ? Result::Succeeded : Result::Failed;
}

static bool backup_rom(const std::shared_ptr<Rom> &rom,
                       const std::string &output_dir, BackupTargets targets,
                       util::CompressionType compression,
                       uint64_t split_archive_size)
{
    if (!targets) {
        LOGE("No backup targets specified");
        return false;
    }

    const std::string system_path(rom->full_system_path());
    const std::string cache_path(rom->full_cache_path());
    const std::string data_path(rom->full_data_path());
    const std::string boot_image_path(rom->boot_image_path());
    const std::string config_path(rom->config_path());
    const std::string thumbnail_path(rom->thumbnail_path());

    LOGI("Backing up:");
    LOGI("- ROM ID: %s", rom->id.c_str());
    LOGI("- Targets:");
    if (targets & BackupTarget::System) {
        LOGI("  - System: %s", system_path.c_str());
    }
    if (targets & BackupTarget::Cache) {
        LOGI("  - Cache: %s", cache_path.c_str());
    }
    if (targets & BackupTarget::Data) {
        LOGI("  - Data: %s", data_path.c_str());
    }
    if (targets & BackupTarget::Boot) {
        LOGI("  - Boot image: %s", boot_image_path.c_str());
    }
    if (targets & BackupTarget::Config) {
        LOGI("  - Configs: %s", config_path.c_str());
        LOGI("             %s", thumbnail_path.c_str());
    }
    LOGI("- Backup directory: %s", output_dir.c_str());

    std::string output_system = get_compressed_backup_name(
            BACKUP_NAME_PREFIX_SYSTEM, compression);
    std::string output_cache = get_compressed_backup_name(
            BACKUP_NAME_PREFIX_CACHE, compression);
    std::string output_data = get_compressed_backup_name(
            BACKUP_NAME_PREFIX_DATA, compression);

    // Backup boot image
    if (targets & BackupTarget::Boot
            && backup_boot_image(rom, output_dir) == Result::Failed) {
        return false;
    }

    // Backup configs
    if (targets & BackupTarget::Config
            && backup_configs(rom, output_dir) == Result::Failed) {
        return false;
    }

    // Backup system
    if (targets & BackupTarget::System) {
        Result ret = backup_partition(
                system_path, output_dir, output_system,
                rom->system_is_image, { "multiboot" }, compression,
                split_archive_size);
        if (ret == Result::Failed) {
            return false;
        }
    }

    // Backup cache
    if (targets & BackupTarget::Cache) {
        Result ret = backup_partition(
                cache_path, output_dir, output_cache,
                rom->cache_is_image, { "multiboot" }, compression,
                split_archive_size);
        if (ret == Result::Failed) {
            return false;
        }
    }

    // Backup data
    if (targets & BackupTarget::Data) {
        Result ret = backup_partition(
                data_path, output_dir, output_data,
                rom->data_is_image, { "media", "multiboot" }, compression,
                split_archive_size);
        if (ret == Result::Failed) {
            return false;
        }
    }

    return true;
}

static bool restore_rom(const std::shared_ptr<Rom> &rom,
                        const std::string &input_dir, BackupTargets targets)
{
    if (!targets) {
        LOGE("No restore targets specified");
        return false;
    }

    const std::string system_path(rom->full_system_path());
    const std::string cache_path(rom->full_cache_path());
    const std::string data_path(rom->full_data_path());
    const std::string boot_image_path(rom->boot_image_path());
    const std::string config_path(rom->config_path());
    const std::string thumbnail_path(rom->thumbnail_path());

    LOGI("Restoring:");
    LOGI("- ROM ID: %s", rom->id.c_str());
    LOGI("- Targets:");
    if (targets & BackupTarget::System) {
        LOGI("  - System: %s", system_path.c_str());
    }
    if (targets & BackupTarget::Cache) {
        LOGI("  - Cache: %s", cache_path.c_str());
    }
    if (targets & BackupTarget::Data) {
        LOGI("  - Data: %s", data_path.c_str());
    }
    if (targets & BackupTarget::Boot) {
        LOGI("  - Boot image: %s", boot_image_path.c_str());
    }
    if (targets & BackupTarget::Config) {
        LOGI("  - Configs: %s", config_path.c_str());
        LOGI("             %s", thumbnail_path.c_str());
    }
    LOGI("- Backup directory: %s", input_dir.c_str());

    std::string multiboot_dir(MULTIBOOT_DIR);
    multiboot_dir += '/';
    multiboot_dir += rom->id;
    if (auto r = util::mkdir_recursive(multiboot_dir, 0775); !r) {
        LOGE("%s: Failed to create directory: %s",
             multiboot_dir.c_str(), r.error().message().c_str());
        return false;
    }

    // Restore boot image
    if (targets & BackupTarget::Boot
            && restore_boot_image(rom, input_dir) == Result::Failed) {
        return false;
    }

    // Restore configs
    if (targets & BackupTarget::Config
            && restore_configs(rom, input_dir) == Result::Failed) {
        return false;
    }

    fix_multiboot_permissions();

    // Restore system
    if (targets & BackupTarget::System) {
        auto image_size = util::mount_get_total_size(
                Roms::get_system_partition());
        if (!image_size) {
            LOGE("Failed to get the size of the system partition");
            return false;
        }

        util::CompressionType compression;
        bool is_split;

        std::string path = find_compressed_backup(
                input_dir, BACKUP_NAME_PREFIX_SYSTEM, compression, is_split);
        if (path.empty()) {
            LOGE("Backup of /system not found");
            return false;
        }

        Result ret = restore_partition(
                system_path, input_dir, path, rom->system_is_image,
                image_size.value(), {}, compression, is_split);
        if (ret == Result::Failed) {
            return false;
        }
    }

    // Restore cache
    if (targets & BackupTarget::Cache) {
        util::CompressionType compression;
        bool is_split;

        std::string path = find_compressed_backup(
                input_dir, BACKUP_NAME_PREFIX_CACHE, compression, is_split);
        if (path.empty()) {
            LOGE("Backup of /cache not found");
            return false;
        }

        Result ret = restore_partition(
                cache_path, input_dir, path, rom->cache_is_image,
                DEFAULT_IMAGE_SIZE, {}, compression, is_split);
        if (ret == Result::Failed) {
            return false;
        }
    }

    // Restore data
    if (targets & BackupTarget::Data) {
        util::CompressionType compression;
        bool is_split;

        std::string path = find_compressed_backup(
                input_dir, BACKUP_NAME_PREFIX_DATA, compression, is_split);
        if (path.empty()) {
            LOGE("Backup of /data not found");
            return false;
        }

        Result ret = restore_partition(
                data_path, input_dir, path, rom->data_is_image,
                DEFAULT_IMAGE_SIZE, { "media" }, compression, is_split);
        if (ret == Result::Failed) {
            return false;
        }
    }

    return true;
}

static bool unshare_mount_namespace()
{
    if (unshare(CLONE_NEWNS) < 0) {
        fprintf(stderr, "unshare() failed: %s\n", strerror(errno));
        return false;
    }

    if (mount("", "/", "", MS_PRIVATE | MS_REC, "") < 0) {
        fprintf(stderr, "Failed to set private mount propagation: %s\n",
                strerror(errno));
        return false;
    }

    if (mount("", "/", "", MS_REMOUNT, "") < 0) {
        fprintf(stderr, "Failed to remount rootfs as writable: %s\n",
                strerror(errno));
        return false;
    }

    return true;
}

static bool ensure_partitions_mounted()
{
    std::string system_partition(Roms::get_system_partition());
    std::string cache_partition(Roms::get_cache_partition());
    std::string data_partition(Roms::get_data_partition());

    if (system_partition.empty() || !util::is_mounted(system_partition)) {
        fprintf(stderr, "System partition is not mounted\n");
        return false;
    }
    if (cache_partition.empty() || !util::is_mounted(cache_partition)) {
        fprintf(stderr, "Cache partition is not mounted\n");
        return false;
    }
    if (data_partition.empty() || !util::is_mounted(data_partition)) {
        fprintf(stderr, "Data partition is not mounted\n");
        return false;
    }

    return true;
}

static bool remount_partitions_writable()
{
    std::string system_partition(Roms::get_system_partition());
    std::string cache_partition(Roms::get_cache_partition());
    std::string data_partition(Roms::get_data_partition());

    return mount("", system_partition.c_str(), "", MS_REMOUNT, "") == 0
            && mount("", cache_partition.c_str(), "", MS_REMOUNT, "") == 0
            && mount("", data_partition.c_str(), "", MS_REMOUNT, "") == 0;
}

static bool is_valid_backup_name(const std::string &name)
{
    // No empty strings, hidden paths, '..', or directory separators
    return !name.empty()                            // Must be non-empty
            && name.find('/') == std::string::npos  // and contain no slashes
            && name != "."                          // and not current directory
            && name != "..";                        // and not parent directory
}

static void warn_selinux_context()
{
    // We do not need to patch the SELinux policy or switch to mb_exec because
    // the daemon will guarantee that we run in that context. We'll just warn if
    // this happens to not be the case (eg. debugging via command line).

    auto context = util::selinux_get_process_attr(
            0, util::SELinuxAttr::Current);
    if (context && context.value() != MB_EXEC_CONTEXT) {
        fprintf(stderr, "WARNING: Not running under %s context\n",
                MB_EXEC_CONTEXT);
    }
}

static void backup_usage(FILE *stream)
{
    fprintf(stream,
            "Usage: backup -r <romid> -t <targets> [-n <name>] [OPTION...]\n\n"
            "Options:\n"
            "  -r, --romid <ROM ID>"
            "                   ROM ID to backup\n"
            "  -t, --targets <targets>\n"
            "                   Comma-separated list of targets to backup\n"
            "                   (Default: 'all')\n"
            "  -n, --name <name>\n"
            "                   Name of backup\n"
            "                   (Default: YYYY.MM.DD-HH.MM.SS)\n"
            "  -c, --compression <compression type>\n"
            "                   Compression type (none, lz4, gzip, xz)\n"
            "                   (Default: lz4)\n"
            "  -s, --split-size <size>\n"
            "                   Split archive maximum size in bytes (0 to disable)\n"
            "                   (Default: %" PRIu64 " bytes)\n"
            "  -d, --backupdir <directory>\n"
            "                   Directory to store backups\n"
            "                   (Default: " MULTIBOOT_BACKUP_DIR ")\n"
            "  -f, --force      Allow overwriting old backup with the same name\n"
            "  -h, --help       Display this help message\n"
            "\n"
            "Valid backup targets: 'all' or some combination of the following:\n"
            "  system,cache,data,boot,config\n"
            "\n"
            "NOTE: This tool is still in development and the arguments above\n"
            "have not yet been finalized.\n",
            DEFAULT_ARCHIVE_SPLIT_SIZE);
}

static void restore_usage(FILE *stream)
{
    fprintf(stream,
            "Usage: restore -r <romid> -t <targets> -n <name> [OPTION...]\n\n"
            "Options:\n"
            "  -r, --romid <ROM ID>\n"
            "                   ROM ID to restore to\n"
            "  -t, --targets <targets>\n"
            "                   Comma-separated list of targets to restore\n"
            "                   (Default: 'all')\n"
            "  -n, --name <name>\n"
            "                   Name of backup to restore\n"
            "  -d, --backupdir <directory>\n"
            "                   Directory containing backups\n"
            "                   (Default: " MULTIBOOT_BACKUP_DIR ")\n"
            "  -h, --help       Display this help message\n"
            "\n"
            "Valid backup targets: 'all' or some combination of the following:\n"
            "  system,cache,data,boot,config\n"
            "\n"
            "NOTE: This tool is still in development and the arguments above\n"
            "have not yet been finalized.\n");
}

int backup_main(int argc, char *argv[])
{
    int opt;

    static const char *short_options = "r:t:n:c:d:s:fh";
    static struct option long_options[] = {
        {"romid",       required_argument, 0, 'r'},
        {"targets",     required_argument, 0, 't'},
        {"name",        required_argument, 0, 'n'},
        {"compression", required_argument, 0, 'c'},
        {"backupdir",   required_argument, 0, 'd'},
        {"split-size",  required_argument, 0, 's'},
        {"force",       no_argument,       0, 'f'},
        {"help",        no_argument,       0, 'h'},
        {0, 0, 0, 0}
    };

    int long_index = 0;

    std::string romid;
    std::string targets_str("all");
    std::string name;
    std::string backupdir(MULTIBOOT_BACKUP_DIR);
    util::CompressionType compression = util::CompressionType::Lz4;
    uint64_t split_archive_size = DEFAULT_ARCHIVE_SPLIT_SIZE;
    bool force = false;

    if (auto n = util::format_time("%Y.%m.%d-%H.%M.%S",
                                   std::chrono::system_clock::now())) {
        n.value().swap(name);
    } else {
        fprintf(stderr, "Failed to format current time\n");
        return EXIT_FAILURE;
    }

    while ((opt = getopt_long(argc, argv, short_options,
            long_options, &long_index)) != -1) {
        switch (opt) {
        case 'r':
            romid = optarg;
            break;
        case 't':
            targets_str = optarg;
            break;
        case 'n':
            name = optarg;
            break;
        case 'c':
            if (!parse_compression_type(optarg, compression)) {
                fprintf(stderr, "Invalid compression type: %s\n", optarg);
                return EXIT_FAILURE;
            }
            break;
        case 'd':
            backupdir = optarg;
            break;
        case 's':
            if (!str_to_num(optarg, 10, split_archive_size)) {
                fprintf(stderr, "Invalid split size: %s\n", optarg);
                return EXIT_FAILURE;
            }
            break;
        case 'f':
            force = true;
            break;
        case 'h':
            backup_usage(stdout);
            return EXIT_SUCCESS;
        default:
            backup_usage(stderr);
            return EXIT_FAILURE;
        }
    }

    // There should be no other arguments
    if (argc - optind != 0) {
        backup_usage(stderr);
        return EXIT_FAILURE;
    }

    if (romid.empty()) {
        fprintf(stderr, "No ROM ID specified\n");
        return EXIT_FAILURE;
    }

    BackupTargets targets = parse_targets_string(targets_str);
    if (!targets) {
        fprintf(stderr, "Invalid targets: %s\n", targets_str.c_str());
        return EXIT_FAILURE;
    }

    if (!is_valid_backup_name(name)) {
        fprintf(stderr, "Invalid backup name: %s\n", name.c_str());
        return EXIT_FAILURE;
    }

    warn_selinux_context();

    if (!unshare_mount_namespace()) {
        return EXIT_FAILURE;
    }

    if (!ensure_partitions_mounted()) {
        return EXIT_FAILURE;
    }

    Roms roms;
    roms.add_installed();

    auto rom = roms.find_by_id(romid);
    if (!rom) {
        fprintf(stderr, "ROM '%s' is not installed\n", romid.c_str());
        return EXIT_FAILURE;
    }

    std::string output_dir(backupdir);
    output_dir += "/";
    output_dir += name;

    struct stat sb;
    if (!force && stat(output_dir.c_str(), &sb) == 0) {
        fprintf(stderr, "Backup '%s' already exists. Choose another name or "
                "pass -f/--force to use this name anyway.\n", name.c_str());
        return EXIT_FAILURE;
    }

    if (auto r = util::mkdir_recursive(output_dir, 0755); !r) {
        fprintf(stderr, "%s: Failed to create directory: %s\n",
                output_dir.c_str(), r.error().message().c_str());
        return EXIT_FAILURE;
    }

    bool ret = backup_rom(rom, output_dir, targets, compression,
                          split_archive_size);
    if (ret) {
        LOGI("=== Finished ===");
        return EXIT_SUCCESS;
    } else {
        LOGI("=== Failed ===");
        return EXIT_FAILURE;
    }
}

int restore_main(int argc, char *argv[])
{
    int opt;

    static const char *short_options = "r:t:n:d:h";
    static struct option long_options[] = {
        {"romid",     required_argument, 0, 'r'},
        {"targets",   required_argument, 0, 't'},
        {"name",      required_argument, 0, 'n'},
        {"backupdir", required_argument, 0, 'd'},
        {"help",      no_argument,       0, 'h'},
        {0, 0, 0, 0}
    };

    int long_index = 0;

    std::string romid;
    std::string targets_str("all");
    std::string name;
    std::string backupdir(MULTIBOOT_BACKUP_DIR);

    while ((opt = getopt_long(argc, argv, short_options,
            long_options, &long_index)) != -1) {
        switch (opt) {
        case 'r':
            romid = optarg;
            break;
        case 't':
            targets_str = optarg;
            break;
        case 'n':
            name = optarg;
            break;
        case 'd':
            backupdir = optarg;
            break;
        case 'h':
            restore_usage(stdout);
            return EXIT_SUCCESS;
        default:
            restore_usage(stderr);
            return EXIT_FAILURE;
        }
    }

    // There should be no other arguments
    if (argc - optind != 0) {
        restore_usage(stderr);
        return EXIT_FAILURE;
    }

    if (romid.empty()) {
        fprintf(stderr, "No ROM ID specified\n");
        return EXIT_FAILURE;
    }

    if (name.empty()) {
        fprintf(stderr, "No backup name specified\n");
        return EXIT_FAILURE;
    }

    BackupTargets targets = parse_targets_string(targets_str);
    if (!targets) {
        fprintf(stderr, "Invalid targets: %s\n", targets_str.c_str());
        return EXIT_FAILURE;
    }

    if (!is_valid_backup_name(name)) {
        fprintf(stderr, "Invalid backup name: %s\n", name.c_str());
        return EXIT_FAILURE;
    }

    warn_selinux_context();

    if (!unshare_mount_namespace()) {
        return EXIT_FAILURE;
    }

    if (!ensure_partitions_mounted()) {
        return EXIT_FAILURE;
    }

    if (!remount_partitions_writable()) {
        fprintf(stderr, "Failed to remount partitions as writable: %s\n",
                strerror(errno));
        return EXIT_FAILURE;
    }

    auto rom = Roms::create_rom(romid);
    if (!rom) {
        fprintf(stderr, "Invalid ROM ID: '%s'\n", romid.c_str());
        return EXIT_FAILURE;
    }

    std::string input_dir(backupdir);
    input_dir += "/";
    input_dir += name;

    struct stat sb;
    if (stat(input_dir.c_str(), &sb) < 0) {
        fprintf(stderr, "Backup '%s' does not exist\n", name.c_str());
        return EXIT_FAILURE;
    }

    bool ret = restore_rom(rom, input_dir, targets);
    if (ret) {
        LOGI("=== Finished ===");
        return EXIT_SUCCESS;
    } else {
        LOGI("=== Failed ===");
        return EXIT_FAILURE;
    }
}

}
