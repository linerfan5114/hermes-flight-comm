#ifndef FLIGHT_COMM_H
#define FLIGHT_COMM_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <math.h>
#include <sys/time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/timers.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_now.h"
#include "driver/i2s.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "esp_cpu.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "hal/wdt_hal.h"

#define TAG "FLIGHT_COMM"

#define I2S_NUM             I2S_NUM_0
#define I2S_BCK_PIN         GPIO_NUM_26
#define I2S_WS_PIN          GPIO_NUM_25
#define I2S_DATA_IN_PIN     GPIO_NUM_33
#define I2S_DATA_OUT_PIN    GPIO_NUM_32

#define SAMPLE_RATE         48000UL
#define SAMPLE_BITS         I2S_BITS_PER_SAMPLE_24BIT
#define CHANNEL_NUM         I2S_CHANNEL_STEREO
#define AUDIO_CHUNK_SIZE    512UL
#define DMA_BUF_COUNT       16UL
#define DMA_BUF_LEN         1024UL

#define PROCESSING_BLOCK_SIZE   256UL
#define FILTER_TAP_COUNT        64UL
#define LMS_MU                  0.01f
#define LMS_LEAKAGE             0.999f

#define WATCHDOG_TIMEOUT_SEC    2
#define HEALTH_CHECK_INTERVAL_MS 500
#define LINK_TIMEOUT_US         3000000ULL
#define HEARTBEAT_INTERVAL_MS   1000

#define PRIMARY_AUDIO_CHANNEL   0
#define BACKUP_AUDIO_CHANNEL    1

#define MAX_PEERS               6
#define MAX_CALLSIGN_LEN        16
#define ENCRYPTION_KEY_LEN      32
#define AUTH_TAG_LEN            16
#define NONCE_LEN               12
#define MAX_PACKET_SIZE         (AUDIO_CHUNK_SIZE * 3)

typedef enum {
    PACKET_VOICE = 0x01,
    PACKET_HEARTBEAT = 0x02,
    PACKET_TELEMETRY = 0x03,
    PACKET_EMERGENCY = 0xFF
} packet_type_t;

typedef enum {
    SYSTEM_STATE_INIT = 0,
    SYSTEM_STATE_SAFE = 1,
    SYSTEM_STATE_STANDBY = 2,
    SYSTEM_STATE_ACTIVE = 3,
    SYSTEM_STATE_EMERGENCY = 4,
    SYSTEM_STATE_FAULT = 5
} system_state_t;

typedef enum {
    FAULT_NONE = 0,
    FAULT_I2S_TIMEOUT = 1,
    FAULT_DMA_OVERRUN = 2,
    FAULT_RADIO_LINK_LOSS = 3,
    FAULT_MEMORY_CORRUPTION = 4,
    FAULT_WATCHDOG_RESET = 5,
    FAULT_TMR_MISMATCH = 6,
    FAULT_COUNT = 7
} fault_code_t;

typedef struct {
    uint8_t mac[6];
    char callsign[MAX_CALLSIGN_LEN];
    uint32_t last_heartbeat;
    uint32_t missed_heartbeats;
    int8_t rssi;
    bool active;
} __attribute__((packed)) peer_entry_t;

typedef struct {
    uint8_t prefix[4];
    packet_type_t type;
    uint16_t sequence;
    uint32_t timestamp;
    uint8_t source_id;
    uint8_t destination_id;
    uint16_t payload_length;
    uint32_t crc32_header;
} __attribute__((packed)) packet_header_t;

typedef struct {
    packet_header_t header;
    int32_t audio_data[AUDIO_CHUNK_SIZE];
    uint8_t nonce[NONCE_LEN];
    uint8_t auth_tag[AUTH_TAG_LEN];
    uint32_t crc32_payload;
} __attribute__((packed)) voice_packet_t;

