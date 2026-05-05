#include "flight_comm.h"

system_health_t g_system_health = {0};
peer_entry_t g_peer_table[MAX_PEERS] = {0};
QueueHandle_t g_audio_tx_queue = NULL;
QueueHandle_t g_audio_rx_queue = NULL;
QueueHandle_t g_command_queue = NULL;
EventGroupHandle_t g_flight_events = NULL;

static TaskHandle_t capture_primary_handle = NULL;
static TaskHandle_t capture_redundant_handle = NULL;
static TaskHandle_t playback_handle = NULL;
static TaskHandle_t health_handle = NULL;
static TaskHandle_t radio_handle = NULL;
static uint8_t master_key[ENCRYPTION_KEY_LEN] = {0};
static bool encryption_ready = false;

static const peer_entry_t default_peers[MAX_PEERS] = {
    { .mac = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}, .callsign = "CDR", .active = false },
    { .mac = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}, .callsign = "PLT", .active = false },
    { .mac = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}, .callsign = "MS1", .active = false },
    { .mac = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}, .callsign = "MS2", .active = false },
    { .mac = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}, .callsign = "EV1", .active = false },
    { .mac = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}, .callsign = "EV2", .active = false }
};

static uint32_t compute_crc32(const uint8_t *data, size_t length) {
    uint32_t crc = 0xFFFFFFFF;
    const uint32_t polynomial = 0xEDB88320;
    for (size_t i = 0; i < length; i++) {
        crc ^= data[i];
        for (int j = 0; j < 8; j++) {
            crc = (crc >> 1) ^ ((crc & 1) ? polynomial : 0);
        }
    }
    return crc ^ 0xFFFFFFFF;
}

static bool validate_memory_block(const void *ptr, size_t size, uint32_t expected_crc) {
    if (!ptr || size == 0) return false;
    uint32_t computed = compute_crc32((const uint8_t *)ptr, size);
    return (computed == expected_crc);
}

void flight_i2s_init(void) {
    i2s_config_t i2s_config = {
        .mode = I2S_MODE_MASTER | I2S_MODE_TX | I2S_MODE_RX,
        .sample_rate = SAMPLE_RATE,
        .bits_per_sample = SAMPLE_BITS,
        .channel_format = CHANNEL_NUM,
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .intr_alloc_flags = ESP_INTR_FLAG_LEVEL3 | ESP_INTR_FLAG_IRAM,
        .dma_buf_count = DMA_BUF_COUNT,
        .dma_buf_len = DMA_BUF_LEN,
        .use_apll = true,
        .tx_desc_auto_clear = true,
        .fixed_mclk = 0,
    };
    i2s_pin_config_t pin_config = {
        .bck_io_num = I2S_BCK_PIN,
        .ws_io_num = I2S_WS_PIN,
        .data_out_num = I2S_DATA_OUT_PIN,
        .data_in_num = I2S_DATA_IN_PIN
    };
    ESP_ERROR_CHECK(i2s_driver_install(I2S_NUM, &i2s_config, 0, NULL));
    ESP_ERROR_CHECK(i2s_set_pin(I2S_NUM, &pin_config));
    ESP_ERROR_CHECK(i2s_set_clk(I2S_NUM, SAMPLE_RATE, SAMPLE_BITS, CHANNEL_NUM));
    i2s_zero_dma_buffer(I2S_NUM);
}

void flight_i2s_deinit(void) {
    i2s_driver_uninstall(I2S_NUM);
}

