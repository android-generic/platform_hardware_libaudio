#
# Copyright (C) 2012 The Android-x86 Open Source Project
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#

TARGET_ALSA_CONF_DIR := system/usr/share/alsa
LOCAL_ALSA_CONF_DIR  := external/alsa-lib/src/conf

copy_conf := \
	alsa.conf \
	cards/aliases.conf \
	ctl/default.conf \
	pcm/center_lfe.conf \
	pcm/default.conf \
	pcm/dmix.conf \
	pcm/dpl.conf \
	pcm/dsnoop.conf \
	pcm/front.conf \
	pcm/hdmi.conf \
	pcm/iec958.conf \
	pcm/modem.conf \
	pcm/rear.conf \
	pcm/side.conf \
	pcm/surround21.conf \
	pcm/surround40.conf \
	pcm/surround41.conf \
	pcm/surround50.conf \
	pcm/surround51.conf \
	pcm/surround71.conf \
	smixer.conf \

LOCAL_ALSA_INIT_DIR  := external/alsa-utils/alsactl/init

copy_init := \
	00main \
	ca0106 \
	default \
	hda \
	help \
	info \
	test

PRODUCT_COPY_FILES := \
	$(foreach f,$(copy_conf),$(LOCAL_ALSA_CONF_DIR)/$(f):$(TARGET_ALSA_CONF_DIR)/$(f)) \
	$(foreach f,$(copy_init),$(LOCAL_ALSA_INIT_DIR)/$(f):$(TARGET_ALSA_CONF_DIR)/init/$(f)) \
    $(LOCAL_PATH)/policy/audio_policy_criteria.conf:system/etc/audio_policy_criteria.conf \
    $(LOCAL_PATH)/policy/audio_policy_configuration.xml:system/etc/audio_policy_configuration.xml \
    $(LOCAL_PATH)/policy/a2dp_audio_policy_configuration.xml:system/etc/a2dp_audio_policy_configuration.xml \
    $(LOCAL_PATH)/policy/r_submix_audio_policy_configuration.xml:system/etc/r_submix_audio_policy_configuration.xml \
    $(LOCAL_PATH)/policy/usb_audio_policy_configuration.xml:system/etc/usb_audio_policy_configuration.xml \
    $(LOCAL_PATH)/policy/hdmi_audio_policy_configuration.xml:system/etc/hdmi_audio_policy_configuration.xml \
    $(LOCAL_PATH)/policy/audio_policy_volumes.xml:system/etc/audio_policy_volumes.xml \
    $(LOCAL_PATH)/policy/default_volume_tables.xml:system/etc/default_volume_tables.xml \
    $(LOCAL_PATH)/effect/audio_effects.xml:system/etc/audio_effects.xml \
    $(LOCAL_PATH)/mixer_paths_0.xml:$(TARGET_COPY_OUT_VENDOR)/etc/mixer_paths_0.xml \
    $(LOCAL_PATH)/mixer_paths_usb.xml:$(TARGET_COPY_OUT_VENDOR)/etc/mixer_paths_usb.xml

PRODUCT_PACKAGES := \
	alsa_mixer \
	alsa_amixer \
	alsa_aplay \
	alsa_ctl \
	alsa_ucm \
	alsa_iecset \
	audio.primary.x86 \
	audio.primary.x86_celadon \
	audio.hdmi.x86 \
	audio.a2dp.default \
	audio.usb.default \
	audio_policy.default \
	audio.r_submix.default \
