/*
 * OPAL utilities
 *
 * Copyright (C) 2022-2023 Luca Boccassi <bluca@debian.org>
 *               2023 Ondrej Kozina <okozina@redhat.com>
 *
 * This file is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This file is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this file; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <assert.h>
#include <sys/ioctl.h>
#include <sys/types.h>

#include "internal.h"
#include "libcryptsetup.h"
#include "luks2/hw_opal/hw_opal.h"

#if HAVE_HW_OPAL

#include <linux/sed-opal.h>

/* Error codes are defined in the specification:
 * TCG_Storage_Architecture_Core_Spec_v2.01_r1.00
 * Section 5.1.5: Method Status Codes
 * Names and values from table 166 */
typedef enum OpalStatus {
	OPAL_STATUS_SUCCESS,
	OPAL_STATUS_NOT_AUTHORIZED,
	OPAL_STATUS_OBSOLETE0, /* Undefined but possible return values are called 'obsolete' */
	OPAL_STATUS_SP_BUSY,
	OPAL_STATUS_SP_FAILED,
	OPAL_STATUS_SP_DISABLED,
	OPAL_STATUS_SP_FROZEN,
	OPAL_STATUS_NO_SESSIONS_AVAILABLE,
	OPAL_STATUS_UNIQUENESS_CONFLICT,
	OPAL_STATUS_INSUFFICIENT_SPACE,
	OPAL_STATUS_INSUFFICIENT_ROWS,
	OPAL_STATUS_INVALID_PARAMETER,
	OPAL_STATUS_OBSOLETE1,
	OPAL_STATUS_OBSOLETE2,
	OPAL_STATUS_TPER_MALFUNCTION,
	OPAL_STATUS_TRANSACTION_FAILURE,
	OPAL_STATUS_RESPONSE_OVERFLOW,
	OPAL_STATUS_AUTHORITY_LOCKED_OUT,
	OPAL_STATUS_FAIL = 0x3F, /* As defined by specification */
	_OPAL_STATUS_MAX,
	_OPAL_STATUS_INVALID = -EINVAL,
} OpalStatus;

static const char* const opal_status_table[_OPAL_STATUS_MAX] = {
	[OPAL_STATUS_SUCCESS]               = "success",
	[OPAL_STATUS_NOT_AUTHORIZED]        = "not authorized",
	[OPAL_STATUS_OBSOLETE0]             = "obsolete",
	[OPAL_STATUS_SP_BUSY]               = "SP busy",
	[OPAL_STATUS_SP_FAILED]             = "SP failed",
	[OPAL_STATUS_SP_DISABLED]           = "SP disabled",
	[OPAL_STATUS_SP_FROZEN]             = "SP frozen",
	[OPAL_STATUS_NO_SESSIONS_AVAILABLE] = "no sessions available",
	[OPAL_STATUS_UNIQUENESS_CONFLICT]   = "uniqueness conflict",
	[OPAL_STATUS_INSUFFICIENT_SPACE]    = "insufficient space",
	[OPAL_STATUS_INSUFFICIENT_ROWS]     = "insufficient rows",
	[OPAL_STATUS_INVALID_PARAMETER]     = "invalid parameter",
	[OPAL_STATUS_OBSOLETE1]             = "obsolete",
	[OPAL_STATUS_OBSOLETE2]             = "obsolete",
	[OPAL_STATUS_TPER_MALFUNCTION]      = "TPer malfunction",
	[OPAL_STATUS_TRANSACTION_FAILURE]   = "transaction failure",
	[OPAL_STATUS_RESPONSE_OVERFLOW]     = "response overflow",
	[OPAL_STATUS_AUTHORITY_LOCKED_OUT]  = "authority locked out",
	[OPAL_STATUS_FAIL]                  = "unknown failure",
};

static const char *opal_status_to_string(int t)
{
	if (t < 0)
		return strerror(-t);

	if (t >= _OPAL_STATUS_MAX)
		return "unknown error";

	return opal_status_table[t];
}

