#!/bin/sh
# Copyright (C) 2011-2014 Red Hat, Inc.
#
# This copyrighted material is made available to anyone wishing to use,
# modify, copy, or redistribute it subject to the terms and conditions
# of the GNU General Public License v.2.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software Foundation,
# Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA

. lib/paths

CMD=${0##*/}
test "$CMD" != lvm || unset CMD

# When needed to trace command from test suite use env var before program
# and run program directly via shell in test dir i.e.:
# sh shell/activate-mirror.sh
# 'LVM_GDB=1 lvcreate -l1 $vg'
# > run
test -z "$LVM_GDB" || exec gdb --readnow --args "$abs_top_builddir/tools/lvm" $CMD "$@"

# Multiple level of LVM_VALGRIND support
# the higher level the more commands are traced
if test -n "$LVM_VALGRIND"; then
	RUN_VALGRIND="aux run_valgrind";
	case "$CMD" in
	  lvs|pvs|vgs|vgck|vgscan)
		test "$LVM_VALGRIND" -gt 2 || unset RUN_VALGRIND ;;
	  pvcreate|pvremove|lvremove|vgcreate|vgremove)
		test "$LVM_VALGRIND" -gt 1 || unset RUN_VALGRIND ;;
	  *)
		test "$LVM_VALGRIND" -gt 0 || unset RUN_VALGRIND ;;
	esac
fi

# the exec is important, because otherwise fatal signals inside "not" go unnoticed
exec $RUN_VALGRIND "$abs_top_builddir/tools/lvm" $CMD "$@"
