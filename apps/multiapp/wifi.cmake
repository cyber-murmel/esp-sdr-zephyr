# SPDX-License-Identifier: GPL-3.0-or-later
#
# The stock Wi-Fi driver as a module: what a build with CONFIG_WIFI_ESP32 adds to one without, for
# the options in ext/wifi.conf, compiled with that Kconfig (extconfig.py). The host is built without
# the driver, so nothing here has to be taken off its targets. A source the hal renames fails the
# build; a new one it needs shows as an undefined symbol in the host's link. The supplicant uses its
# internal crypto, so the module has no mbedTLS (ext/wifi_shim).

set(wifi_hal ${ZEPHYR_HAL_ESPRESSIF_MODULE_DIR})
set(wifi_sup ${wifi_hal}/components/wpa_supplicant)

set(WIFI_SOURCES
  ${ZEPHYR_BASE}/drivers/wifi/esp32/src/esp_wifi_drv.c
  ${CMAKE_CURRENT_LIST_DIR}/ext/wifi_glue.c
  # Allocates through k_malloc(), which the module wraps. The host has its own copy.
  ${wifi_hal}/zephyr/port/heap/heap_caps_zephyr.c
  ${ESP_SDR_PHY_SOURCES}
  ${wifi_hal}/components/esp_coex/esp32s3/esp_coex_adapter.c
  ${wifi_hal}/components/esp_wifi/esp32s3/esp_adapter.c
  ${wifi_hal}/components/esp_wifi/regulatory/esp_wifi_regulatory.c
  ${wifi_hal}/components/esp_wifi/src/lib_printf.c
  ${wifi_hal}/components/esp_wifi/src/wifi_init.c
  ${wifi_hal}/zephyr/port/stubs/wifi_compat_stubs.c
  ${wifi_sup}/esp_supplicant/src/esp_common.c
  ${wifi_sup}/esp_supplicant/src/esp_eap_client.c
  ${wifi_sup}/esp_supplicant/src/esp_hostap.c
  ${wifi_sup}/esp_supplicant/src/esp_owe.c
  ${wifi_sup}/esp_supplicant/src/esp_wpa2_api_port.c
  ${wifi_sup}/esp_supplicant/src/esp_wpa3.c
  ${wifi_sup}/esp_supplicant/src/esp_wpa_main.c
  ${wifi_sup}/esp_supplicant/src/esp_wpas_glue.c
  ${wifi_sup}/esp_supplicant/src/esp_wps.c
  ${wifi_sup}/port/eloop.c
  ${wifi_sup}/port/os_xtensa.c
  ${wifi_sup}/src/ap/ap_config.c
  ${wifi_sup}/src/ap/comeback_token.c
  ${wifi_sup}/src/ap/ieee802_11.c
  ${wifi_sup}/src/ap/ieee802_1x.c
  ${wifi_sup}/src/ap/pmksa_cache_auth.c
  ${wifi_sup}/src/ap/sta_info.c
  ${wifi_sup}/src/ap/wpa_auth.c
  ${wifi_sup}/src/ap/wpa_auth_ie.c
  ${wifi_sup}/src/common/dragonfly.c
  ${wifi_sup}/src/common/ieee802_11_common.c
  ${wifi_sup}/src/common/sae.c
  ${wifi_sup}/src/common/wpa_common.c
  ${wifi_sup}/src/crypto/aes-cbc.c
  ${wifi_sup}/src/crypto/aes-ccm.c
  ${wifi_sup}/src/crypto/aes-ctr.c
  ${wifi_sup}/src/crypto/aes-gcm.c
  ${wifi_sup}/src/crypto/aes-internal-dec.c
  ${wifi_sup}/src/crypto/aes-internal-enc.c
  ${wifi_sup}/src/crypto/aes-internal.c
  ${wifi_sup}/src/crypto/aes-omac1.c
  ${wifi_sup}/src/crypto/aes-siv.c
  ${wifi_sup}/src/crypto/aes-unwrap.c
  ${wifi_sup}/src/crypto/aes-wrap.c
  ${wifi_sup}/src/crypto/ccmp.c
  ${wifi_sup}/src/crypto/crypto_internal-cipher.c
  ${wifi_sup}/src/crypto/crypto_internal-modexp.c
  ${wifi_sup}/src/crypto/crypto_internal-rsa.c
  ${wifi_sup}/src/crypto/crypto_internal.c
  ${wifi_sup}/src/crypto/crypto_ops.c
  ${wifi_sup}/src/crypto/des-internal.c
  ${wifi_sup}/src/crypto/dh_group5.c
  ${wifi_sup}/src/crypto/dh_groups.c
  ${wifi_sup}/src/crypto/md4-internal.c
  ${wifi_sup}/src/crypto/md5-internal.c
  ${wifi_sup}/src/crypto/md5.c
  ${wifi_sup}/src/crypto/ms_funcs.c
  ${wifi_sup}/src/crypto/rc4.c
  ${wifi_sup}/src/crypto/sha1-internal.c
  ${wifi_sup}/src/crypto/sha1-pbkdf2.c
  ${wifi_sup}/src/crypto/sha1-prf.c
  ${wifi_sup}/src/crypto/sha1-tlsprf.c
  ${wifi_sup}/src/crypto/sha1-tprf.c
  ${wifi_sup}/src/crypto/sha1.c
  ${wifi_sup}/src/crypto/sha256-internal.c
  ${wifi_sup}/src/crypto/sha256-kdf.c
  ${wifi_sup}/src/crypto/sha256-prf.c
  ${wifi_sup}/src/crypto/sha256-tlsprf.c
  ${wifi_sup}/src/crypto/sha256.c
  ${wifi_sup}/src/crypto/sha384-internal.c
  ${wifi_sup}/src/crypto/sha384-prf.c
  ${wifi_sup}/src/crypto/sha384-tlsprf.c
  ${wifi_sup}/src/crypto/sha512-internal.c
  ${wifi_sup}/src/crypto/tls_internal.c
  ${wifi_sup}/src/eap_common/eap_wsc_common.c
  ${wifi_sup}/src/eap_peer/chap.c
  ${wifi_sup}/src/eap_peer/eap.c
  ${wifi_sup}/src/eap_peer/eap_common.c
  ${wifi_sup}/src/eap_peer/eap_fast.c
  ${wifi_sup}/src/eap_peer/eap_fast_common.c
  ${wifi_sup}/src/eap_peer/eap_mschapv2.c
  ${wifi_sup}/src/eap_peer/eap_peap.c
  ${wifi_sup}/src/eap_peer/eap_peap_common.c
  ${wifi_sup}/src/eap_peer/eap_tls.c
  ${wifi_sup}/src/eap_peer/eap_tls_common.c
  ${wifi_sup}/src/eap_peer/eap_ttls.c
  ${wifi_sup}/src/eap_peer/mschapv2.c
  ${wifi_sup}/src/rsn_supp/pmksa_cache.c
  ${wifi_sup}/src/rsn_supp/wpa.c
  ${wifi_sup}/src/rsn_supp/wpa_ie.c
  ${wifi_sup}/src/tls/asn1.c
  ${wifi_sup}/src/tls/bignum.c
  ${wifi_sup}/src/tls/pkcs1.c
  ${wifi_sup}/src/tls/pkcs5.c
  ${wifi_sup}/src/tls/pkcs8.c
  ${wifi_sup}/src/tls/rsa.c
  ${wifi_sup}/src/tls/tlsv1_client.c
  ${wifi_sup}/src/tls/tlsv1_client_ocsp.c
  ${wifi_sup}/src/tls/tlsv1_client_read.c
  ${wifi_sup}/src/tls/tlsv1_client_write.c
  ${wifi_sup}/src/tls/tlsv1_common.c
  ${wifi_sup}/src/tls/tlsv1_cred.c
  ${wifi_sup}/src/tls/tlsv1_record.c
  ${wifi_sup}/src/tls/x509v3.c
  ${wifi_sup}/src/utils/bitfield.c
  ${wifi_sup}/src/utils/common.c
  ${wifi_sup}/src/utils/ext_password.c
  ${wifi_sup}/src/utils/json.c
  ${wifi_sup}/src/utils/uuid.c
  ${wifi_sup}/src/utils/wpa_debug.c
  ${wifi_sup}/src/utils/wpabuf.c
  ${wifi_sup}/src/wps/wps.c
  ${wifi_sup}/src/wps/wps_attr_build.c
  ${wifi_sup}/src/wps/wps_attr_parse.c
  ${wifi_sup}/src/wps/wps_attr_process.c
  ${wifi_sup}/src/wps/wps_common.c
  ${wifi_sup}/src/wps/wps_dev_attr.c
  ${wifi_sup}/src/wps/wps_enrollee.c
)
list(REMOVE_DUPLICATES WIFI_SOURCES)