static int opal_geometry_fd(int fd,
			    bool *ret_align,
			    uint32_t *ret_block_size,
			    uint64_t *ret_alignment_granularity_blocks,
			    uint64_t *ret_lowest_lba_blocks)
{
	int r;
	struct opal_geometry geo;

	assert(fd >= 0);

	r = ioctl(fd, IOC_OPAL_GET_GEOMETRY, &geo);
	if (r != OPAL_STATUS_SUCCESS)
		return r;

	if (ret_align)
		*ret_align = (geo.align == 1);
	if (ret_block_size)
		*ret_block_size = geo.logical_block_size;
	if (ret_alignment_granularity_blocks)
		*ret_alignment_granularity_blocks = geo.alignment_granularity;
	if (ret_lowest_lba_blocks)
		*ret_lowest_lba_blocks = geo.lowest_aligned_lba;

	return r;
}

static int opal_range_check_attributes_fd(struct crypt_device *cd,
	int fd,
	uint32_t segment_number,
	const struct volume_key *vk,
	const uint64_t *check_offset_sectors,
	const uint64_t *check_length_sectors,
	bool *check_read_locked,
	bool *check_write_locked)
{
	int r;
	struct opal_lr_status *lrs;
	uint32_t opal_block_bytes;
	uint64_t offset, length;
	bool read_locked, write_locked;

	assert(fd >= 0);
	assert(cd);
	assert(vk);

	r = opal_geometry_fd(fd, NULL, &opal_block_bytes, NULL, NULL);
	if (r != OPAL_STATUS_SUCCESS)
		return -EINVAL;

	lrs = crypt_safe_alloc(sizeof(*lrs));
	if (!lrs) {
		r = -ENOMEM;
		goto out;
	}

	*lrs = (struct opal_lr_status) {
		.session = {
			.who = segment_number + 1,
			.opal_key = {
				.key_len = vk->keylength,
				.lr = segment_number
			}
		}
	};
	memcpy(lrs->session.opal_key.key, vk->key, vk->keylength);

	r = ioctl(fd, IOC_OPAL_GET_LR_STATUS, lrs);
	if (r != OPAL_STATUS_SUCCESS) {
		log_dbg(cd, "Failed to get locking range status on device '%s'.",
			crypt_get_device_name(cd));
		r = -EINVAL;
		goto out;
	}

	r = 0;

	offset = lrs->range_start * opal_block_bytes / SECTOR_SIZE;
	if (check_offset_sectors && (offset != *check_offset_sectors)) {
		log_err(cd, _("OPAL range %d offset %" PRIu64 " does not match expected values %" PRIu64 "."),
			segment_number, offset, *check_offset_sectors);
		r = -EINVAL;
	}

	length = lrs->range_length * opal_block_bytes / SECTOR_SIZE;
	if (check_length_sectors && (length != *check_length_sectors)) {
		log_err(cd, _("OPAL range %d length %" PRIu64" does not match device length %" PRIu64 "."),
			segment_number, length, *check_length_sectors);
		r = -EINVAL;
	}

	if (!lrs->RLE || !lrs->WLE) {
		log_err(cd, _("OPAL range %d locking is disabled."), segment_number);
		r = -EINVAL;
	}

	read_locked = (lrs->l_state == OPAL_LK);
	write_locked = !!(lrs->l_state & (OPAL_RO | OPAL_LK));

	if (check_read_locked && (read_locked != *check_read_locked)) {
		log_dbg(cd, "OPAL range %d read lock is %slocked.",
			segment_number, *check_read_locked ? "" : "not ");
		log_err(cd, _("Unexpected OPAL range %d lock state."), segment_number);
		r = -EINVAL;
	}

	if (check_write_locked && (write_locked != *check_write_locked)) {
		log_dbg(cd, "OPAL range %d write lock is %slocked.",
			segment_number, *check_write_locked ? "" : "not ");
		log_err(cd, _("Unexpected OPAL range %d lock state."), segment_number);
		r = -EINVAL;
	}
out:
	crypt_safe_free(lrs);

	return r;
}

