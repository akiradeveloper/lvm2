/*
 * Copyright (C) 2001-2004 Sistina Software, Inc. All rights reserved.
 * Copyright (C) 2004-2007 Red Hat, Inc. All rights reserved.
 *
 * This file is part of LVM2.
 *
 * This copyrighted material is made available to anyone wishing to use,
 * modify, copy, or redistribute it subject to the terms and conditions
 * of the GNU Lesser General Public License v.2.1.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include "lib.h"
#include "metadata.h"
#include "display.h"
#include "activate.h"
#include "toolcontext.h"
#include "segtype.h"
#include "defaults.h"
#include "lvm-signal.h"

#include <stdarg.h>

#define SIZE_BUF 128

typedef enum { SIZE_LONG = 0, SIZE_SHORT = 1, SIZE_UNIT = 2 } size_len_t;

static const struct {
	alloc_policy_t alloc;
	const char str[14]; /* must be changed when size extends 13 chars */
	const char repchar;
} _policies[] = {
	{
	ALLOC_CONTIGUOUS, "contiguous", 'c'}, {
	ALLOC_CLING, "cling", 'l'}, {
	ALLOC_CLING_BY_TAGS, "cling_by_tags", 't'}, {	/* Only used in log mesgs */
	ALLOC_NORMAL, "normal", 'n'}, {
	ALLOC_ANYWHERE, "anywhere", 'a'}, {
	ALLOC_INHERIT, "inherit", 'i'}
};

static const int _num_policies = DM_ARRAY_SIZE(_policies);

char alloc_policy_char(alloc_policy_t alloc)
{
	int i;

	for (i = 0; i < _num_policies; i++)
		if (_policies[i].alloc == alloc)
			return _policies[i].repchar;

	return '-';
}

const char *get_alloc_string(alloc_policy_t alloc)
{
	int i;

	for (i = 0; i < _num_policies; i++)
		if (_policies[i].alloc == alloc)
			return _policies[i].str;

	return NULL;
}

alloc_policy_t get_alloc_from_string(const char *str)
{
	int i;

	/* cling_by_tags is part of cling */
	if (!strcmp("cling_by_tags", str))
		return ALLOC_CLING;

	for (i = 0; i < _num_policies; i++)
		if (!strcmp(_policies[i].str, str))
			return _policies[i].alloc;

	/* Special case for old metadata */
	if (!strcmp("next free", str))
		return ALLOC_NORMAL;

	log_error("Unrecognised allocation policy %s", str);
	return ALLOC_INVALID;
}

static const char *_percent_types[7] = { "NONE", "VG", "FREE", "LV", "PVS", "ORIGIN" };

const char *get_percent_string(percent_type_t def)
{
	return _percent_types[def];
}

const char *display_lvname(const struct logical_volume *lv)
{
	/* On allocation failure, just return the LV name. */
	return lv_fullname_dup(lv->vg->cmd->mem, lv) ? : lv->name;
}

#define BASE_UNKNOWN 0
#define BASE_SHARED 1
#define BASE_1024 8
#define BASE_1000 15
#define BASE_SPECIAL 21
#define NUM_UNIT_PREFIXES 6
#define NUM_SPECIAL 3

