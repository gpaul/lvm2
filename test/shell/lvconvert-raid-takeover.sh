#!/bin/sh
# Copyright (C) 2016,2017 Red Hat, Inc. All rights reserved.
#
# This copyrighted material is made available to anyone wishing to use,
# modify, copy, or redistribute it subject to the terms and conditions
# of the GNU General Public License v.2.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software Foundation,
# Inc., 51 Franklin Street, Fifth Floor, Boston, MA2110-1301 USA

SKIP_WITH_LVMLOCKD=1
SKIP_WITH_LVMPOLLD=1

. lib/inittest

aux have_raid 1 9 0 || skip

correct_raid4_layout=0
aux have_raid 1 9 1 && correct_raid4_layout=1

aux prepare_vg 6 80

function _lvcreate
{
	local level=$1
	local req_stripes=$2
	local stripes=$3
	local size=$4
	local vg=$5
	local lv=$6

	lvcreate -y -aey --type $level -i $req_stripes -L $size -n $lv $vg
	check lv_field $vg/$lv segtype "$level"
	check lv_field $vg/$lv stripes $stripes
	echo y | mkfs -t ext4 /dev/mapper/$vg-$lv
	fsck -fn  /dev/mapper/$vg-$lv
}

function _lvconvert
{
	local req_level=$1
	local level=$2
	local stripes=$3
	local vg=$4
	local lv=$5
	local region_size=$6
	local wait_and_check=1
	local R=""

	[ -n "$region_size" ] && R="-R $region_size"
	[ "${level:0:7}" = "striped" ] && wait_and_check=0
	[ "${level:0:5}" = "raid0" ] && wait_and_check=0


	lvconvert -y --ty $req_level $R $vg/$lv
	[ $? -ne 0 ] && return $?
	check lv_field $vg/$lv segtype "$level"
	check lv_field $vg/$lv stripes $stripes
	if [ "$wait_and_check" -eq 1 ]
	then
		fsck -fn  /dev/mapper/$vg-$lv
		aux wait_for_sync $vg $lv
	fi
	fsck -fn  /dev/mapper/$vg-$lv
}

function _invalid_raid5_conversions
{
	local vg=$1
	local lv=$2

	not _lvconvert striped 4 $vg $lv1
	not _lvconvert raid0 raid0 4 $vg $lv1
	not _lvconvert raid0_meta raid0_meta 4 $vg $lv1
	not _lvconvert raid4 raid4 5 $vg $lv1
	not _lvconvert raid5_ls raid5_ls 5 $vg $lv1
	not _lvconvert raid5_rs raid5_rs 5 $vg $lv1
	not _lvconvert raid5_la raid5_la 5 $vg $lv1
	not _lvconvert raid5_ra raid5_ra 5 $vg $lv1
	not _lvconvert raid6_zr raid6_zr 6 $vg $lv1
	not _lvconvert raid6_nr raid6_nr 6 $vg $lv1
	not _lvconvert raid6_nc raid6_nc 6 $vg $lv1
	not _lvconvert raid6_n_6 raid6_n_6 6 $vg $lv1
	not _lvconvert raid6 raid6_n_6 6 $vg $lv1
}

# Delayst leg so that rebuilding status characters
#  can be read before resync finished too quick.
# aux delay_dev "$dev1" 1

# Create 3-way mirror
lvcreate --yes -aey --type mirror -R 64K -m 2 -L 64M -n $lv1 $vg
check lv_field $vg/$lv1 segtype "mirror"
check lv_field $vg/$lv1 stripes 3
check lv_field $vg/$lv1 regionsize "64.00k"
echo y | mkfs -t ext4 /dev/mapper/$vg-$lv1
aux wait_for_sync $vg $lv1
fsck -fn  /dev/mapper/$vg-$lv1

# Convert 3-way to 4-way mirror
lvconvert -m 3 $vg/$lv1
check lv_field $vg/$lv1 segtype "mirror"
check lv_field $vg/$lv1 stripes 4
fsck -fn  /dev/mapper/$vg-$lv1
aux wait_for_sync $vg $lv1
fsck -fn  /dev/mapper/$vg-$lv1

