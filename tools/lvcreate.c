/*
 * Copyright (C) 2001  Sistina Software
 *
 * This file is released under the GPL.
 *
 */

#include "tools.h"
#include <fcntl.h>

int lvcreate(int argc, char **argv)
{
	int zero;
	uint32_t read_ahead = 0;
	int stripes = 1;
	int stripesize = 0;

	int opt = 0;
	uint32_t status = 0;
	uint32_t size = 0;
	uint32_t size_rest;
	uint32_t extents = 0;
	struct volume_group *vg;
	struct logical_volume *lv;
	struct list *lvh, *pvh, *pvl;
	char *lv_name = NULL;
	char *vg_name;
	char *st;

	if (arg_count(snapshot_ARG) || arg_count(chunksize_ARG)) {
		log_error("Snapshots are not yet supported in LVM2.");
		return EINVALID_CMD_LINE;
	}

	/* mutually exclusive */
	if ((arg_count(zero_ARG) && arg_count(snapshot_ARG)) ||
	    (arg_count(extents_ARG) && arg_count(size_ARG))) {
		log_error("Invalid combination of arguments");
		return EINVALID_CMD_LINE;
	}

	if (arg_count(size_ARG) + arg_count(extents_ARG) == 0) {
		log_error("Please indicate size using option -l or -L");
		return EINVALID_CMD_LINE;
	}

	if (strcmp(arg_str_value(contiguous_ARG, "n"), "n"))
		status |= ALLOC_CONTIGUOUS;
	else
		status |= ALLOC_SIMPLE;

	zero = strcmp(arg_str_value(zero_ARG, "y"), "n");

	if (arg_count(stripes_ARG)) {
		stripes = arg_int_value(stripes_ARG, 1);
		if (stripes == 1)
			log_print("Redundant stripes argument: default is 1");
	}

	if (arg_count(stripesize_ARG))
		stripesize = 2 * arg_int_value(stripesize_ARG, 0);

	if (stripes == 1 && stripesize) {
		log_print("Ignoring stripesize argument with single stripe");
		stripesize = 0;
	}

	if (stripes > 1 && !stripesize) {
		stripesize = 2 * STRIPE_SIZE_DEFAULT;
		log_print("Using default stripesize %dKB", stripesize / 2);
	}

	if (arg_count(permission_ARG))
		status |= arg_int_value(permission_ARG, 0);
	else
		status |= LVM_READ | LVM_WRITE;

	if (arg_count(readahead_ARG))
		read_ahead = arg_int_value(readahead_ARG, 0);

	if (arg_count(extents_ARG))
		extents = arg_int_value(extents_ARG, 0);

	/* Size returned in kilobyte units; held in sectors */
	if (arg_count(size_ARG))
		size = arg_int_value(size_ARG, 0);

	if (arg_count(name_ARG))
		lv_name = arg_value(name_ARG);

	/* If VG not on command line, try -n arg and then environment */
	if (!argc) {
		if (!(vg_name = extract_vgname(fid, lv_name))) {
			log_error("Please provide a volume group name");
			return EINVALID_CMD_LINE;
		}
	} else {
		/* Ensure lv_name doesn't contain a different VG! */
		if (lv_name && strchr(lv_name, '/')) {
			if (!(vg_name = extract_vgname(fid, lv_name)))
				return EINVALID_CMD_LINE;
			if (strcmp(vg_name, argv[0])) {
				log_error("Inconsistent volume group names "
					  "given: %s and %s", vg_name, argv[0]);
				return EINVALID_CMD_LINE;
			}
		}
		vg_name = argv[0];
		argv++;
		argc--;
	}

	if (lv_name && (st = strrchr(lv_name, '/')))
		lv_name = st + 1;

	/* does VG exist? */
	log_verbose("Finding volume group %s", vg_name);
	if (!(vg = fid->ops->vg_read(fid, vg_name))) {
		log_error("Volume group %s doesn't exist", vg_name);
		return ECMD_FAILED;
	}

/******* Removed check
	if (!(vg->status & ACTIVE)) {
		log_error("Volume group %s must be active before changing a "
			  "logical volume", vg_name);
		return ECMD_FAILED;
	}
********/

	if (lv_name && (lvh = find_lv_in_vg(vg, lv_name))) {
		log_error("Logical volume %s already exists in "
			  "volume group %s", lv_name, vg_name);
		return ECMD_FAILED;
	}

	if (argc) {
		/* Build up list of PVs */
		if (!(pvh = pool_alloc(fid->cmd->mem, sizeof(struct list)))) {
			log_error("pvh list allocation failed");
			return ECMD_FAILED;
		}
		list_init(pvh);
		for (; opt < argc; opt++) {
			if (!(pvl = find_pv_in_vg(vg, argv[opt]))) {
				log_error("Physical Volume %s not found in "
					  "Volume Group %s", argv[opt],
					  vg->name);
				return EINVALID_CMD_LINE;
			}
			if (list_item(pvl, struct pv_list)->pv.pe_count ==
			    list_item(pvl, struct pv_list)->pv.pe_allocated) {
				log_error("No free extents on physical volume"
					  " %s", argv[opt]);
				continue;
				/* FIXME But check not null at end! */
			}
			list_add(pvh, pvl);
		}
	} else {
		/* Use full list from VG */
		pvh = &vg->pvs;
	}

	if (argc && argc < stripes ) {
		log_error("Too few physical volumes on "
			  "command line for %d-way striping", stripes);
		return EINVALID_CMD_LINE;
	}

	if (stripes < 1 || stripes > MAX_STRIPES) {
		log_error("Number of stripes (%d) must be between %d and %d", 
			  stripes, 1, MAX_STRIPES);
		return EINVALID_CMD_LINE;
	}

	if (stripes > 1 && (stripesize < STRIPE_SIZE_MIN ||
			    stripesize > STRIPE_SIZE_MAX ||
			    stripesize & (stripesize - 1))) {
		log_error("Invalid stripe size %d", stripesize);
		return EINVALID_CMD_LINE;
	}

	if (stripesize > vg->extent_size) {
		log_error("Setting stripe size %d KB to physical extent "
			  "size %u KB",
		     	  stripesize / 2, vg->extent_size / 2);
		stripesize = vg->extent_size;
	}

	if (size) {
		/* No of 512-byte sectors */
		extents = size * 2;

		if (extents % vg->extent_size) {
			char *s1;

			extents += vg->extent_size - extents % vg->extent_size;
			log_print("Rounding up size to full physical extent %s",
				  (s1 = display_size(extents / 2, SIZE_SHORT)));
			dbg_free(s1);
		}

		extents /= vg->extent_size;
	}

        if ((size_rest = extents % stripes)) {
                log_print("Rounding size (%d extents) up to stripe boundary "
                          "size (%d extents)", extents,
                          extents - size_rest + stripes);
                extents = extents - size_rest + stripes;
        }

	if (!(lv = lv_create(lv_name, status, stripes, stripesize, extents,
			     vg, pvh))) return ECMD_FAILED;

	if (arg_count(readahead_ARG)) {
		log_verbose("Setting read ahead sectors");
		lv->read_ahead = read_ahead;
	}

	/* store vg on disk(s) */
	if (!fid->ops->vg_write(fid, vg))
		return ECMD_FAILED;

	log_print("Logical volume %s created", lv->name);

	if (!lv_activate(lv))
		return ECMD_FAILED;

	if (zero) {
		struct device *dev;
		char *name;

		if (!(name = dbg_malloc(NAME_LEN))) {
			log_error("Name allocation failed - device not zeroed");
			return ECMD_FAILED;
		}

		snprintf(name, NAME_LEN, "%s%s/%s", fid->cmd->dev_dir,
			 lv->vg->name, lv->name);

		log_verbose("Zeroing start of logical volume %s", name);

		if (!(dev = dev_cache_get(name, NULL))) {
			log_error("%s not found: device not zeroed", name);
			return ECMD_FAILED;
		}
		if (!(dev_open(dev, O_WRONLY)))
			return ECMD_FAILED;
		dev_zero(dev, 0, 4096);
		dev_close(dev);

	} else
		log_print("WARNING: %s not zeroed", lv->name);

/******** FIXME backup
	if ((ret = do_autobackup(vg_name, vg)))
		return ret;
***********/

	return 0;
}
