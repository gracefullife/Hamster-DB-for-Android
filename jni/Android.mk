# Copyright (C) 2009 The Android Open Source Project
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#
#




# ========================================================
# Build Static Library 
# ========================================================

LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)

LOCAL_MODULE:= hamsterdb-java

LOCAL_SRC_FILES:= log.c \
		blob.c \
		btree.c \
		btree_check.c \
		btree_enum.c \
		btree_erase.c \
		btree_find.c \
		btree_insert.c \
		cache.c \
		db.c \
		env.c \
		error.c \
		extkeys.c \
		freelist.c \
		freelist_v2.c \
		freelist_statistics.c \
		keys.c \
		mem.c \
		hamsterdb.c \
		os_posix.c \
		page.c \
		statistics.c \
		txn.c \
		util.c \
		btree_cursor.c \
		device.c \
		hamsterdb_jni.c

LOCAL_C_INCLUDES += $(LOCAL_PATH)/../include $(LOCAL_PATH)/../../platforms/android-9/arch-arm/usr/include/
LOCAL_CFLAGS += $(LOCAL_C_INCLUDES:%=-I%) -DHAM_LITTLE_ENDIAN -DHAM_DISABLE_COMPRESSION -DHAM_DISABLE_ENCRYPTION
LOCAL_STATIC_LIBS:= -lstdlib

include $(BUILD_SHARED_LIBRARY)