/* Size supplied in sectors */
static const char *_display_size(const struct cmd_context *cmd,
				 uint64_t size, size_len_t sl)
{
	unsigned base = BASE_UNKNOWN;
	unsigned s;
	int suffix, precision;
	uint64_t byte = UINT64_C(0);
	uint64_t units = UINT64_C(1024);
	char *size_buf = NULL;
	const char * const size_str[][3] = {
		/* BASE_UNKNOWN */
		{"         ", "   ", " "},	/* [0] */

		/* BASE_SHARED - Used if cmd->si_unit_consistency = 0 */
		{" Exabyte", " EB", "E"},	/* [1] */
		{" Petabyte", " PB", "P"},	/* [2] */
		{" Terabyte", " TB", "T"},	/* [3] */
		{" Gigabyte", " GB", "G"},	/* [4] */
		{" Megabyte", " MB", "M"},	/* [5] */
		{" Kilobyte", " KB", "K"},	/* [6] */
		{" Byte    ", " B", "B"},	/* [7] */

		/* BASE_1024 - Used if cmd->si_unit_consistency = 1 */
		{" Exbibyte", " EiB", "e"},	/* [8] */
		{" Pebibyte", " PiB", "p"},	/* [9] */
		{" Tebibyte", " TiB", "t"},	/* [10] */
		{" Gibibyte", " GiB", "g"},	/* [11] */
		{" Mebibyte", " MiB", "m"},	/* [12] */
		{" Kibibyte", " KiB", "k"},	/* [13] */
		{" Byte    ", " B", "b"},	/* [14] */

		/* BASE_1000 - Used if cmd->si_unit_consistency = 1 */
		{" Exabyte",  " EB", "E"},	/* [15] */
		{" Petabyte", " PB", "P"},	/* [16] */
		{" Terabyte", " TB", "T"},	/* [17] */
		{" Gigabyte", " GB", "G"},	/* [18] */
		{" Megabyte", " MB", "M"},	/* [19] */
		{" Kilobyte", " kB", "K"},	/* [20] */

		/* BASE_SPECIAL */
		{" Byte    ", " B ", "B"},	/* [21] (shared with BASE_1000) */
		{" Units   ", " Un", "U"},	/* [22] */
		{" Sectors ", " Se", "S"},	/* [23] */
	};

	if (!(size_buf = dm_pool_alloc(cmd->mem, SIZE_BUF))) {
		log_error("no memory for size display buffer");
		return "";
	}

	suffix = cmd->current_settings.suffix;

	if (!cmd->si_unit_consistency) {
		/* Case-independent match */
		for (s = 0; s < NUM_UNIT_PREFIXES; s++)
			if (toupper((int) cmd->current_settings.unit_type) ==
			    *size_str[BASE_SHARED + s][2]) {
				base = BASE_SHARED;
				break;
			}
	} else {
		/* Case-dependent match for powers of 1000 */
		for (s = 0; s < NUM_UNIT_PREFIXES; s++)
			if (cmd->current_settings.unit_type ==
			    *size_str[BASE_1000 + s][2]) {
				base = BASE_1000;
				break;
			}

		/* Case-dependent match for powers of 1024 */
		if (base == BASE_UNKNOWN)
			for (s = 0; s < NUM_UNIT_PREFIXES; s++)
			if (cmd->current_settings.unit_type ==
			    *size_str[BASE_1024 + s][2]) {
				base = BASE_1024;
				break;
			}
	}

	if (base == BASE_UNKNOWN)
		/* Check for special units - s, b or u */
		for (s = 0; s < NUM_SPECIAL; s++)
			if (toupper((int) cmd->current_settings.unit_type) ==
			    *size_str[BASE_SPECIAL + s][2]) {
				base = BASE_SPECIAL;
				break;
			}

	if (size == UINT64_C(0)) {
		if (base == BASE_UNKNOWN)
			s = 0;
		sprintf(size_buf, "0%s", suffix ? size_str[base + s][sl] : "");
		return size_buf;
	}

	size *= UINT64_C(512);

	if (base != BASE_UNKNOWN)
		byte = cmd->current_settings.unit_factor;
	else {
		/* Human-readable style */
		if (cmd->current_settings.unit_type == 'H') {
			units = UINT64_C(1000);
			base = BASE_1000;
		} else {
			units = UINT64_C(1024);
			base = BASE_1024;
		}

		if (!cmd->si_unit_consistency)
			base = BASE_SHARED;

		byte = units * units * units * units * units * units;

		for (s = 0; s < NUM_UNIT_PREFIXES && size < byte; s++)
			byte /= units;

		suffix = 1;
	}

	/* FIXME Make precision configurable */
	switch (toupper(*size_str[base + s][SIZE_UNIT])) {
	case 'B':
	case 'S':
		precision = 0;
		break;
	default:
		precision = 2;
	}

	snprintf(size_buf, SIZE_BUF - 1, "%.*f%s", precision,
		 (double) size / byte, suffix ? size_str[base + s][sl] : "");

	return size_buf;
}

