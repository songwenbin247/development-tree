#
# Freescale Management Complex (MC) bus drivers
#
# Copyright (C) 2014 Freescale Semiconductor, Inc.
#
# This file is released under the GPLv2
#
obj-$(CONFIG_FSL_MC_BUS) += mc-bus-driver.o

mc-bus-driver-objs := mc-bus.o \
		      mc-sys.o \
		      dprc.o \
		      dpmng.o \
		      dprc-driver.o \
		      mc-allocator.o \
		      dpmcp.o \
		      dpbp.o

# MC restool kernel support
obj-$(CONFIG_FSL_MC_RESTOOL) += mc-restool.o

# MC DPIO driver
obj-$(CONFIG_FSL_MC_DPIO) += dpio/