set(WIFI_INCLUDES
  ${CMAKE_CURRENT_LIST_DIR}/ext/wifi_shim
  ${ZEPHYR_BASE}/subsys/net/l2
  ${wifi_sup}/esp_supplicant/include
  ${wifi_sup}/esp_supplicant/src
  ${wifi_sup}/include
  ${wifi_sup}/include/esp_supplicant
  ${wifi_sup}/port/include
  ${wifi_sup}/src
  ${wifi_sup}/src/crypto
  ${wifi_sup}/src/eap_peer
  ${wifi_sup}/src/utils
)

# The hal's zephyr_compile_definitions() for the driver.
set(WIFI_DEFINES
  __ets__
  ESP_SUPPLICANT
  ESPRESSIF_USE
  IEEE8021X_EAPOL
  EAP_PEER_METHOD
  EAP_MSCHAPv2
  EAP_TTLS
  EAP_TLS
  EAP_PEAP
  USE_WPA2_TASK
  CONFIG_WPS
  USE_WPS_TASK
  CONFIG_ECC
  CONFIG_IEEE80211W
  CONFIG_SHA256
  CONFIG_NO_RADIUS
  CONFIG_CRYPTO_INTERNAL
  CONFIG_TLS_INTERNAL_CLIENT
  CONFIG_ESP_WIFI_ENABLED
  CONFIG_ESP_COEX_ENABLED
  CONFIG_SOC_WIFI_SUPPORTED
  CONFIG_ESP_PHY_ENABLE_USB
)