const char *display_size_long(const struct cmd_context *cmd, uint64_t size)
{
	return _display_size(cmd, size, SIZE_LONG);
}

const char *display_size_units(const struct cmd_context *cmd, uint64_t size)
{
	return _display_size(cmd, size, SIZE_UNIT);
}

const char *display_size(const struct cmd_context *cmd, uint64_t size)
{
	return _display_size(cmd, size, SIZE_SHORT);
}

void pvdisplay_colons(const struct physical_volume *pv)
{
	char uuid[64] __attribute__((aligned(8)));

	if (!pv)
		return;

	if (!id_write_format(&pv->id, uuid, sizeof(uuid))) {
		stack;
		return;
	}

	log_print("%s:%s:%" PRIu64 ":-1:%" PRIu64 ":%" PRIu64 ":-1:%" PRIu32 ":%u:%u:%u:%s",
		  pv_dev_name(pv), pv_vg_name(pv), pv->size,
		  /* FIXME pv->pv_number, Derive or remove? */
		  pv->status,	/* FIXME Support old or new format here? */
		  pv->status & ALLOCATABLE_PV,	/* FIXME remove? */
		  /* FIXME pv->lv_cur, Remove? */
		  pv->pe_size / 2,
		  pv->pe_count,
		  pv->pe_count - pv->pe_alloc_count,
		  pv->pe_alloc_count, *uuid ? uuid : "none");
}

void pvdisplay_segments(const struct physical_volume *pv)
{
	const struct pv_segment *pvseg;

	if (pv->pe_size)
		log_print("--- Physical Segments ---");

	dm_list_iterate_items(pvseg, &pv->segments) {
		log_print("Physical extent %u to %u:",
			  pvseg->pe, pvseg->pe + pvseg->len - 1);

		if (pvseg_is_allocated(pvseg)) {
			log_print("  Logical volume\t%s%s/%s",
				  pvseg->lvseg->lv->vg->cmd->dev_dir,
				  pvseg->lvseg->lv->vg->name,
				  pvseg->lvseg->lv->name);
			log_print("  Logical extents\t%d to %d",
				  pvseg->lvseg->le, pvseg->lvseg->le +
				  pvseg->lvseg->len - 1);
		} else
			log_print("  FREE");
	}

	log_print(" ");
}

/* FIXME Include label fields */
void pvdisplay_full(const struct cmd_context *cmd,
		    const struct physical_volume *pv,
		    void *handle __attribute__((unused)))
{
	char uuid[64] __attribute__((aligned(8)));
	const char *size;

	uint32_t pe_free;
	uint64_t data_size, pvsize, unusable;

	if (!pv)
		return;

	if (!id_write_format(&pv->id, uuid, sizeof(uuid))) {
		stack;
		return;
	}

	log_print("--- %sPhysical volume ---", pv->pe_size ? "" : "NEW ");
	log_print("PV Name               %s", pv_dev_name(pv));
	log_print("VG Name               %s%s",
		  is_orphan(pv) ? "" : pv->vg_name,
		  pv->status & EXPORTED_VG ? " (exported)" : "");

	data_size = (uint64_t) pv->pe_count * pv->pe_size;
	if (pv->size > data_size + pv->pe_start) {
		pvsize = pv->size;
		unusable = pvsize - data_size;
	} else {
		pvsize = data_size + pv->pe_start;
		unusable = pvsize - pv->size;
	}

	size = display_size(cmd, pvsize);
	if (data_size)
		log_print("PV Size               %s / not usable %s",	/*  [LVM: %s]", */
			  size, display_size(cmd, unusable));
	else
		log_print("PV Size               %s", size);

	/* PV number not part of LVM2 design
	   log_print("PV#                   %u", pv->pv_number);
	 */

	pe_free = pv->pe_count - pv->pe_alloc_count;
	if (pv->pe_count && (pv->status & ALLOCATABLE_PV))
		log_print("Allocatable           yes %s",
			  (!pe_free && pv->pe_count) ? "(but full)" : "");
	else
		log_print("Allocatable           NO");

	/* LV count is no longer available when displaying PV
	   log_print("Cur LV                %u", vg->lv_count);
	 */

	if (cmd->si_unit_consistency)
		log_print("PE Size               %s", display_size(cmd, (uint64_t) pv->pe_size));
	else
		log_print("PE Size (KByte)       %" PRIu32, pv->pe_size / 2);

	log_print("Total PE              %u", pv->pe_count);
	log_print("Free PE               %" PRIu32, pe_free);
	log_print("Allocated PE          %u", pv->pe_alloc_count);
	log_print("PV UUID               %s", *uuid ? uuid : "none");
	log_print(" ");
}