int opal_setup_ranges(struct crypt_device *cd,
		      struct device *dev,
		      const struct volume_key *vk,
		      uint64_t range_start,
		      uint64_t range_length,
		      uint32_t segment_number,
		      const void *admin_key,
		      size_t admin_key_len)
{
	struct opal_lr_act *activate = NULL;
	struct opal_session_info *user_session = NULL;
	struct opal_lock_unlock *user_add_to_lr = NULL, *lock = NULL;
	struct opal_new_pw *new_pw = NULL;
	struct opal_user_lr_setup *setup = NULL;
	int r, fd;

	assert(cd);
	assert(dev);
	assert(vk);
	assert(admin_key);
	assert(vk->keylength <= OPAL_KEY_MAX);

	if (admin_key_len > OPAL_KEY_MAX)
		return -EINVAL;

	fd = device_open(cd, dev, O_RDWR);
	if (fd < 0)
		return -EIO;

	r = opal_enabled(cd, dev);
	if (r < 0)
		return r;

	/* If OPAL has never been enabled, we need to take ownership and do basic setup first */
	if (r == 0) {
		activate = crypt_safe_alloc(sizeof(struct opal_lr_act));
		if (!activate) {
			r = -ENOMEM;
			goto out;
		}
		*activate = (struct opal_lr_act) {
			.key = {
				.key_len = admin_key_len,
			},
			.num_lrs = 8,
			/* A max of 9 segments are supported, enable them all as there's no reason not to
			 * (0 is whole-volume)
			 */
			.lr = { 1, 2, 3, 4, 5, 6, 7, 8 },
		};
		memcpy(activate->key.key, admin_key, admin_key_len);

		r = ioctl(fd, IOC_OPAL_TAKE_OWNERSHIP, &activate->key);
		if (r < 0) {
			r = -ENOTSUP;
			log_dbg(cd, "OPAL not supported on this kernel version, refusing.");
			goto out;
		}
		if (r == OPAL_STATUS_NOT_AUTHORIZED) /* We'll try again with a different key. */ {
			r = -EPERM;
			log_dbg(cd, "Failed to take ownership of OPAL device '%s': permission denied",
				crypt_get_device_name(cd));
			goto out;
		}
		if (r != OPAL_STATUS_SUCCESS) {
			log_dbg(cd, "Failed to take ownership of OPAL device '%s': %s",
				crypt_get_device_name(cd), opal_status_to_string(r));
			r = -EINVAL;
			goto out;
		}

		r = ioctl(fd, IOC_OPAL_ACTIVATE_LSP, activate);
		if (r != OPAL_STATUS_SUCCESS) {
			log_dbg(cd, "Failed to activate OPAL device '%s': %s",
				crypt_get_device_name(cd), opal_status_to_string(r));
			r = -EINVAL;
			goto out;
		}
	} else {
		/* If it is already enabled, wipe the locking range first */
		user_session = crypt_safe_alloc(sizeof(struct opal_session_info));
		if (!user_session) {
			r = -ENOMEM;
			goto out;
		}
		*user_session = (struct opal_session_info) {
			.who = OPAL_ADMIN1,
			.opal_key = {
				.lr = segment_number,
				.key_len = admin_key_len,
			},
		};
		memcpy(user_session->opal_key.key, admin_key, admin_key_len);

		r = ioctl(fd, IOC_OPAL_ERASE_LR, user_session);
		if (r != OPAL_STATUS_SUCCESS) {
			log_dbg(cd, "Failed to reset (erase) OPAL locking range %u on device '%s': %s",
				segment_number, crypt_get_device_name(cd), opal_status_to_string(r));
			r = ioctl(fd, IOC_OPAL_SECURE_ERASE_LR, user_session);
			if (r != OPAL_STATUS_SUCCESS) {
				log_dbg(cd, "Failed to reset (secure erase) OPAL locking range %u on device '%s': %s",
					segment_number, crypt_get_device_name(cd), opal_status_to_string(r));
				r = -EINVAL;
				goto out;
			}
		}
	}

	user_session = crypt_safe_alloc(sizeof(struct opal_session_info));
	if (!user_session) {
		r = -ENOMEM;
		goto out;
	}
	*user_session = (struct opal_session_info) {
		.who = segment_number + 1,
		.opal_key = {
			.key_len = admin_key_len,
		},
	};
	memcpy(user_session->opal_key.key, admin_key, admin_key_len);

	r = ioctl(fd, IOC_OPAL_ACTIVATE_USR, user_session);
	if (r != OPAL_STATUS_SUCCESS) {
		log_dbg(cd, "Failed to activate OPAL user on device '%s': %s",
			crypt_get_device_name(cd), opal_status_to_string(r));
		r = -EINVAL;
		goto out;
	}

	user_add_to_lr = crypt_safe_alloc(sizeof(struct opal_lock_unlock));
	if (!user_add_to_lr) {
		r = -ENOMEM;
		goto out;
	}
	*user_add_to_lr = (struct opal_lock_unlock) {
		.session = {
			.who = segment_number + 1,
			.opal_key = {
				.lr = segment_number,
				.key_len = admin_key_len,
			},
		},
		.l_state = OPAL_RO,
	};
	memcpy(user_add_to_lr->session.opal_key.key, admin_key, admin_key_len);

	r = ioctl(fd, IOC_OPAL_ADD_USR_TO_LR, user_add_to_lr);
	if (r != OPAL_STATUS_SUCCESS) {
		log_dbg(cd, "Failed to add OPAL user to locking range %u (RO) on device '%s': %s",
			segment_number, crypt_get_device_name(cd), opal_status_to_string(r));
		r = -EINVAL;
		goto out;
	}
	user_add_to_lr->l_state = OPAL_RW;
	r = ioctl(fd, IOC_OPAL_ADD_USR_TO_LR, user_add_to_lr);
	if (r != OPAL_STATUS_SUCCESS) {
		log_dbg(cd, "Failed to add OPAL user to locking range %u (RW) on device '%s': %s",
			segment_number, crypt_get_device_name(cd), opal_status_to_string(r));
		r = -EINVAL;
		goto out;
	}

	new_pw = crypt_safe_alloc(sizeof(struct opal_new_pw));
	if (!new_pw) {
		r = -ENOMEM;
		goto out;
	}
	*new_pw = (struct opal_new_pw) {
		.session = {
			.who = OPAL_ADMIN1,
			.opal_key = {
				.lr = segment_number,
				.key_len = admin_key_len,
			},
		},
		.new_user_pw = {
			.who = segment_number + 1,
			.opal_key = {
				.key_len = vk->keylength,
				.lr = segment_number,
			},
		},
	};
	memcpy(new_pw->new_user_pw.opal_key.key, vk->key, vk->keylength);
	memcpy(new_pw->session.opal_key.key, admin_key, admin_key_len);

	log_dbg(cd, "User authority key length: %zu", vk->keylength);

	r = ioctl(fd, IOC_OPAL_SET_PW, new_pw);
	if (r != OPAL_STATUS_SUCCESS) {
		log_dbg(cd, "Failed to set OPAL user password on device '%s': (%d) %s",
			crypt_get_device_name(cd), r, opal_status_to_string(r));
		r = -EINVAL;
		goto out;
	}

	setup = crypt_safe_alloc(sizeof(struct opal_user_lr_setup));
	if (!setup) {
		r = -ENOMEM;
		goto out;
	}
	*setup = (struct opal_user_lr_setup) {
		.range_start = range_start,
		.range_length = range_length,
		/* Some drives do not enable Locking Ranges on setup. This have some
		 * interesting consequences: Lock command called later below will pass,
		 * but locking range will _not_ be locked at all.
		 */
		.RLE = 1,
		.WLE = 1,
		.session = {
			.who = OPAL_ADMIN1,
			.opal_key = {
				.key_len = admin_key_len,
				.lr = segment_number,
			},
		},
	};
	memcpy(setup->session.opal_key.key, admin_key, admin_key_len);

	r = ioctl(fd, IOC_OPAL_LR_SETUP, setup);
	if (r != OPAL_STATUS_SUCCESS) {
		log_dbg(cd, "Failed to setup locking range of length %llu at offset %llu on OPAL device '%s': %s",
			setup->range_length, setup->range_start, crypt_get_device_name(cd), opal_status_to_string(r));
		r = -EINVAL;
		goto out;
	}

	/* After setup an OPAL device is unlocked, but the expectation with cryptsetup is that it needs
	 * to be activated separately, so lock it immediately. */
	lock = crypt_safe_alloc(sizeof(struct opal_lock_unlock));
	if (!lock) {
		r = -ENOMEM;
		goto out;
	}
	*lock = (struct opal_lock_unlock) {
		.l_state = OPAL_LK,
		.session = {
			.who = segment_number + 1,
			.opal_key = {
				.key_len = vk->keylength,
				.lr = segment_number,
			},
		}
	};
	memcpy(lock->session.opal_key.key, vk->key, vk->keylength);

	r = ioctl(fd, IOC_OPAL_LOCK_UNLOCK, lock);
	if (r != OPAL_STATUS_SUCCESS) {
		log_dbg(cd, "Failed to lock OPAL device '%s': %s",
			crypt_get_device_name(cd), opal_status_to_string(r));
		r = -EINVAL;
		goto out;
	}

	/* Double check the locking range is locked and the ranges are set up as configured */
	r = opal_range_check_attributes_fd(cd, fd, segment_number, vk, &range_start,
					   &range_length, &(bool) {true}, &(bool){true});
out:
	crypt_safe_free(activate);
	crypt_safe_free(user_session);
	crypt_safe_free(user_add_to_lr);
	crypt_safe_free(new_pw);
	crypt_safe_free(setup);
	crypt_safe_free(lock);

	return r;
}