# Takeover 4-way mirror to raid1
lvconvert --yes --type raid1 -R 64k $vg/$lv1
check lv_field $vg/$lv1 segtype "raid1"
check lv_field $vg/$lv1 stripes 4
check lv_field $vg/$lv1 regionsize "64.00k"
fsck -fn  /dev/mapper/$vg-$lv1

## Convert 4-way raid1 to 5-way
lvconvert -m 4 -R 128K $vg/$lv1
check lv_field $vg/$lv1 segtype "raid1"
check lv_field $vg/$lv1 stripes 5
# FIXME: once lv_raid_chanage_image_count() supports region_size changes
not check lv_field $vg/$lv1 regionsize "128.00k"
fsck -fn  /dev/mapper/$vg-$lv1
aux wait_for_sync $vg $lv1
fsck -fn  /dev/mapper/$vg-$lv1

# FIXME: enable once lvconvert rejects early
## Try converting 4-way raid1 to 9-way
#not lvconvert --yes -m 8 $vg/$lv1
#check lv_field $vg/$lv1 segtype "raid1"
#check lv_field $vg/$lv1 stripes 4

# Convert 5-way raid1 to 2-way
lvconvert --yes -m 1 $vg/$lv1
lvs $vg/$lv1
dmsetup status $vg-$lv1
dmsetup table $vg-$lv1
check lv_field $vg/$lv1 segtype "raid1"
check lv_field $vg/$lv1 stripes 2
fsck -fn  /dev/mapper/$vg-$lv1

# Convert 2-way raid1 to mirror
lvconvert --yes --type mirror -R 32K $vg/$lv1
check lv_field $vg/$lv1 segtype "mirror"
check lv_field $vg/$lv1 stripes 2
check lv_field $vg/$lv1 regionsize "32.00k"
aux wait_for_sync $vg $lv1
fsck -fn  /dev/mapper/$vg-$lv1
aux wait_for_sync $vg $lv1

# Clean up
lvremove --yes $vg/$lv1


if [ $correct_raid4_layout -eq 1 ]
then

#
# Start out with raid4
#

# Create 3-way striped raid4 (4 legs total)
_lvcreate raid4 3 4 64M $vg $lv1
aux wait_for_sync $vg $lv1

# Convert raid4 -> striped
_lvconvert striped striped 3 $vg $lv1

# Convert striped -> raid4
_lvconvert raid4 raid4 4 $vg $lv1 64k
check lv_field $vg/$lv1 regionsize "64.00k"

# Convert raid4 -> raid5_n
_lvconvert raid5 raid5_n 4 $vg $lv1 128k
check lv_field $vg/$lv1 regionsize "128.00k"

# Convert raid5_n -> striped
_lvconvert striped striped 3 $vg $lv1

# Convert striped -> raid5_n
_lvconvert raid5_n raid5_n 4 $vg $lv1

# Convert raid5_n -> raid4
_lvconvert raid4 raid4 4 $vg $lv1

# Convert raid4 -> raid0
_lvconvert raid0 raid0 3 $vg $lv1

# Convert raid0 -> raid5_n
_lvconvert raid5_n raid5_n 4 $vg $lv1

# Convert raid5_n -> raid0_meta
_lvconvert raid0_meta raid0_meta 3 $vg $lv1

# Convert raid0_meta -> raid5_n
_lvconvert raid5 raid5_n 4 $vg $lv1

# Convert raid4 -> raid0_meta
_lvconvert raid0_meta raid0_meta 3 $vg $lv1

# Convert raid0_meta -> raid4
_lvconvert raid4 raid4 4 $vg $lv1

# Convert raid4 -> raid0
_lvconvert raid0 raid0 3 $vg $lv1

# Convert raid0 -> raid4
_lvconvert raid4 raid4 4 $vg $lv1

# Convert raid4 -> striped
_lvconvert striped striped 3 $vg $lv1

# Convert striped -> raid6_n_6
_lvconvert raid6_n_6 raid6_n_6 5 $vg $lv1

# Convert raid6_n_6 -> striped
_lvconvert striped striped 3 $vg $lv1

# Convert striped -> raid6_n_6
_lvconvert raid6 raid6_n_6 5 $vg $lv1

# Convert raid6_n_6 -> raid5_n
_lvconvert raid5_n raid5_n 4 $vg $lv1