int pvdisplay_short(const struct cmd_context *cmd __attribute__((unused)),
		    const struct volume_group *vg __attribute__((unused)),
		    const struct physical_volume *pv,
		    void *handle __attribute__((unused)))
{
	char uuid[64] __attribute__((aligned(8)));

	if (!pv)
		return 0;

	if (!id_write_format(&pv->id, uuid, sizeof(uuid)))
		return_0;

	log_print("PV Name               %s     ", pv_dev_name(pv));
	/* FIXME  pv->pv_number); */
	log_print("PV UUID               %s", *uuid ? uuid : "none");
	log_print("PV Status             %sallocatable",
		  (pv->status & ALLOCATABLE_PV) ? "" : "NOT ");
	log_print("Total PE / Free PE    %u / %u",
		  pv->pe_count, pv->pe_count - pv->pe_alloc_count);

	log_print(" ");
	return 0;
}

void lvdisplay_colons(const struct logical_volume *lv)
{
	int inkernel;
	struct lvinfo info;
	inkernel = lv_info(lv->vg->cmd, lv, 0, &info, 1, 0) && info.exists;

	log_print("%s%s/%s:%s:%" PRIu64 ":%d:-1:%d:%" PRIu64 ":%d:-1:%d:%d:%d:%d",
		  lv->vg->cmd->dev_dir,
		  lv->vg->name,
		  lv->name,
		  lv->vg->name,
		  ((lv->status & (LVM_READ | LVM_WRITE)) >> 8) |
		  ((inkernel && info.read_only) ? 4 : 0), inkernel ? 1 : 0,
		  /* FIXME lv->lv_number,  */
		  inkernel ? info.open_count : 0, lv->size, lv->le_count,
		  /* FIXME Add num allocated to struct! lv->lv_allocated_le, */
		  (lv->alloc == ALLOC_CONTIGUOUS ? 2 : 0), lv->read_ahead,
		  inkernel ? info.major : -1, inkernel ? info.minor : -1);
}