static int opal_lock_unlock(struct crypt_device *cd,
			    struct device *dev,
			    uint32_t segment_number,
			    const struct volume_key *vk,
			    bool lock)
{
	struct opal_lock_unlock unlock = {
		.l_state = lock ? OPAL_LK : OPAL_RW,
		.session = {
			.who = segment_number + 1,
			.opal_key = {
				.lr = segment_number,
			},
		},
	};
	int r, fd;

	if (opal_supported(cd, dev) <= 0)
		return -ENOTSUP;
	if (!lock && !vk)
		return -EINVAL;

	fd = device_open(cd, dev, O_RDWR);
	if (fd < 0)
		return -EIO;

	if (!lock) {
		assert(vk->keylength <= OPAL_KEY_MAX);

		unlock.session.opal_key.key_len = vk->keylength;
		memcpy(unlock.session.opal_key.key, vk->key, vk->keylength);
	}

	r = ioctl(fd, IOC_OPAL_LOCK_UNLOCK, &unlock);
	if (r < 0) {
		r = -ENOTSUP;
		log_dbg(cd, "OPAL not supported on this kernel version, refusing.");
		goto out;
	}
	if (r == OPAL_STATUS_NOT_AUTHORIZED) /* We'll try again with a different key. */ {
		r = -EPERM;
		log_dbg(cd, "Failed to %slock OPAL device '%s': permission denied",
			lock ? "" : "un", crypt_get_device_name(cd));
		goto out;
	}
	if (r != OPAL_STATUS_SUCCESS) {
		log_dbg(cd, "Failed to %slock OPAL device '%s': %s",
			lock ? "" : "un", crypt_get_device_name(cd), opal_status_to_string(r));
		r = -EINVAL;
		goto out;
	}

	/* If we are unlocking, also tell the kernel to automatically unlock when resuming
	 * from suspend, otherwise the drive will be locked and everything will go up in flames.
	 * Also set the flag to allow locking without having to pass the key again.
	 * But do not error out if this fails, as the device will already be unlocked. */
	if (!lock) {
		unlock.flags = OPAL_SAVE_FOR_LOCK;
		r = ioctl(fd, IOC_OPAL_SAVE, &unlock);
		if (r != OPAL_STATUS_SUCCESS) {
			log_std(cd, "Failed to prepare OPAL device '%s' for sleep resume, be aware before suspending: %s",
				crypt_get_device_name(cd), opal_status_to_string(r));
			r = 0;
		}
	}
out:
	if (!lock)
		crypt_safe_memzero(unlock.session.opal_key.key, unlock.session.opal_key.key_len);

	return r;
}

