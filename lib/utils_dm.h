/*
 * libdevmapper - device-mapper backend for cryptsetup
 *
 * Copyright (C) 2004, Jana Saout <jana@saout.de>
 * Copyright (C) 2004-2007, Clemens Fruhwirth <clemens@endorphin.org>
 * Copyright (C) 2009-2018, Red Hat, Inc. All rights reserved.
 * Copyright (C) 2009-2018, Milan Broz
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#ifndef _UTILS_DM_H
#define _UTILS_DM_H

/* device-mapper library helpers */
#include <inttypes.h>

struct crypt_device;
struct volume_key;
struct crypt_params_verity;
struct device;

/* Device mapper backend - kernel support flags */
#define DM_KEY_WIPE_SUPPORTED (1 << 0)	/* key wipe message */
#define DM_LMK_SUPPORTED      (1 << 1)	/* lmk mode */
#define DM_SECURE_SUPPORTED   (1 << 2)	/* wipe (secure) buffer flag */
#define DM_PLAIN64_SUPPORTED  (1 << 3)	/* plain64 IV */
#define DM_DISCARDS_SUPPORTED (1 << 4)	/* discards/TRIM option is supported */
#define DM_VERITY_SUPPORTED   (1 << 5)	/* dm-verity target supported */
#define DM_TCW_SUPPORTED      (1 << 6)	/* tcw (TCRYPT CBC with whitening) */
#define DM_SAME_CPU_CRYPT_SUPPORTED (1 << 7) /* same_cpu_crypt */
#define DM_SUBMIT_FROM_CRYPT_CPUS_SUPPORTED (1 << 8) /* submit_from_crypt_cpus */
#define DM_VERITY_ON_CORRUPTION_SUPPORTED (1 << 9) /* ignore/restart_on_corruption, ignore_zero_block */
#define DM_VERITY_FEC_SUPPORTED (1 << 10) /* Forward Error Correction (FEC) */
#define DM_KERNEL_KEYRING_SUPPORTED (1 << 11) /* dm-crypt allows loading kernel keyring keys */
#define DM_INTEGRITY_SUPPORTED (1 << 12) /* dm-integrity target supported */
#define DM_SECTOR_SIZE_SUPPORTED (1 << 13) /* support for sector size setting in dm-crypt/dm-integrity */
#define DM_CAPI_STRING_SUPPORTED (1 << 14) /* support for cryptoapi format cipher definition */
#define DM_DEFERRED_SUPPORTED (1 << 15) /* deferred removal of device */
#define DM_INTEGRITY_RECALC_SUPPORTED (1 << 16) /* dm-integrity automatic recalculation supported */

typedef enum { DM_CRYPT = 0, DM_VERITY, DM_INTEGRITY, DM_UNKNOWN } dm_target_type;

int dm_flags(struct crypt_device *cd, dm_target_type target, uint32_t *flags);

#define DM_ACTIVE_DEVICE	(1 << 0)
#define DM_ACTIVE_UUID		(1 << 1)
#define DM_ACTIVE_HOLDERS	(1 << 2)

#define DM_ACTIVE_CRYPT_CIPHER	(1 << 3)
#define DM_ACTIVE_CRYPT_KEYSIZE	(1 << 4)
#define DM_ACTIVE_CRYPT_KEY	(1 << 5)

#define DM_ACTIVE_VERITY_ROOT_HASH	(1 << 6)
#define DM_ACTIVE_VERITY_HASH_DEVICE	(1 << 7)
#define DM_ACTIVE_VERITY_PARAMS		(1 << 8)

#define DM_ACTIVE_INTEGRITY_PARAMS	(1 << 9)

struct crypt_dm_active_device {
	dm_target_type target;
	uint64_t size;		/* active device size */
	uint32_t flags;		/* activation flags */
	const char *uuid;
	struct device *data_device;
	unsigned holders:1;
	union {
	struct {
		const char *cipher;
		const char *integrity;

		/* Active key for device */
		struct volume_key *vk;

		/* struct crypt_active_device */
		uint64_t offset;	/* offset in sectors */
		uint64_t iv_offset;	/* IV initialisation sector */
		uint32_t tag_size;	/* additional on-disk tag size */
		uint32_t sector_size;	/* encryption sector size */
	} crypt;
	struct {
		struct device *hash_device;
		struct device *fec_device;

		const char *root_hash;
		uint32_t root_hash_size;

		uint64_t hash_offset;	/* hash offset in blocks (not header) */
		uint64_t hash_blocks;	/* size of hash device (in hash blocks) */
		uint64_t fec_offset;	/* FEC offset in blocks (not header) */
		uint64_t fec_blocks;	/* size of FEC device (in hash blocks) */
		struct crypt_params_verity *vp;
	} verity;
	struct {
		uint64_t journal_size;
		uint32_t journal_watermark;
		uint32_t journal_commit_time;
		uint32_t interleave_sectors;
		uint32_t tag_size;
		uint64_t offset;	/* offset in sectors */
		uint32_t sector_size;	/* integrity sector size */
		uint32_t buffer_sectors;

		const char *integrity;
		/* Active key for device */
		struct volume_key *vk;

		const char *journal_integrity;
		struct volume_key *journal_integrity_key;

		const char *journal_crypt;
		struct volume_key *journal_crypt_key;

		struct device *meta_device;
	} integrity;
	} u;
};

void dm_backend_init(struct crypt_device *cd);
void dm_backend_exit(struct crypt_device *cd);

int dm_remove_device(struct crypt_device *cd, const char *name, uint32_t flags);
int dm_status_device(struct crypt_device *cd, const char *name);
int dm_status_suspended(struct crypt_device *cd, const char *name);
int dm_status_verity_ok(struct crypt_device *cd, const char *name);
int dm_status_integrity_failures(struct crypt_device *cd, const char *name, uint64_t *count);
int dm_query_device(struct crypt_device *cd, const char *name,
		    uint32_t get_flags, struct crypt_dm_active_device *dmd);
int dm_create_device(struct crypt_device *cd, const char *name,
		     const char *type, struct crypt_dm_active_device *dmd);
int dm_reload_device(struct crypt_device *cd, const char *name,
		     struct crypt_dm_active_device *dmd, unsigned resume);
int dm_suspend_device(struct crypt_device *cd, const char *name);
int dm_suspend_and_wipe_key(struct crypt_device *cd, const char *name);
int dm_resume_device(struct crypt_device *cd, const char *name, uint32_t flags);
int dm_resume_and_reinstate_key(struct crypt_device *cd, const char *name,
				const struct volume_key *vk);
int dm_error_device(struct crypt_device *cd, const char *name);

const char *dm_get_dir(void);

int lookup_dm_dev_by_uuid(struct crypt_device *cd, const char *uuid, const char *type);

/* These are DM helpers used only by utils_devpath file */
int dm_is_dm_device(int major, int minor);
int dm_is_dm_kernel_name(const char *name);
char *dm_device_path(const char *prefix, int major, int minor);

#endif /* _UTILS_DM_H */