int lvdisplay_full(struct cmd_context *cmd,
		   const struct logical_volume *lv,
		   void *handle __attribute__((unused)))
{
	struct lvinfo info;
	int inkernel, snap_active = 0;
	char uuid[64] __attribute__((aligned(8)));
	const char *access_str;
	struct lv_segment *snap_seg = NULL, *mirror_seg = NULL;
	struct lv_segment *seg = NULL;
	int lvm1compat;
	dm_percent_t snap_percent;
	int thin_data_active = 0, thin_metadata_active = 0;
	dm_percent_t thin_data_percent, thin_metadata_percent;
	int thin_active = 0;
	dm_percent_t thin_percent;

	if (!id_write_format(&lv->lvid.id[1], uuid, sizeof(uuid)))
		return_0;

	inkernel = lv_info(cmd, lv, 0, &info, 1, 1) && info.exists;

	if ((lv->status & LVM_WRITE) && inkernel && info.read_only)
		access_str = "read/write (activated read only)";
	else if (lv->status & LVM_WRITE)
		access_str = "read/write";
	else
		access_str = "read only";

	log_print("--- Logical volume ---");

	lvm1compat = find_config_tree_bool(cmd, global_lvdisplay_shows_full_device_path_CFG, NULL);

	if (lvm1compat)
		/* /dev/vgname/lvname doen't actually exist for internal devices */
		log_print("LV Name                %s%s/%s",
			  lv->vg->cmd->dev_dir, lv->vg->name, lv->name);
	else if (lv_is_visible(lv)) {
		/* Thin pool does not have /dev/vg/name link */
		if (!lv_is_thin_pool(lv))
			log_print("LV Path                %s%s/%s",
				  lv->vg->cmd->dev_dir,
				  lv->vg->name, lv->name);
		log_print("LV Name                %s", lv->name);
	} else
		log_print("Internal LV Name       %s", lv->name);

	log_print("VG Name                %s", lv->vg->name);
	log_print("LV UUID                %s", uuid);
	log_print("LV Write Access        %s", access_str);
	log_print("LV Creation host, time %s, %s",
		  lv_host_dup(cmd->mem, lv), lv_time_dup(cmd->mem, lv));

	if (lv_is_origin(lv)) {
		log_print("LV snapshot status     source of");

		dm_list_iterate_items_gen(snap_seg, &lv->snapshot_segs,
				       origin_list) {
			if (inkernel &&
			    (snap_active = lv_snapshot_percent(snap_seg->cow,
							       &snap_percent)))
				if (snap_percent == DM_PERCENT_INVALID)
					snap_active = 0;
			if (lvm1compat)
				log_print("                       %s%s/%s [%s]",
					  lv->vg->cmd->dev_dir, lv->vg->name,
					  snap_seg->cow->name,
					  snap_active ? "active" : "INACTIVE");
			else
				log_print("                       %s [%s]",
					  snap_seg->cow->name,
					  snap_active ? "active" : "INACTIVE");
		}
		snap_seg = NULL;
	} else if ((snap_seg = find_snapshot(lv))) {
		if (inkernel &&
		    (snap_active = lv_snapshot_percent(snap_seg->cow,
						       &snap_percent)))
			if (snap_percent == DM_PERCENT_INVALID)
				snap_active = 0;

		if (lvm1compat)
			log_print("LV snapshot status     %s destination for %s%s/%s",
				  snap_active ? "active" : "INACTIVE",
				  lv->vg->cmd->dev_dir, lv->vg->name,
				  snap_seg->origin->name);
		else
			log_print("LV snapshot status     %s destination for %s",
				  snap_active ? "active" : "INACTIVE",
				  snap_seg->origin->name);
	}

	if (lv_is_thin_volume(lv)) {
		seg = first_seg(lv);
		log_print("LV Pool name           %s", seg->pool_lv->name);
		if (seg->origin)
			log_print("LV Thin origin name    %s",
				  seg->origin->name);
		if (seg->external_lv)
			log_print("LV External origin name %s",
				  seg->external_lv->name);
		if (seg->merge_lv)
			log_print("LV merging to          %s",
				  seg->merge_lv->name);
		if (inkernel)
			thin_active = lv_thin_percent(lv, 0, &thin_percent);
		if (lv_is_merging_origin(lv))
			log_print("LV merged with         %s",
				  find_snapshot(lv)->lv->name);
	} else if (lv_is_thin_pool(lv)) {
		if (lv_info(cmd, lv, 1, &info, 1, 1) && info.exists) {
			thin_data_active = lv_thin_pool_percent(lv, 0, &thin_data_percent);
			thin_metadata_active = lv_thin_pool_percent(lv, 1, &thin_metadata_percent);
		}
		/* FIXME: display thin_pool targets transid for activated LV as well */
		seg = first_seg(lv);
		log_print("LV Pool metadata       %s", seg->metadata_lv->name);
		log_print("LV Pool data           %s", seg_lv(seg, 0)->name);
	}

	if (inkernel && info.suspended)
		log_print("LV Status              suspended");
	else if (activation())
		log_print("LV Status              %savailable",
			  inkernel ? "" : "NOT ");

/********* FIXME lv_number
    log_print("LV #                   %u", lv->lv_number + 1);
************/

	if (inkernel)
		log_print("# open                 %u", info.open_count);

	log_print("LV Size                %s",
		  display_size(cmd,
			       snap_seg ? snap_seg->origin->size : lv->size));

	if (thin_data_active)
		log_print("Allocated pool data    %.2f%%",
			  dm_percent_to_float(thin_data_percent));

	if (thin_metadata_active)
		log_print("Allocated metadata     %.2f%%",
			  dm_percent_to_float(thin_metadata_percent));

	if (thin_active)
		log_print("Mapped size            %.2f%%",
			  dm_percent_to_float(thin_percent));

	log_print("Current LE             %u",
		  snap_seg ? snap_seg->origin->le_count : lv->le_count);

	if (snap_seg) {
		log_print("COW-table size         %s",
			  display_size(cmd, (uint64_t) lv->size));
		log_print("COW-table LE           %u", lv->le_count);

		if (snap_active)
			log_print("Allocated to snapshot  %.2f%%",
				  dm_percent_to_float(snap_percent));

		log_print("Snapshot chunk size    %s",
			  display_size(cmd, (uint64_t) snap_seg->chunk_size));
	}

	if (lv_is_mirrored(lv)) {
		mirror_seg = first_seg(lv);
		log_print("Mirrored volumes       %" PRIu32, mirror_seg->area_count);
		if (lv_is_converting(lv))
			log_print("LV type        Mirror undergoing conversion");
	}

	log_print("Segments               %u", dm_list_size(&lv->segments));

/********* FIXME Stripes & stripesize for each segment
	log_print("Stripe size            %s", display_size(cmd, (uint64_t) lv->stripesize));
***********/

	log_print("Allocation             %s", get_alloc_string(lv->alloc));
	if (lv->read_ahead == DM_READ_AHEAD_AUTO)
		log_print("Read ahead sectors     auto");
	else if (lv->read_ahead == DM_READ_AHEAD_NONE)
		log_print("Read ahead sectors     0");
	else
		log_print("Read ahead sectors     %u", lv->read_ahead);

	if (inkernel && lv->read_ahead != info.read_ahead)
		log_print("- currently set to     %u", info.read_ahead);

	if (lv->status & FIXED_MINOR) {
		if (lv->major >= 0)
			log_print("Persistent major       %d", lv->major);
		log_print("Persistent minor       %d", lv->minor);
	}

	if (inkernel)
		log_print("Block device           %d:%d", info.major,
			  info.minor);

	log_print(" ");

	return 0;
}