esp_err_t flight_i2s_read_block(audio_block_t *block, uint8_t channel) {
    if (!block || channel > 1) return ESP_ERR_INVALID_ARG;
    int32_t raw_buffer[AUDIO_CHUNK_SIZE * 2];
    size_t bytes_read = 0;
    esp_err_t ret = i2s_read(I2S_NUM, raw_buffer, sizeof(raw_buffer), &bytes_read, pdMS_TO_TICKS(100));
    if (ret != ESP_OK) {
        system_fault_report(FAULT_I2S_TIMEOUT);
        return ret;
    }
    for (uint32_t i = 0; i < PROCESSING_BLOCK_SIZE && (i * 2 + channel) < (bytes_read / 4); i++) {
        block->samples[i] = raw_buffer[i * 2 + channel];
    }
    block->timestamp = esp_timer_get_time();
    block->crc = compute_crc32((uint8_t *)block->samples, sizeof(block->samples));
    float energy = 0.0f;
    int32_t prev_sample = 0;
    int32_t zero_crossings = 0;
    for (uint32_t i = 0; i < PROCESSING_BLOCK_SIZE; i++) {
        float sample_f = (float)block->samples[i];
        energy += sample_f * sample_f;
        if (i > 0 && ((block->samples[i] ^ prev_sample) < 0)) {
            zero_crossings++;
        }
        prev_sample = block->samples[i];
    }
    block->energy_level = energy / PROCESSING_BLOCK_SIZE;
    block->zero_crossing_rate = (float)zero_crossings / PROCESSING_BLOCK_SIZE;
    return ESP_OK;
}

esp_err_t flight_i2s_write_block(audio_block_t *block, uint8_t channel) {
    if (!block || channel > 1) return ESP_ERR_INVALID_ARG;
    int32_t stereo_buffer[PROCESSING_BLOCK_SIZE * 2];
    for (uint32_t i = 0; i < PROCESSING_BLOCK_SIZE; i++) {
        stereo_buffer[i * 2 + channel] = block->samples[i];
        stereo_buffer[i * 2 + (1 - channel)] = 0;
    }
    size_t bytes_written = 0;
    esp_err_t ret = i2s_write(I2S_NUM, stereo_buffer, sizeof(stereo_buffer), &bytes_written, pdMS_TO_TICKS(100));
    if (ret != ESP_OK) {
        system_fault_report(FAULT_I2S_TIMEOUT);
    }
    return ret;
}

void lms_filter_init(lms_filter_t *filter, float mu, float leakage) {
    if (!filter) return;
    memset(filter->weights, 0, sizeof(filter->weights));
    memset(filter->buffer, 0, sizeof(filter->buffer));
    filter->mu = mu;
    filter->leakage = leakage;
    filter->buffer_index = 0;
    filter->initialized = true;
    filter->crc_checksum = compute_crc32((uint8_t *)filter, sizeof(lms_filter_t) - sizeof(uint32_t));
}

void lms_filter_process(lms_filter_t *filter, const int32_t *input, int32_t *output, uint32_t length) {
    if (!filter || !filter->initialized || !input || !output) return;
    if (!validate_memory_block(filter, sizeof(lms_filter_t) - sizeof(uint32_t), filter->crc_checksum)) {
        lms_filter_init(filter, LMS_MU, LMS_LEAKAGE);
        system_fault_report(FAULT_MEMORY_CORRUPTION);
    }
    for (uint32_t n = 0; n < length; n++) {
        filter->buffer[filter->buffer_index] = (float)input[n];
        float y = 0.0f;
        uint32_t idx = filter->buffer_index;
        for (uint32_t k = 0; k < FILTER_TAP_COUNT; k++) {
            y += filter->weights[k] * filter->buffer[(idx - k + FILTER_TAP_COUNT) % FILTER_TAP_COUNT];
        }
        float error = (float)input[n] - y;
        for (uint32_t k = 0; k < FILTER_TAP_COUNT; k++) {
            filter->weights[k] = filter->leakage * filter->weights[k] + 
                                filter->mu * error * filter->buffer[(idx - k + FILTER_TAP_COUNT) % FILTER_TAP_COUNT];
        }
        output[n] = (int32_t)error;
        filter->buffer_index = (filter->buffer_index + 1) % FILTER_TAP_COUNT;
    }
    filter->crc_checksum = compute_crc32((uint8_t *)filter, sizeof(lms_filter_t) - sizeof(uint32_t));
}

