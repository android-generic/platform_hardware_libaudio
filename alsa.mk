#
# Copyright (C) 2012 The Android-x86 Open Source Project
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#

# Prebuilts/Configs/Firmwares
PRODUCT_PACKAGES += \
	sof-firmware \
	alsa-ucm-conf-common \
	alsa-ucm-conf-amd \
	alsa-ucm-conf-hda \
	alsa-ucm-conf-intel \
	alsa-ucm-conf-samsung \
	alsa-ucm-conf-sof-soundwire \
	alsa-ucm-conf-usb-audio

PRODUCT_COPY_FILES := \
    $(LOCAL_PATH)/policy/audio_policy_criteria.conf:system/etc/audio_policy_criteria.conf \
    $(LOCAL_PATH)/policy/audio_policy_configuration.xml:system/etc/audio_policy_configuration.xml \
    $(LOCAL_PATH)/policy/a2dp_audio_policy_configuration.xml:system/etc/a2dp_audio_policy_configuration.xml \
    $(LOCAL_PATH)/policy/r_submix_audio_policy_configuration.xml:system/etc/r_submix_audio_policy_configuration.xml \
    $(LOCAL_PATH)/policy/usb_audio_policy_configuration.xml:system/etc/usb_audio_policy_configuration.xml \
    $(LOCAL_PATH)/policy/hdmi_audio_policy_configuration.xml:system/etc/hdmi_audio_policy_configuration.xml \
    $(LOCAL_PATH)/policy/bluetooth_audio_policy_configuration_7_0.xml:system/etc/bluetooth_audio_policy_configuration_7_0.xml \
    $(LOCAL_PATH)/policy/a2dp_in_audio_policy_configuration_7_0.xml:system/etc/a2dp_in_audio_policy_configuration_7_0.xml \
    $(LOCAL_PATH)/policy/audio_policy_volumes.xml:system/etc/audio_policy_volumes.xml \
    $(LOCAL_PATH)/policy/default_volume_tables.xml:system/etc/default_volume_tables.xml \
    $(LOCAL_PATH)/effect/audio_effects.xml:system/etc/audio_effects.xml \
    $(LOCAL_PATH)/mixer_paths_0.xml:$(TARGET_COPY_OUT_VENDOR)/etc/mixer_paths_0.xml \
    $(LOCAL_PATH)/mixer_paths_usb.xml:$(TARGET_COPY_OUT_VENDOR)/etc/mixer_paths_usb.xml

PRODUCT_PACKAGES += \
	alsa_mixer \
	alsa_amixer \
	alsa_aplay \
	alsa_ctl \
	alsa_ucm \
	alsa_tplg \
	alsa_iecset \
	libalsatplg_module_nhlt \
	audio.primary.x86 \
	audio.primary.x86_celadon \
	audio.hdmi.x86 \
	audio.bluetooth.default \
	audio.usb.default \
	audio.usb.x86 \
	audio.r_submix.default \