int opal_lock(struct crypt_device *cd, struct device *dev, uint32_t segment_number)
{
	return opal_lock_unlock(cd, dev, segment_number, NULL, /* lock= */ true);
}

int opal_unlock(struct crypt_device *cd,
		struct device *dev,
		uint32_t segment_number,
		const struct volume_key *vk)
{
	return opal_lock_unlock(cd, dev, segment_number, vk, /* lock= */ false);
}

int opal_factory_reset(struct crypt_device *cd,
		       struct device *dev,
		       const char *password,
		       size_t password_len)
{
	struct opal_key reset = {
		.key_len = password_len,
	};
	int r, fd;

	assert(cd);
	assert(dev);
	assert(password);

	if (password_len > OPAL_KEY_MAX)
		return -EINVAL;

	fd = device_open(cd, dev, O_RDWR);
	if (fd < 0)
		return -EIO;

	memcpy(reset.key, password, password_len);

	r = ioctl(fd, IOC_OPAL_PSID_REVERT_TPR, &reset);
	if (r < 0) {
		r = -ENOTSUP;
		log_dbg(cd, "OPAL not supported on this kernel version, refusing.");
		goto out;
	}
	if (r == OPAL_STATUS_NOT_AUTHORIZED) /* We'll try again with a different key. */ {
		r = -EPERM;
		log_dbg(cd, "Failed to reset OPAL device '%s', incorrect PSID?",
			crypt_get_device_name(cd));
		goto out;
	}
	if (r != OPAL_STATUS_SUCCESS) {
		r = -EINVAL;
		log_dbg(cd, "Failed to reset OPAL device '%s' with PSID: %s",
			crypt_get_device_name(cd), opal_status_to_string(r));
		goto out;
	}
out:
	crypt_safe_memzero(reset.key, reset.key_len);

	return r;
}