typedef struct {
    packet_header_t header;
    system_state_t state;
    fault_code_t active_faults[FAULT_COUNT];
    float cpu_temp;
    float battery_voltage;
    uint32_t uptime_seconds;
    uint32_t crc32;
} __attribute__((packed)) telemetry_packet_t;

typedef struct {
    packet_header_t header;
    uint32_t magic_number;
    uint32_t crc32;
} __attribute__((packed)) heartbeat_packet_t;

typedef struct __attribute__((aligned(16))) {
    float weights[FILTER_TAP_COUNT];
    float buffer[FILTER_TAP_COUNT];
    float mu;
    float leakage;
    uint32_t buffer_index;
    bool initialized;
    uint32_t crc_checksum;
} lms_filter_t;

typedef struct __attribute__((aligned(16))) {
    int32_t samples[PROCESSING_BLOCK_SIZE];
    float energy_level;
    float zero_crossing_rate;
    uint32_t timestamp;
    uint32_t crc;
} audio_block_t;

typedef struct __attribute__((aligned(16))) {
    system_state_t current_state;
    system_state_t previous_state;
    fault_code_t fault_queue[FAULT_COUNT];
    uint8_t fault_count;
    uint32_t uptime_seconds;
    uint64_t total_packets_sent;
    uint64_t total_packets_received;
    uint32_t crc_errors;
    uint32_t watchdog_resets;
    float cpu_temperature;
    SemaphoreHandle_t state_mutex;
    TimerHandle_t watchdog_timer;
} system_health_t;

extern system_health_t g_system_health;
extern peer_entry_t g_peer_table[MAX_PEERS];
extern QueueHandle_t g_audio_tx_queue;
extern QueueHandle_t g_audio_rx_queue;
extern QueueHandle_t g_command_queue;
extern EventGroupHandle_t g_flight_events;

void flight_i2s_init(void);
void flight_i2s_deinit(void);
esp_err_t flight_i2s_read_block(audio_block_t *block, uint8_t channel);
esp_err_t flight_i2s_write_block(audio_block_t *block, uint8_t channel);

void lms_filter_init(lms_filter_t *filter, float mu, float leakage);
void lms_filter_process(lms_filter_t *filter, const int32_t *input, int32_t *output, uint32_t length);
void lms_filter_verify(lms_filter_t *filter);

bool packet_header_create(packet_header_t *header, packet_type_t type, uint8_t destination);
bool packet_header_validate(const packet_header_t *header);
bool voice_packet_pack(voice_packet_t *packet, const audio_block_t *block, uint8_t destination);
bool voice_packet_unpack(const voice_packet_t *packet, audio_block_t *block);

void encryption_init(const uint8_t *key, uint32_t key_len);
void encryption_encrypt_block(uint8_t *data, uint32_t length, uint8_t *nonce, uint8_t *tag);
bool encryption_decrypt_block(uint8_t *data, uint32_t length, uint8_t *nonce, uint8_t *tag);

void radio_init(void);
void radio_send_packet(const void *packet, uint32_t length, uint8_t destination);
void radio_receive_callback(const uint8_t *mac, const uint8_t *data, uint32_t length);

void watchdog_init(uint32_t timeout_seconds);
void watchdog_feed(void);
void watchdog_force_reset(void);

void system_health_init(void);
void system_health_update(void);
void system_fault_report(fault_code_t fault);
void system_state_transition(system_state_t new_state);
bool system_self_test(void);
void system_tmr_execute(int32_t *output_a, int32_t *output_b, int32_t *output_c, const int32_t *input, uint32_t length);

void vTaskAudioCapturePrimary(void *pvParameters);
void vTaskAudioCaptureRedundant(void *pvParameters);
void vTaskAudioPlayback(void *pvParameters);
void vTaskHealthMonitor(void *pvParameters);
void vTaskRadioController(void *pvParameters);
void vTaskWatchdogService(void *pvParameters);

void flight_emergency_broadcast(void);
void flight_safe_mode_handler(void);
void flight_fault_recovery_handler(fault_code_t fault);

#endif