void lms_filter_verify(lms_filter_t *filter) {
    if (!filter || !filter->initialized) return;
    if (!validate_memory_block(filter, sizeof(lms_filter_t) - sizeof(uint32_t), filter->crc_checksum)) {
        lms_filter_init(filter, LMS_MU, LMS_LEAKAGE);
        system_fault_report(FAULT_MEMORY_CORRUPTION);
    }
}

bool packet_header_create(packet_header_t *header, packet_type_t type, uint8_t destination) {
    if (!header) return false;
    header->prefix[0] = 0xDE;
    header->prefix[1] = 0xAD;
    header->prefix[2] = 0xBE;
    header->prefix[3] = 0xEF;
    header->type = type;
    header->sequence = (uint16_t)(esp_random() & 0xFFFF);
    header->timestamp = (uint32_t)(esp_timer_get_time() & 0xFFFFFFFF);
    header->source_id = 0x01;
    header->destination_id = destination;
    header->payload_length = 0;
    header->crc32_header = 0;
    header->crc32_header = compute_crc32((uint8_t *)header, sizeof(packet_header_t) - sizeof(uint32_t));
    return true;
}

bool packet_header_validate(const packet_header_t *header) {
    if (!header) return false;
    if (header->prefix[0] != 0xDE || header->prefix[1] != 0xAD ||
        header->prefix[2] != 0xBE || header->prefix[3] != 0xEF) {
        return false;
    }
    uint32_t computed = compute_crc32((uint8_t *)header, sizeof(packet_header_t) - sizeof(uint32_t));
    return (computed == header->crc32_header);
}

bool voice_packet_pack(voice_packet_t *packet, const audio_block_t *block, uint8_t destination) {
    if (!packet || !block) return false;
    if (!packet_header_create(&packet->header, PACKET_VOICE, destination)) return false;
    memcpy(packet->audio_data, block->samples, sizeof(block->samples));
    esp_fill_random(packet->nonce, NONCE_LEN);
    if (encryption_ready) {
        encryption_encrypt_block((uint8_t *)packet->audio_data, sizeof(packet->audio_data), packet->nonce, packet->auth_tag);
    }
    packet->header.payload_length = sizeof(packet->audio_data) + NONCE_LEN + AUTH_TAG_LEN;
    packet->crc32_payload = compute_crc32((uint8_t *)packet->audio_data, sizeof(packet->audio_data));
    return true;
}

bool voice_packet_unpack(const voice_packet_t *packet, audio_block_t *block) {
    if (!packet || !block) return false;
    if (!packet_header_validate(&packet->header)) {
        g_system_health.crc_errors++;
        return false;
    }
    if (encryption_ready) {
        uint8_t decrypted[sizeof(packet->audio_data)];
        memcpy(decrypted, packet->audio_data, sizeof(packet->audio_data));
        if (!encryption_decrypt_block(decrypted, sizeof(decrypted), packet->nonce, packet->auth_tag)) {
            return false;
        }
        memcpy(block->samples, decrypted, sizeof(block->samples));
    } else {
        memcpy(block->samples, packet->audio_data, sizeof(block->samples));
    }
    uint32_t computed = compute_crc32((uint8_t *)block->samples, sizeof(block->samples));
    if (computed != packet->crc32_payload) {
        g_system_health.crc_errors++;
        return false;
    }
    block->timestamp = packet->header.timestamp;
    block->crc = compute_crc32((uint8_t *)block->samples, sizeof(block->samples));
    return true;
}

void encryption_init(const uint8_t *key, uint32_t key_len) {
    if (!key || key_len == 0) return;
    memset(master_key, 0, sizeof(master_key));
    uint32_t copy_len = (key_len > ENCRYPTION_KEY_LEN) ? ENCRYPTION_KEY_LEN : key_len;
    memcpy(master_key, key, copy_len);
    for (uint32_t i = 0; i < ENCRYPTION_KEY_LEN; i++) {
        master_key[i] ^= (uint8_t)((i * 0x55 + 0xAA) & 0xFF);
    }
    encryption_ready = true;
}