int opal_reset_segment(struct crypt_device *cd,
		       struct device *dev,
		       uint32_t segment_number,
		       const char *password,
		       size_t password_len)
{
	struct opal_session_info *user_session = NULL;
	struct opal_user_lr_setup *setup = NULL;
	int r, fd;

	assert(cd);
	assert(dev);
	assert(password);

	if (password_len > OPAL_KEY_MAX)
		return -EINVAL;

	if (opal_enabled(cd, dev) <= 0)
		return -EINVAL;

	user_session = crypt_safe_alloc(sizeof(struct opal_session_info));
	if (!user_session)
		return -ENOMEM;
	*user_session = (struct opal_session_info) {
		.who = OPAL_ADMIN1,
		.opal_key = {
			.lr = segment_number,
			.key_len = password_len,
		},
	};
	memcpy(user_session->opal_key.key, password, password_len);

	fd = device_open(cd, dev, O_RDWR);
	if (fd < 0) {
		r = -EIO;
		goto out;
	}

	r = ioctl(fd, IOC_OPAL_ERASE_LR, user_session);
	if (r != OPAL_STATUS_SUCCESS) {
		log_dbg(cd, "Failed to reset (erase) OPAL locking range %u on device '%s': %s",
			segment_number, crypt_get_device_name(cd), opal_status_to_string(r));
		r = ioctl(fd, IOC_OPAL_SECURE_ERASE_LR, user_session);
		if (r != OPAL_STATUS_SUCCESS) {
			log_dbg(cd, "Failed to reset (secure erase) OPAL locking range %u on device '%s': %s",
				segment_number, crypt_get_device_name(cd), opal_status_to_string(r));
			r = -EINVAL;
			goto out;
		}

		/* Unlike IOC_OPAL_ERASE_LR, IOC_OPAL_SECURE_ERASE_LR does not disable the locking range,
		 * we have to do that by hand.
		 */
		setup = crypt_safe_alloc(sizeof(struct opal_user_lr_setup));
		if (!setup) {
			r = -ENOMEM;
			goto out;
		}
		*setup = (struct opal_user_lr_setup) {
			.range_start = 0,
			.range_length = 0,
			.session = {
				.who = OPAL_ADMIN1,
				.opal_key = user_session->opal_key,
			},
		};

		r = ioctl(fd, IOC_OPAL_LR_SETUP, setup);
		if (r != OPAL_STATUS_SUCCESS) {
			log_dbg(cd, "Failed to disable locking range on OPAL device '%s': %s",
				crypt_get_device_name(cd), opal_status_to_string(r));
			r = -EINVAL;
			goto out;
		}
	}
out:
	crypt_safe_free(user_session);
	crypt_safe_free(setup);

	return r;
}

