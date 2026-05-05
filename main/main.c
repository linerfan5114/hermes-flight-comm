#include "flight_comm.h"

void app_main(void) {
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    gpio_reset_pin(GPIO_NUM_2);
    gpio_set_direction(GPIO_NUM_2, GPIO_MODE_OUTPUT);
    gpio_set_level(GPIO_NUM_2, 0);

    g_audio_tx_queue = xQueueCreate(8, sizeof(audio_block_t));
    g_audio_rx_queue = xQueueCreate(8, sizeof(audio_block_t));
    g_command_queue = xQueueCreate(4, sizeof(uint32_t));

    system_health_init();
    watchdog_init(WATCHDOG_TIMEOUT_SEC);

    flight_i2s_init();

    uint8_t flight_key[ENCRYPTION_KEY_LEN];
    esp_fill_random(flight_key, ENCRYPTION_KEY_LEN);
    encryption_init(flight_key, ENCRYPTION_KEY_LEN);

    radio_init();

    if (system_self_test()) {
        system_state_transition(SYSTEM_STATE_ACTIVE);
    } else {
        system_state_transition(SYSTEM_STATE_FAULT);
    }

    xTaskCreatePinnedToCore(vTaskAudioCapturePrimary, "cap_pri", 16384, NULL, 6, &capture_primary_handle, 0);
    xTaskCreatePinnedToCore(vTaskAudioCaptureRedundant, "cap_red", 16384, NULL, 5, &capture_redundant_handle, 1);
    xTaskCreatePinnedToCore(vTaskAudioPlayback, "playback", 12288, NULL, 5, &playback_handle, 1);
    xTaskCreatePinnedToCore(vTaskHealthMonitor, "health", 8192, NULL, 4, &health_handle, 0);
    xTaskCreatePinnedToCore(vTaskRadioController, "radio_ctrl", 8192, NULL, 3, &radio_handle, 1);
    xTaskCreate(vTaskWatchdogService, "wd_svc", 4096, NULL, 7, NULL);

    gpio_set_level(GPIO_NUM_2, 1);

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}