void encryption_encrypt_block(uint8_t *data, uint32_t length, uint8_t *nonce, uint8_t *tag) {
    if (!data || !nonce || !tag || !encryption_ready) return;
    for (uint32_t i = 0; i < length; i++) {
        uint8_t key_byte = master_key[(i + nonce[i % NONCE_LEN]) % ENCRYPTION_KEY_LEN];
        data[i] ^= key_byte;
        data[i] ^= nonce[i % NONCE_LEN];
    }
    uint32_t tag_value = compute_crc32(data, length);
    memcpy(tag, &tag_value, (AUTH_TAG_LEN < 4) ? AUTH_TAG_LEN : 4);
}

bool encryption_decrypt_block(uint8_t *data, uint32_t length, uint8_t *nonce, uint8_t *tag) {
    if (!data || !nonce || !tag || !encryption_ready) return false;
    encryption_encrypt_block(data, length, nonce, tag);
    uint32_t expected_tag;
    memcpy(&expected_tag, tag, (AUTH_TAG_LEN < 4) ? AUTH_TAG_LEN : 4);
    uint32_t computed_tag = compute_crc32(data, length);
    uint32_t mask = ((uint32_t)1 << (AUTH_TAG_LEN * 8)) - 1;
    return (expected_tag & mask) == (computed_tag & mask);
}

static void wifi_init_flight(void) {
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_channel(6, WIFI_SECOND_CHAN_NONE));
    ESP_ERROR_CHECK(esp_wifi_start());
}

void radio_init(void) {
    wifi_init_flight();
    ESP_ERROR_CHECK(esp_now_init());
    ESP_ERROR_CHECK(esp_now_register_recv_cb((esp_now_recv_cb_t)radio_receive_callback));
    ESP_ERROR_CHECK(esp_now_set_pmk(master_key));
    memcpy(g_peer_table, default_peers, sizeof(default_peers));
    for (int i = 0; i < MAX_PEERS; i++) {
        esp_now_peer_info_t peer = {0};
        memcpy(peer.peer_addr, g_peer_table[i].mac, 6);
        peer.channel = 6;
        peer.ifidx = WIFI_IF_STA;
        peer.encrypt = false;
        if (esp_now_add_peer(&peer) != ESP_OK) {
            ESP_LOGW(TAG, "Failed to add peer %s", g_peer_table[i].callsign);
        }
    }
}

void radio_send_packet(const void *packet, uint32_t length, uint8_t destination) {
    if (!packet || length == 0) return;
    for (int i = 0; i < MAX_PEERS; i++) {
        if (g_peer_table[i].active && (destination == 0xFF || i == (destination - 1))) {
            esp_now_send(g_peer_table[i].mac, (const uint8_t *)packet, length);
            g_system_health.total_packets_sent++;
        }
    }
}

void radio_receive_callback(const uint8_t *mac, const uint8_t *data, uint32_t length) {
    if (!mac || !data || length < sizeof(packet_header_t)) return;
    const packet_header_t *header = (const packet_header_t *)data;
    if (!packet_header_validate(header)) return;
    for (int i = 0; i < MAX_PEERS; i++) {
        if (memcmp(mac, g_peer_table[i].mac, 6) == 0) {
            g_peer_table[i].last_heartbeat = (uint32_t)(esp_timer_get_time() & 0xFFFFFFFF);
            g_peer_table[i].missed_heartbeats = 0;
            g_peer_table[i].active = true;
            break;
        }
    }
    g_system_health.total_packets_received++;
    if (header->type == PACKET_VOICE) {
        const voice_packet_t *voice = (const voice_packet_t *)data;
        audio_block_t block;
        if (voice_packet_unpack(voice, &block)) {
            xQueueSend(g_audio_rx_queue, &block, 0);
        }
    } else if (header->type == PACKET_HEARTBEAT) {
        g_system_health.fault_count = 0;
    } else if (header->type == PACKET_EMERGENCY) {
        system_state_transition(SYSTEM_STATE_EMERGENCY);
    }
}

