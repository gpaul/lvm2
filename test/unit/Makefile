# Copyright (C) 2011-2018 Red Hat, Inc. All rights reserved.
#
# This file is part of LVM2.
#
# This copyrighted material is made available to anyone wishing to use,
# modify, copy, or redistribute it subject to the terms and conditions
# of the GNU General Public License v.2.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software Foundation,
# Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA

UNIT_SOURCE=\
	device_mapper/vdo/status.c \
	\
	test/unit/activation-generator_t.c \
	test/unit/bcache_t.c \
	test/unit/bcache_utils_t.c \
	test/unit/bitset_t.c \
	test/unit/config_t.c \
	test/unit/dmlist_t.c \
	test/unit/dmstatus_t.c \
	test/unit/io_engine_t.c \
	test/unit/radix_tree_t.c \
	test/unit/matcher_t.c \
	test/unit/framework.c \
	test/unit/percent_t.c \
	test/unit/run.c \
	test/unit/string_t.c \
	test/unit/vdo_t.c

test/unit/radix_tree_t.o: test/unit/rt_case1.c

UNIT_DEPENDS=$(subst .c,.d,$(UNIT_SOURCE))
UNIT_OBJECTS=$(UNIT_SOURCE:%.c=%.o)
CLEAN_TARGETS+=$(UNIT_DEPENDS) $(UNIT_OBJECTS)

test/unit/unit-test: $(UNIT_OBJECTS) lib/liblvm-internal.a libdaemon/client/libdaemonclient.a $(INTERNAL_LIBS)
	@echo "    [LD] $@"
	$(Q) $(CC) $(CFLAGS) $(LDFLAGS) $(EXTRA_EXEC_LDFLAGS) \
	      -o $@ $+ $(LIBS) $(DMEVENT_LIBS) $(SYSTEMD_LIBS) -lm -ldl -laio

.PHONEY: run-unit-test
run-unit-test: test/unit/unit-test
	@echo Running unit tests
	LD_LIBRARY_PATH=libdm test/unit/unit-test run

ifeq ("$(USE_TRACKING)","yes")
ifeq (,$(findstring $(MAKECMDGOALS),cscope.out cflow clean distclean lcov \
 help check check_local check_lvmpolld run-unit-test))
	-include $(UNIT_DEPENDS)
endif
endif