void display_stripe(const struct lv_segment *seg, uint32_t s, const char *pre)
{
	switch (seg_type(seg, s)) {
	case AREA_PV:
		/* FIXME Re-check the conditions for 'Missing' */
		log_print("%sPhysical volume\t%s", pre,
			  seg_pv(seg, s) ?
			  pv_dev_name(seg_pv(seg, s)) :
			    "Missing");

		if (seg_pv(seg, s))
			log_print("%sPhysical extents\t%d to %d", pre,
				  seg_pe(seg, s),
				  seg_pe(seg, s) + seg->area_len - 1);
		break;
	case AREA_LV:
		log_print("%sLogical volume\t%s", pre,
			  seg_lv(seg, s) ?
			  seg_lv(seg, s)->name : "Missing");

		if (seg_lv(seg, s))
			log_print("%sLogical extents\t%d to %d", pre,
				  seg_le(seg, s),
				  seg_le(seg, s) + seg->area_len - 1);
		break;
	case AREA_UNASSIGNED:
		log_print("%sUnassigned area", pre);
	}
}

int lvdisplay_segments(const struct logical_volume *lv)
{
	const struct lv_segment *seg;

	log_print("--- Segments ---");

	dm_list_iterate_items(seg, &lv->segments) {
		log_print("%s extents %u to %u:",
			  lv_is_virtual(lv) ? "Virtual" : "Logical",
			  seg->le, seg->le + seg->len - 1);

		log_print("  Type\t\t%s", seg->segtype->ops->name(seg));

		if (seg->segtype->ops->target_monitored)
			log_print("  Monitoring\t\t%s",
				  lvseg_monitor_dup(lv->vg->cmd->mem, seg));

		if (seg->segtype->ops->display)
			seg->segtype->ops->display(seg);
	}

	log_print(" ");
	return 1;
}