void watchdog_init(uint32_t timeout_seconds) {
    wdt_hal_context_t rtc_wdt_ctx;
    wdt_hal_init(&rtc_wdt_ctx, WDT_RWDT, 0, false);
    wdt_hal_config_stage(&rtc_wdt_ctx, WDT_STAGE0, timeout_seconds * 1000, WDT_STAGE_ACTION_RESET_SYSTEM);
    wdt_hal_enable(&rtc_wdt_ctx);
}

void watchdog_feed(void) {
    wdt_hal_feed(&rtc_wdt_ctx);
}

void watchdog_force_reset(void) {
    wdt_hal_context_t rtc_wdt_ctx;
    wdt_hal_init(&rtc_wdt_ctx, WDT_RWDT, 0, false);
    wdt_hal_config_stage(&rtc_wdt_ctx, WDT_STAGE0, 100, WDT_STAGE_ACTION_RESET_SYSTEM);
    wdt_hal_enable(&rtc_wdt_ctx);
    while(1) { vTaskDelay(1); }
}

void system_health_init(void) {
    memset(&g_system_health, 0, sizeof(g_system_health));
    g_system_health.current_state = SYSTEM_STATE_INIT;
    g_system_health.state_mutex = xSemaphoreCreateMutex();
    g_system_health.watchdog_timer = xTimerCreate("wd_timer", pdMS_TO_TICKS(WATCHDOG_TIMEOUT_SEC * 500), pdTRUE, NULL, (TimerCallbackFunction_t)watchdog_feed);
    if (g_system_health.watchdog_timer) {
        xTimerStart(g_system_health.watchdog_timer, 0);
    }
    system_state_transition(SYSTEM_STATE_SAFE);
}

