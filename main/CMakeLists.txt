idf_component_register(
    SRCS
        "main.cpp"
        "app_state.cpp"
        "audio_pipeline.cpp"
        "bt_ag.cpp"
        "bt_diag.c"
        "bt_sc_guard.c"
        "level_meter.cpp"
        "pcm_ring.cpp"
        "rtc_clock.cpp"
        "settings.cpp"
        "sound.cpp"
        "spi_lock.cpp"
        "storage.cpp"
        "ui.cpp"
        "wav_writer.cpp"
    INCLUDE_DIRS "."
)

# bt_sc_guard.c・bt_diag.c: Bluedroid の関数をリンク時に差し替える。Bluedroid の内部の型を使うので、
# この 2 つのファイルだけに内部のインクルードを通す
set(bt_dir "${IDF_PATH}/components/bt")
set(bluedroid_dir "${bt_dir}/host/bluedroid")
set_source_files_properties(bt_sc_guard.c bt_diag.c PROPERTIES INCLUDE_DIRECTORIES
    "${bt_dir}/common/include;${bt_dir}/common/osi/include;${bluedroid_dir}/common/include;${bluedroid_dir}/stack/include;${bluedroid_dir}/stack/btm/include;${bluedroid_dir}/device/include;${bluedroid_dir}/hci/include")
target_link_libraries(${COMPONENT_LIB} INTERFACE
    "-Wl,--wrap=btm_sec_set_peer_sec_caps"
    "-u __wrap_btm_sec_set_peer_sec_caps"
    "-Wl,--wrap=btm_acl_role_changed"
    "-u __wrap_btm_acl_role_changed"
    "-Wl,--wrap=esp_vhci_host_send_packet"
    "-u __wrap_esp_vhci_host_send_packet"
    "-Wl,--wrap=esp_vhci_host_register_callback"
    "-u __wrap_esp_vhci_host_register_callback")