# Its own copy of the PHY's blob, from the phy module's writable archive: a Wi-Fi boot loads neither
# phy nor sdr, and the blobs call far more of it than phy exports.
set(WIFI_LIBS ${libphy})
foreach(blob core net80211 pp espnow)
  multiapp_blob(wifi_lib_${blob} ${wifi_hal}/zephyr/blobs/lib/${CONFIG_SOC_SERIES}/lib${blob}.a)
  list(APPEND WIFI_LIBS ${wifi_lib_${blob}})
endforeach()

multiapp_module(wifi INTERNAL DEVICES FILE_ONLY
  KCONFIG ${CMAKE_CURRENT_LIST_DIR}/ext/wifi.conf
  KCONFIG_OWN "WIFI_ESP32" "ESP32_WIFI_.*" "ESP_WIFI_.*" "ESP_PHY_.*" "ESP32_PHY_.*"
  # Its crypto library, which it does not build; what wifi_glue.c replaces; the radio without Wi-Fi,
  # which this replaces; what only sizes things the host has none of.
  KCONFIG_DROP "MBEDTLS.*" "PSA_.*" "TF_PSA.*" "APP_LINK_WITH_MBEDTLS" "CSPRNG_NEEDED"
               "DYNAMIC_THREAD.*" "ESP_SDR_.*" "HEAP_MEM_POOL_ADD_SIZE_ESP_WIFI"
               "NET_TCP_WORKQ_STACK_SIZE"
  KCONFIG_AFTER ${CMAKE_CURRENT_LIST_DIR}/ext/wifi_compat.h
  PRELUDE ${CMAKE_CURRENT_LIST_DIR}/ext/wifi_prelude.h
  WRAP k_malloc k_aligned_alloc k_calloc k_realloc k_free
       z_impl_k_thread_stack_alloc z_impl_k_thread_stack_free
  LIBS ${WIFI_LIBS}
  BLOBS_LD ${CMAKE_CURRENT_LIST_DIR}/ext/module_wifi.ld
  VERSION_SCRIPT ${CMAKE_CURRENT_LIST_DIR}/ext/wifi.ver
  INCLUDES ${WIFI_INCLUDES}
  DEFINES ${WIFI_DEFINES}
  SOURCES ${WIFI_SOURCES})
