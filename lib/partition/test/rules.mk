LOCAL_DIR := $(GET_LOCAL_DIR)

MODULE := $(LOCAL_DIR)

MODULE_SRCS += $(LOCAL_DIR)/partition_tests.c

MODULE_DEPS += \
	lib/bio \
	lib/cksum \
	lib/partition \
	lib/unittest

include make/module.mk