void system_health_update(void) {
    if (xSemaphoreTake(g_system_health.state_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        g_system_health.uptime_seconds = (uint32_t)(esp_timer_get_time() / 1000000ULL);
        g_system_health.cpu_temperature = (float)esp_random() / (float)UINT32_MAX * 80.0f + 20.0f;
        watchdog_feed();
        xSemaphoreGive(g_system_health.state_mutex);
    }
}

void system_fault_report(fault_code_t fault) {
    if (xSemaphoreTake(g_system_health.state_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        if (g_system_health.fault_count < FAULT_COUNT) {
            g_system_health.fault_queue[g_system_health.fault_count++] = fault;
        }
        if (g_system_health.fault_count >= FAULT_COUNT / 2) {
            system_state_transition(SYSTEM_STATE_FAULT);
        }
        xSemaphoreGive(g_system_health.state_mutex);
    }
}

void system_state_transition(system_state_t new_state) {
    if (xSemaphoreTake(g_system_health.state_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        g_system_health.previous_state = g_system_health.current_state;
        g_system_health.current_state = new_state;
        ESP_LOGI(TAG, "State: %d -> %d", g_system_health.previous_state, g_system_health.current_state);
        xSemaphoreGive(g_system_health.state_mutex);
    }
}

bool system_self_test(void) {
    audio_block_t test_block;
    memset(&test_block, 0, sizeof(test_block));
    for (int i = 0; i < PROCESSING_BLOCK_SIZE; i++) {
        test_block.samples[i] = (int32_t)((esp_random() & 0xFFFFFF) - 0x800000);
    }
    int32_t output_a[PROCESSING_BLOCK_SIZE];
    int32_t output_b[PROCESSING_BLOCK_SIZE];
    int32_t output_c[PROCESSING_BLOCK_SIZE];
    system_tmr_execute(output_a, output_b, output_c, test_block.samples, PROCESSING_BLOCK_SIZE);
    for (uint32_t i = 0; i < PROCESSING_BLOCK_SIZE; i++) {
        if (output_a[i] != output_b[i] || output_b[i] != output_c[i]) {
            if (output_a[i] == output_b[i] || output_a[i] == output_c[i]) {
                continue;
            }
            system_fault_report(FAULT_TMR_MISMATCH);
            return false;
        }
    }
    return true;
}

void system_tmr_execute(int32_t *output_a, int32_t *output_b, int32_t *output_c, const int32_t *input, uint32_t length) {
    static lms_filter_t filter_a, filter_b, filter_c;
    static bool tmr_init = false;
    if (!tmr_init) {
        lms_filter_init(&filter_a, LMS_MU, LMS_LEAKAGE);
        lms_filter_init(&filter_b, LMS_MU, LMS_LEAKAGE);
        lms_filter_init(&filter_c, LMS_MU, LMS_LEAKAGE);
        tmr_init = true;
    }
    lms_filter_process(&filter_a, input, output_a, length);
    lms_filter_process(&filter_b, input, output_b, length);
    lms_filter_process(&filter_c, input, output_c, length);
}

void vTaskAudioCapturePrimary(void *pvParameters) {
    audio_block_t raw_block, filtered_block;
    voice_packet_t tx_packet;
    static lms_filter_t noise_filter;
    lms_filter_init(&noise_filter, LMS_MU, LMS_LEAKAGE);
    while (g_system_health.current_state == SYSTEM_STATE_ACTIVE || g_system_health.current_state == SYSTEM_STATE_STANDBY) {
        if (flight_i2s_read_block(&raw_block, PRIMARY_AUDIO_CHANNEL) != ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        if (raw_block.energy_level < 100.0f && raw_block.zero_crossing_rate < 0.01f) {
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }
        system_tmr_execute(filtered_block.samples, filtered_block.samples, filtered_block.samples, raw_block.samples, PROCESSING_BLOCK_SIZE);
        filtered_block.energy_level = raw_block.energy_level;
        filtered_block.timestamp = raw_block.timestamp;
        filtered_block.crc = compute_crc32((uint8_t *)filtered_block.samples, sizeof(filtered_block.samples));
        if (voice_packet_pack(&tx_packet, &filtered_block, 0xFF)) {
            radio_send_packet(&tx_packet, sizeof(voice_packet_t), 0xFF);
        }
        vTaskDelay(pdMS_TO_TICKS(2));
    }
    vTaskDelete(NULL);
}

void vTaskAudioCaptureRedundant(void *pvParameters) {
    audio_block_t raw_block, filtered_block;
    while (g_system_health.current_state == SYSTEM_STATE_ACTIVE || g_system_health.current_state == SYSTEM_STATE_FAULT) {
        if (flight_i2s_read_block(&raw_block, BACKUP_AUDIO_CHANNEL) != ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        int32_t temp_output[PROCESSING_BLOCK_SIZE];
        system_tmr_execute(temp_output, filtered_block.samples, filtered_block.samples, raw_block.samples, PROCESSING_BLOCK_SIZE);
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    vTaskDelete(NULL);
}

void vTaskAudioPlayback(void *pvParameters) {
    audio_block_t rx_block;
    while (1) {
        if (xQueueReceive(g_audio_rx_queue, &rx_block, portMAX_DELAY) == pdTRUE) {
            flight_i2s_write_block(&rx_block, PRIMARY_AUDIO_CHANNEL);
        }
    }
}

void vTaskHealthMonitor(void *pvParameters) {
    TickType_t xLastWakeTime = xTaskGetTickCount();
    while (1) {
        system_health_update();
        if (g_system_health.fault_count >= FAULT_COUNT) {
            system_state_transition(SYSTEM_STATE_EMERGENCY);
            flight_emergency_broadcast();
        }
        if (g_system_health.current_state == SYSTEM_STATE_FAULT && g_system_health.fault_count < FAULT_COUNT / 2) {
            system_state_transition(SYSTEM_STATE_ACTIVE);
        }
        if (system_self_test() == false) {
            flight_fault_recovery_handler(FAULT_TMR_MISMATCH);
        }
        vTaskDelayUntil(&xLastWakeTime, pdMS_TO_TICKS(HEALTH_CHECK_INTERVAL_MS));
    }
}

void vTaskRadioController(void *pvParameters) {
    heartbeat_packet_t heartbeat;
    TickType_t xLastWakeTime = xTaskGetTickCount();
    while (1) {
        packet_header_create(&heartbeat.header, PACKET_HEARTBEAT, 0xFF);
        heartbeat.magic_number = 0xDEADBEEF;
        heartbeat.crc32 = compute_crc32((uint8_t *)&heartbeat, sizeof(heartbeat) - sizeof(uint32_t));
        radio_send_packet(&heartbeat, sizeof(heartbeat), 0xFF);
        for (int i = 0; i < MAX_PEERS; i++) {
            if (g_peer_table[i].active) {
                g_peer_table[i].missed_heartbeats++;
                if (g_peer_table[i].missed_heartbeats > 3) {
                    g_peer_table[i].active = false;
                    system_fault_report(FAULT_RADIO_LINK_LOSS);
                }
            }
        }
        vTaskDelayUntil(&xLastWakeTime, pdMS_TO_TICKS(HEARTBEAT_INTERVAL_MS));
    }
}

void vTaskWatchdogService(void *pvParameters) {
    while (1) {
        watchdog_feed();
        vTaskDelay(pdMS_TO_TICKS((WATCHDOG_TIMEOUT_SEC * 1000) / 2));
    }
}

void flight_emergency_broadcast(void) {
    voice_packet_t emergency_packet;
    audio_block_t emergency_block;
    memset(&emergency_block, 0, sizeof(emergency_block));
    for (int i = 0; i < PROCESSING_BLOCK_SIZE; i++) {
        emergency_block.samples[i] = (int32_t)(sinf(2.0f * 3.14159f * 1000.0f * i / SAMPLE_RATE) * 83

8607.0f);
    }
    emergency_block.timestamp = (uint32_t)(esp_timer_get_time() & 0xFFFFFFFF);
    emergency_block.crc = compute_crc32((uint8_t *)emergency_block.samples, sizeof(emergency_block.samples));
    packet_header_create(&emergency_packet.header, PACKET_EMERGENCY, 0xFF);
    memcpy(emergency_packet.audio_data, emergency_block.samples, sizeof(emergency_block.samples));
    radio_send_packet(&emergency_packet, sizeof(voice_packet_t), 0xFF);
}

void flight_safe_mode_handler(void) {
    flight_i2s_deinit();
    vTaskDelay(pdMS_TO_TICKS(1000));
    flight_i2s_init();
    lms_filter_t safe_filter;
    lms_filter_init(&safe_filter, LMS_MU, LMS_LEAKAGE);
    system_state_transition(SYSTEM_STATE_STANDBY);
}

void flight_fault_recovery_handler(fault_code_t fault) {
    ESP_LOGE(TAG, "Recovering from fault: %d", fault);
    switch (fault) {
        case FAULT_I2S_TIMEOUT:
            flight_i2s_deinit();
            vTaskDelay(pdMS_TO_TICKS(500));
            flight_i2s_init();
            break;
        case FAULT_MEMORY_CORRUPTION:
            watchdog_force_reset();
            break;
        case FAULT_RADIO_LINK_LOSS:
            radio_init();
            break;
        case FAULT_TMR_MISMATCH:
            system_state_transition(SYSTEM_STATE_FAULT);
            break;
        default:
            flight_safe_mode_handler();
            break;
    }
    if (xSemaphoreTake(g_system_health.state_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        g_system_health.fault_count = (g_system_health.fault_count > 0) ? g_system_health.fault_count - 1 : 0;
        xSemaphoreGive(g_system_health.state_mutex);
    }
}