# Convert raid5_n -> raid6_n_6
_lvconvert raid6_n_6 raid6_n_6 5 $vg $lv1

# Convert raid6_n_6 -> raid4
_lvconvert raid4 raid4 4 $vg $lv1

# Convert raid4 -> raid6_n_6
_lvconvert raid6 raid6_n_6 5 $vg $lv1

# Convert raid6_n_6 -> raid0
_lvconvert raid0 raid0 3 $vg $lv1

# Convert raid0 -> raid6_n_6
_lvconvert raid6_n_6 raid6_n_6 5 $vg $lv1

# Convert raid6_n_6 -> raid0_meta
_lvconvert raid0_meta raid0_meta 3 $vg $lv1

# Convert raid0_meta -> raid6_n_6
_lvconvert raid6 raid6_n_6 5 $vg $lv1

# Clean up
lvremove -y $vg

# Create + convert 4-way raid5 variations
_lvcreate raid5 4 5 64M $vg $lv1
aux wait_for_sync $vg $lv1
_invalid_raid5_conversions $vg $lv1
not _lvconvert raid6_rs_6 raid6_rs_6 6 $vg $lv1
not _lvconvert raid6_la_6 raid6_la_6 6 $vg $lv1
not _lvconvert raid6_ra_6 raid6_ra_6 6 $vg $lv1
_lvconvert raid6_ls_6 raid6_ls_6 6 $vg $lv1
_lvconvert raid5_ls raid5_ls 5 $vg $lv1
lvremove -y $vg

_lvcreate raid5_ls 4 5 64M $vg $lv1
aux wait_for_sync $vg $lv1
_invalid_raid5_conversions $vg $lv1
not _lvconvert raid6_rs_6 raid6_rs_6 6 $vg $lv1
not _lvconvert raid6_la_6 raid6_la_6 6 $vg $lv1
not _lvconvert raid6_ra_6 raid6_ra_6 6 $vg $lv1
_lvconvert raid6_ls_6 raid6_ls_6 6 $vg $lv1
_lvconvert raid5_ls raid5_ls 5 $vg $lv1
lvremove -y $vg

_lvcreate raid5_rs 4 5 64M $vg $lv1
aux wait_for_sync $vg $lv1
_invalid_raid5_conversions $vg $lv1
not _lvconvert raid6_ra_6 raid6_ra_6 6 $vg $lv1
not _lvconvert raid6_la_6 raid6_la_6 6 $vg $lv1
not _lvconvert raid6_ra_6 raid6_ra_6 6 $vg $lv1
_lvconvert raid6_rs_6 raid6_rs_6 6 $vg $lv1
_lvconvert raid5_rs raid5_rs 5 $vg $lv1
lvremove -y $vg

_lvcreate raid5_la 4 5 64M $vg $lv1
aux wait_for_sync $vg $lv1
_invalid_raid5_conversions $vg $lv1
not _lvconvert raid6_ls_6 raid6_ls_6 6 $vg $lv1
not _lvconvert raid6_rs_6 raid6_rs_6 6 $vg $lv1
not _lvconvert raid6_ra_6 raid6_ra_6 6 $vg $lv1
_lvconvert raid6_la_6 raid6_la_6 6 $vg $lv1
_lvconvert raid5_la raid5_la 5 $vg $lv1
lvremove -y $vg

_lvcreate raid5_ra 4 5 64M $vg $lv1
aux wait_for_sync $vg $lv1
_invalid_raid5_conversions $vg $lv1
not _lvconvert raid6_ls_6 raid6_ls_6 6 $vg $lv1
not _lvconvert raid6_rs_6 raid6_rs_6 6 $vg $lv1
not _lvconvert raid6_la_6 raid6_la_6 6 $vg $lv1
_lvconvert raid6_ra_6 raid6_ra_6 6 $vg $lv1
_lvconvert raid5_ra raid5_ra 5 $vg $lv1
lvremove -y $vg

else

not lvcreate -y -aey --type raid4 -i 3 -L 64M -n $lv4 $vg
not lvconvert -y --ty raid4 $vg/$lv1
not lvconvert -y --ty raid4 $vg/$lv2

fi

vgremove -ff $vg