void vgdisplay_extents(const struct volume_group *vg __attribute__((unused)))
{
}

void vgdisplay_full(const struct volume_group *vg)
{
	uint32_t access_str;
	uint32_t active_pvs;
	char uuid[64] __attribute__((aligned(8)));

	active_pvs = vg->pv_count - vg_missing_pv_count(vg);

	log_print("--- Volume group ---");
	log_print("VG Name               %s", vg->name);
	log_print("System ID             %s", vg->system_id);
	log_print("Format                %s", vg->fid->fmt->name);
	if (vg->fid->fmt->features & FMT_MDAS) {
		log_print("Metadata Areas        %d",
			  vg_mda_count(vg));
		log_print("Metadata Sequence No  %d", vg->seqno);
	}
	access_str = vg->status & (LVM_READ | LVM_WRITE);
	log_print("VG Access             %s%s%s%s",
		  access_str == (LVM_READ | LVM_WRITE) ? "read/write" : "",
		  access_str == LVM_READ ? "read" : "",
		  access_str == LVM_WRITE ? "write" : "",
		  access_str == 0 ? "error" : "");
	log_print("VG Status             %s%sresizable",
		  vg_is_exported(vg) ? "exported/" : "",
		  vg_is_resizeable(vg) ? "" : "NOT ");
	/* vg number not part of LVM2 design
	   log_print ("VG #                  %u\n", vg->vg_number);
	 */
	if (vg_is_clustered(vg)) {
		log_print("Clustered             yes");
		log_print("Shared                %s",
			  vg->status & SHARED ? "yes" : "no");
	}

	log_print("MAX LV                %u", vg->max_lv);
	log_print("Cur LV                %u", vg_visible_lvs(vg));
	log_print("Open LV               %u", lvs_in_vg_opened(vg));
/****** FIXME Max LV Size
      log_print ( "MAX LV Size           %s",
               ( s1 = display_size ( LVM_LV_SIZE_MAX(vg))));
      free ( s1);
*********/
	log_print("Max PV                %u", vg->max_pv);
	log_print("Cur PV                %u", vg->pv_count);
	log_print("Act PV                %u", active_pvs);

	log_print("VG Size               %s",
		  display_size(vg->cmd,
			       (uint64_t) vg->extent_count * vg->extent_size));

	log_print("PE Size               %s",
		  display_size(vg->cmd, vg->extent_size));

	log_print("Total PE              %u", vg->extent_count);

	log_print("Alloc PE / Size       %u / %s",
		  vg->extent_count - vg->free_count,
		  display_size(vg->cmd,
			       (uint64_t) (vg->extent_count - vg->free_count) *
			       vg->extent_size));

	log_print("Free  PE / Size       %u / %s", vg->free_count,
		  display_size(vg->cmd, vg_free(vg)));

	if (!id_write_format(&vg->id, uuid, sizeof(uuid))) {
		stack;
		return;
	}

	log_print("VG UUID               %s", uuid);
	log_print(" ");
}