static int opal_query_status(struct crypt_device *cd, struct device *dev, unsigned expected)
{
	struct opal_status st = { };
	int fd, r;

	assert(cd);
	assert(dev);

	fd = device_open(cd, dev, O_RDWR);
	if (fd < 0)
		return -EIO;

	r = ioctl(fd, IOC_OPAL_GET_STATUS, &st);

	return r < 0 ? -EINVAL : (st.flags & expected) ? 1 : 0;
}

int opal_supported(struct crypt_device *cd, struct device *dev)
{
	return opal_query_status(cd, dev, OPAL_FL_SUPPORTED|OPAL_FL_LOCKING_SUPPORTED);
}

int opal_enabled(struct crypt_device *cd, struct device *dev)
{
	return opal_query_status(cd, dev, OPAL_FL_LOCKING_ENABLED);
}

int opal_geometry(struct crypt_device *cd,
		  struct device *dev,
		  bool *ret_align,
		  uint32_t *ret_block_size,
		  uint64_t *ret_alignment_granularity_blocks,
		  uint64_t *ret_lowest_lba_blocks)
{
	int fd;

	assert(cd);
	assert(dev);

	fd = device_open(cd, dev, O_RDWR);
	if (fd < 0)
		return -EIO;

	return opal_geometry_fd(fd, ret_align, ret_block_size,
				ret_alignment_granularity_blocks, ret_lowest_lba_blocks);
}

int opal_range_check_attributes(struct crypt_device *cd,
		     struct device *dev,
		     uint32_t segment_number,
		     const struct volume_key *vk,
		     const uint64_t *check_offset_sectors,
		     const uint64_t *check_length_sectors,
		     bool *check_read_locked,
		     bool *check_write_locked)
{
	int fd;

	assert(cd);
	assert(dev);
	assert(vk);

	fd = device_open(cd, dev, O_RDWR);
	if (fd < 0)
		return -EIO;

	return opal_range_check_attributes_fd(cd, fd, segment_number, vk,
					      check_offset_sectors, check_length_sectors, check_read_locked,
					      check_write_locked);
}

#else

int opal_setup_ranges(struct crypt_device *cd,
		      struct device *dev,
		      const struct volume_key *vk,
		      uint64_t range_start,
		      uint64_t range_length,
		      uint32_t segment_number,
		      const void *admin_key,
		      size_t admin_key_len)
{
	return -ENOTSUP;
}

int opal_lock(struct crypt_device *cd, struct device *dev, uint32_t segment_number)
{
	return -ENOTSUP;
}

int opal_unlock(struct crypt_device *cd,
		struct device *dev,
		uint32_t segment_number,
		const struct volume_key *vk)
{
	return -ENOTSUP;
}

int opal_supported(struct crypt_device *cd, struct device *dev)
{
	return -ENOTSUP;
}

int opal_enabled(struct crypt_device *cd, struct device *dev)
{
	return -ENOTSUP;
}

int opal_factory_reset(struct crypt_device *cd,
		       struct device *dev,
		       const char *password,
		       size_t password_len)
{
	return -ENOTSUP;
}

int opal_reset_segment(struct crypt_device *cd,
		       struct device *dev,
		       uint32_t segment_number,
		       const char *password,
		       size_t password_len)
{
	return -ENOTSUP;
}

int opal_geometry(struct crypt_device *cd,
		  struct device *dev,
		  bool *ret_align,
		  uint32_t *ret_block_size,
		  uint64_t *ret_alignment_granularity_blocks,
		  uint64_t *ret_lowest_lba_blocks)
{
	return -ENOTSUP;
}

int opal_range_check_attributes(struct crypt_device *cd,
				struct device *dev,
				uint32_t segment_number,
				const struct volume_key *vk,
				const uint64_t *check_offset_sectors,
				const uint64_t *check_length_sectors,
				bool *check_read_locked,
				bool *check_write_locked)
{
	return -ENOTSUP;
}

#endif