void vgdisplay_colons(const struct volume_group *vg)
{
	uint32_t active_pvs;
	const char *access_str;
	char uuid[64] __attribute__((aligned(8)));

	active_pvs = vg->pv_count - vg_missing_pv_count(vg);

	switch (vg->status & (LVM_READ | LVM_WRITE)) {
		case LVM_READ | LVM_WRITE:
			access_str = "r/w";
			break;
		case LVM_READ:
			access_str = "r";
			break;
		case LVM_WRITE:
			access_str = "w";
			break;
		default:
			access_str = "";
	}

	if (!id_write_format(&vg->id, uuid, sizeof(uuid))) {
		stack;
		return;
	}

	log_print("%s:%s:%" PRIu64 ":-1:%u:%u:%u:-1:%u:%u:%u:%" PRIu64 ":%" PRIu32
		  ":%u:%u:%u:%s",
		vg->name,
		access_str,
		vg->status,
		/* internal volume group number; obsolete */
		vg->max_lv,
		vg_visible_lvs(vg),
		lvs_in_vg_opened(vg),
		/* FIXME: maximum logical volume size */
		vg->max_pv,
		vg->pv_count,
		active_pvs,
		(uint64_t) vg->extent_count * (vg->extent_size / 2),
		vg->extent_size / 2,
		vg->extent_count,
		vg->extent_count - vg->free_count,
		vg->free_count,
		uuid[0] ? uuid : "none");
}

void vgdisplay_short(const struct volume_group *vg)
{
	log_print("\"%s\" %-9s [%-9s used / %s free]", vg->name,
/********* FIXME if "open" print "/used" else print "/idle"???  ******/
		  display_size(vg->cmd,
			       (uint64_t) vg->extent_count * vg->extent_size),
		  display_size(vg->cmd,
			       ((uint64_t) vg->extent_count -
				vg->free_count) * vg->extent_size),
		  display_size(vg->cmd, vg_free(vg)));
}

void display_formats(const struct cmd_context *cmd)
{
	const struct format_type *fmt;

	dm_list_iterate_items(fmt, &cmd->formats) {
		log_print("%s", fmt->name);
	}
}

void display_segtypes(const struct cmd_context *cmd)
{
	const struct segment_type *segtype;

	dm_list_iterate_items(segtype, &cmd->segtypes) {
		log_print("%s", segtype->name);
	}
}

void display_tags(const struct cmd_context *cmd)
{
	const struct dm_str_list *sl;

	dm_list_iterate_items(sl, &cmd->tags) {
		log_print("%s", sl->str);
	}
}

void display_name_error(name_error_t name_error)
{
	switch(name_error) {
	case NAME_VALID:
		/* Valid name */
		break;
	case NAME_INVALID_EMPTY:
		log_error("Name is zero length.");
		break;
	case NAME_INVALID_HYPEN:
		log_error("Name cannot start with hyphen.");
		break;
	case NAME_INVALID_DOTS:
		log_error("Name starts with . or .. and has no "
			  "following character(s).");
		break;
	case NAME_INVALID_CHARSET:
		log_error("Name contains invalid character, valid set includes: "
			  "[a-zA-Z0-9.-_+].");
		break;
	case NAME_INVALID_LENGTH:
		/* Report that name length - 1 to accommodate nul*/
		log_error("Name length exceeds maximum limit of %d.", (NAME_LEN - 1));
		break;
	default:
		log_error(INTERNAL_ERROR "Unknown error %d on name validation.", name_error);
		break;
	}
}

/*
 * Prompt for y or n from stdin.
 * Defaults to 'no' in silent mode.
 * All callers should support --yes and/or --force to override this.
 */
char yes_no_prompt(const char *prompt, ...)
{
	int c = 0, ret = 0, cb = 0;
	va_list ap;

	sigint_allow();
	do {
		if (c == '\n' || !c) {
			va_start(ap, prompt);
			vfprintf(stderr, prompt, ap);
			va_end(ap);
			fflush(stderr);
			if (silent_mode()) {
				fputc('n', stderr);
				ret = 'n';
				break;
			}
			ret = 0;
		}

		if ((c = getchar()) == EOF) {
			ret = 'n'; /* SIGINT */
			cb = 1;
			break;
		}

		c = tolower(c);
		if ((c == 'y') || (c == 'n')) {
			/* If both 'y' and 'n' given, begin again. */
			if (ret && c != ret)
				ret = -1;
			else
				ret = c;
		}
	} while (ret < 1 || c != '\n');

	sigint_restore();

	if (cb && !sigint_caught())
		fputc(ret, stderr);

	if (c != '\n')
		fputc('\n', stderr);

	return ret;
}
