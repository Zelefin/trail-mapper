#include <stdbool.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "driver/gpio.h"
#include "driver/sdspi_host.h"
#include "driver/spi_common.h"
#include "driver/spi_master.h"
#include "driver/uart.h"
#include "esp_err.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdmmc_cmd.h"

static const char *TAG = "trail-mapper";

#define RED_LED_GPIO GPIO_NUM_13
#define YELLOW_LED_GPIO GPIO_NUM_14
#define GREEN_LED_GPIO GPIO_NUM_21
#define BUZZER_GPIO GPIO_NUM_33
#define RECORD_BUTTON_GPIO GPIO_NUM_32

#define SD_MOUNT_POINT "/sdcard"
#define BLACKBOX_LOG_PATH SD_MOUNT_POINT "/blackbox.log"
#define SD_MISO_GPIO GPIO_NUM_19
#define SD_MOSI_GPIO GPIO_NUM_23
#define SD_CLK_GPIO GPIO_NUM_18
#define SD_CS_GPIO GPIO_NUM_22

#define TFT_CS_GPIO GPIO_NUM_27
#define TFT_DC_GPIO GPIO_NUM_16
#define TFT_RST_GPIO GPIO_NUM_17
#define TFT_WIDTH 160
#define TFT_HEIGHT 80
#define TFT_X_OFFSET 0
#define TFT_Y_OFFSET 24

#define GPS_UART_NUM UART_NUM_2
#define GPS_TX_GPIO GPIO_NUM_26
#define GPS_RX_GPIO GPIO_NUM_25
#define GPS_BAUD_RATE 9600

#define UART_BUF_SIZE 1024
#define LINE_BUF_SIZE 256
#define GPS_TEST_SECONDS 60

#define ST77XX_SWRESET 0x01
#define ST77XX_NOP 0x00
#define ST77XX_SLPOUT 0x11
#define ST77XX_NORON 0x13
#define ST77XX_INVOFF 0x20
#define ST77XX_INVON 0x21
#define ST77XX_CASET 0x2A
#define ST77XX_RASET 0x2B
#define ST77XX_RAMWR 0x2C
#define ST77XX_MADCTL 0x36
#define ST77XX_COLMOD 0x3A
#define ST77XX_DISPON 0x29

#define RGB565_BLACK 0x0000
#define RGB565_BLUE 0x001F
#define RGB565_GREEN 0x07E0
#define RGB565_RED 0xF800
#define RGB565_YELLOW 0xFFE0
#define RGB565_WHITE 0xFFFF

typedef struct {
    char utc_time[16];
    double latitude;
    double longitude;
    int fix_quality;
    int satellites;
    double hdop;
} gps_gga_fix_t;

typedef struct {
    sdmmc_card_t *card;
    spi_host_device_t host_slot;
} sd_mount_t;

typedef struct {
    int nmea_lines;
    int gga_sentences;
    int valid_fixes;
    gps_gga_fix_t last_fix;
    bool has_fix;
} gps_sd_result_t;

static void set_leds(int red, int yellow, int green)
{
    gpio_set_level(RED_LED_GPIO, red);
    gpio_set_level(YELLOW_LED_GPIO, yellow);
    gpio_set_level(GREEN_LED_GPIO, green);
}

static void beep(int count)
{
    for (int i = 0; i < count; i++) {
        gpio_set_level(BUZZER_GPIO, 1);
        vTaskDelay(pdMS_TO_TICKS(80));
        gpio_set_level(BUZZER_GPIO, 0);
        vTaskDelay(pdMS_TO_TICKS(120));
    }
}

static void blackbox_log(FILE *file, const char *level, const char *format, ...)
{
    if (file == NULL) {
        return;
    }

    int64_t uptime_ms = esp_timer_get_time() / 1000;
    fprintf(file, "%lld,%s,", uptime_ms, level);

    va_list args;
    va_start(args, format);
    vfprintf(file, format, args);
    va_end(args);

    fputc('\n', file);
    fflush(file);
}

static void init_status_gpio(void)
{
    gpio_config_t outputs = {
        .pin_bit_mask = (1ULL << RED_LED_GPIO) |
                        (1ULL << YELLOW_LED_GPIO) |
                        (1ULL << GREEN_LED_GPIO) |
                        (1ULL << BUZZER_GPIO),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&outputs));

    set_leds(0, 1, 0);
    gpio_set_level(BUZZER_GPIO, 0);
}

static void init_record_button(void)
{
    gpio_config_t input = {
        .pin_bit_mask = 1ULL << RECORD_BUTTON_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&input));
}

static bool record_button_is_down(void)
{
    return gpio_get_level(RECORD_BUTTON_GPIO) == 0;
}

static bool record_button_pressed_event(void)
{
    if (!record_button_is_down()) {
        return false;
    }

    vTaskDelay(pdMS_TO_TICKS(35));
    if (!record_button_is_down()) {
        return false;
    }

    while (record_button_is_down()) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    vTaskDelay(pdMS_TO_TICKS(50));
    return true;
}

static void wait_for_record_button(void)
{
    while (!record_button_pressed_event()) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

static void hold_tft_deselected(void)
{
    gpio_config_t tft_idle = {
        .pin_bit_mask = (1ULL << TFT_CS_GPIO) | (1ULL << TFT_DC_GPIO),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&tft_idle));
    gpio_set_level(TFT_DC_GPIO, 0);
    gpio_set_level(TFT_CS_GPIO, 1);
}

static void lock_tft_deselected(void)
{
    hold_tft_deselected();
    esp_err_t ret = gpio_hold_en(TFT_CS_GPIO);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Could not enable TFT CS hold: %s", esp_err_to_name(ret));
    }
}

static esp_err_t tft_send(spi_device_handle_t spi, int dc_level, const uint8_t *data, size_t len)
{
    if (len == 0) {
        return ESP_OK;
    }

    gpio_set_level(TFT_DC_GPIO, dc_level);
    spi_transaction_t transaction = {
        .length = len * 8,
        .tx_buffer = data,
    };

    return spi_device_polling_transmit(spi, &transaction);
}

static esp_err_t tft_cmd(spi_device_handle_t spi, uint8_t cmd)
{
    return tft_send(spi, 0, &cmd, sizeof(cmd));
}

static esp_err_t tft_data(spi_device_handle_t spi, const uint8_t *data, size_t len)
{
    return tft_send(spi, 1, data, len);
}

static esp_err_t tft_cmd_data(spi_device_handle_t spi, uint8_t cmd, const uint8_t *data, size_t len)
{
    esp_err_t ret = tft_cmd(spi, cmd);
    if (ret != ESP_OK) {
        return ret;
    }

    return tft_data(spi, data, len);
}

static void tft_reset(void)
{
    gpio_set_level(TFT_RST_GPIO, 0);
    vTaskDelay(pdMS_TO_TICKS(50));
    gpio_set_level(TFT_RST_GPIO, 1);
    vTaskDelay(pdMS_TO_TICKS(120));
}

static esp_err_t tft_init_panel(spi_device_handle_t spi)
{
    tft_reset();

    esp_err_t ret = tft_cmd(spi, ST77XX_SWRESET);
    if (ret != ESP_OK) {
        return ret;
    }
    vTaskDelay(pdMS_TO_TICKS(150));

    ret = tft_cmd(spi, ST77XX_SLPOUT);
    if (ret != ESP_OK) {
        return ret;
    }
    vTaskDelay(pdMS_TO_TICKS(120));

    uint8_t color_mode = 0x05; // 16-bit RGB565
    ret = tft_cmd_data(spi, ST77XX_COLMOD, &color_mode, sizeof(color_mode));
    if (ret != ESP_OK) {
        return ret;
    }
    vTaskDelay(pdMS_TO_TICKS(10));

    uint8_t madctl = 0x68; // landscape orientation, BGR color order
    ret = tft_cmd_data(spi, ST77XX_MADCTL, &madctl, sizeof(madctl));
    if (ret != ESP_OK) {
        return ret;
    }

    ret = tft_cmd(spi, ST77XX_INVOFF);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = tft_cmd(spi, ST77XX_NORON);
    if (ret != ESP_OK) {
        return ret;
    }
    vTaskDelay(pdMS_TO_TICKS(10));

    ret = tft_cmd(spi, ST77XX_DISPON);
    if (ret != ESP_OK) {
        return ret;
    }
    vTaskDelay(pdMS_TO_TICKS(120));

    return ESP_OK;
}

static esp_err_t tft_set_window(spi_device_handle_t spi, int x, int y, int width, int height)
{
    uint16_t x0 = x + TFT_X_OFFSET;
    uint16_t x1 = x + width - 1 + TFT_X_OFFSET;
    uint16_t y0 = y + TFT_Y_OFFSET;
    uint16_t y1 = y + height - 1 + TFT_Y_OFFSET;
    uint8_t caset[] = {
        (uint8_t)(x0 >> 8), (uint8_t)x0,
        (uint8_t)(x1 >> 8), (uint8_t)x1,
    };
    uint8_t raset[] = {
        (uint8_t)(y0 >> 8), (uint8_t)y0,
        (uint8_t)(y1 >> 8), (uint8_t)y1,
    };

    esp_err_t ret = tft_cmd_data(spi, ST77XX_CASET, caset, sizeof(caset));
    if (ret != ESP_OK) {
        return ret;
    }

    ret = tft_cmd_data(spi, ST77XX_RASET, raset, sizeof(raset));
    if (ret != ESP_OK) {
        return ret;
    }

    return tft_cmd(spi, ST77XX_RAMWR);
}

static esp_err_t tft_fill_rect(spi_device_handle_t spi, int x, int y, int width, int height, uint16_t color)
{
    esp_err_t ret = tft_set_window(spi, x, y, width, height);
    if (ret != ESP_OK) {
        return ret;
    }

    uint8_t line[TFT_WIDTH * 2];
    for (int i = 0; i < width; i++) {
        line[i * 2] = (uint8_t)(color >> 8);
        line[i * 2 + 1] = (uint8_t)color;
    }

    for (int row = 0; row < height; row++) {
        ret = tft_data(spi, line, width * 2);
        if (ret != ESP_OK) {
            return ret;
        }
    }

    return ESP_OK;
}

static const uint8_t *font5x7(char c)
{
    static const uint8_t blank[5] = {0x00, 0x00, 0x00, 0x00, 0x00};
    static const uint8_t colon[5] = {0x00, 0x36, 0x36, 0x00, 0x00};
    static const uint8_t plus[5] = {0x08, 0x08, 0x3E, 0x08, 0x08};
    static const uint8_t minus[5] = {0x08, 0x08, 0x08, 0x08, 0x08};
    static const uint8_t dot[5] = {0x00, 0x60, 0x60, 0x00, 0x00};
    static const uint8_t slash[5] = {0x20, 0x10, 0x08, 0x04, 0x02};
    static const uint8_t digits[10][5] = {
        {0x3E, 0x51, 0x49, 0x45, 0x3E},
        {0x00, 0x42, 0x7F, 0x40, 0x00},
        {0x42, 0x61, 0x51, 0x49, 0x46},
        {0x21, 0x41, 0x45, 0x4B, 0x31},
        {0x18, 0x14, 0x12, 0x7F, 0x10},
        {0x27, 0x45, 0x45, 0x45, 0x39},
        {0x3C, 0x4A, 0x49, 0x49, 0x30},
        {0x01, 0x71, 0x09, 0x05, 0x03},
        {0x36, 0x49, 0x49, 0x49, 0x36},
        {0x06, 0x49, 0x49, 0x29, 0x1E},
    };
    static const uint8_t letters[26][5] = {
        {0x7E, 0x11, 0x11, 0x11, 0x7E},
        {0x7F, 0x49, 0x49, 0x49, 0x36},
        {0x3E, 0x41, 0x41, 0x41, 0x22},
        {0x7F, 0x41, 0x41, 0x22, 0x1C},
        {0x7F, 0x49, 0x49, 0x49, 0x41},
        {0x7F, 0x09, 0x09, 0x09, 0x01},
        {0x3E, 0x41, 0x49, 0x49, 0x7A},
        {0x7F, 0x08, 0x08, 0x08, 0x7F},
        {0x00, 0x41, 0x7F, 0x41, 0x00},
        {0x20, 0x40, 0x41, 0x3F, 0x01},
        {0x7F, 0x08, 0x14, 0x22, 0x41},
        {0x7F, 0x40, 0x40, 0x40, 0x40},
        {0x7F, 0x02, 0x0C, 0x02, 0x7F},
        {0x7F, 0x04, 0x08, 0x10, 0x7F},
        {0x3E, 0x41, 0x41, 0x41, 0x3E},
        {0x7F, 0x09, 0x09, 0x09, 0x06},
        {0x3E, 0x41, 0x51, 0x21, 0x5E},
        {0x7F, 0x09, 0x19, 0x29, 0x46},
        {0x46, 0x49, 0x49, 0x49, 0x31},
        {0x01, 0x01, 0x7F, 0x01, 0x01},
        {0x3F, 0x40, 0x40, 0x40, 0x3F},
        {0x1F, 0x20, 0x40, 0x20, 0x1F},
        {0x3F, 0x40, 0x38, 0x40, 0x3F},
        {0x63, 0x14, 0x08, 0x14, 0x63},
        {0x07, 0x08, 0x70, 0x08, 0x07},
        {0x61, 0x51, 0x49, 0x45, 0x43},
    };

    if (c >= 'a' && c <= 'z') {
        c -= 32;
    }
    if (c >= '0' && c <= '9') {
        return digits[c - '0'];
    }
    if (c >= 'A' && c <= 'Z') {
        return letters[c - 'A'];
    }
    if (c == ':') {
        return colon;
    }
    if (c == '+') {
        return plus;
    }
    if (c == '-') {
        return minus;
    }
    if (c == '.') {
        return dot;
    }
    if (c == '/') {
        return slash;
    }
    return blank;
}

static esp_err_t tft_draw_char(spi_device_handle_t spi, int x, int y, char c, uint16_t color, int scale)
{
    const uint8_t *glyph = font5x7(c);
    for (int col = 0; col < 5; col++) {
        for (int row = 0; row < 7; row++) {
            if ((glyph[col] & (1 << row)) != 0) {
                esp_err_t ret = tft_fill_rect(spi,
                                              x + col * scale,
                                              y + row * scale,
                                              scale,
                                              scale,
                                              color);
                if (ret != ESP_OK) {
                    return ret;
                }
            }
        }
    }

    return ESP_OK;
}

static esp_err_t tft_draw_text(spi_device_handle_t spi, int x, int y, const char *text, uint16_t color, int scale)
{
    while (*text != '\0') {
        esp_err_t ret = tft_draw_char(spi, x, y, *text, color, scale);
        if (ret != ESP_OK) {
            return ret;
        }
        x += 6 * scale;
        text++;
    }

    return ESP_OK;
}

static esp_err_t tft_draw_status_screen(spi_device_handle_t spi, const char *title, const char *line1, const char *line2, const char *line3)
{
    esp_err_t ret = tft_fill_rect(spi, 0, 0, TFT_WIDTH, TFT_HEIGHT, RGB565_BLACK);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = tft_fill_rect(spi, 0, 0, TFT_WIDTH, 14, RGB565_BLUE);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = tft_draw_text(spi, 4, 3, title, RGB565_WHITE, 1);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = tft_draw_text(spi, 4, 22, line1, RGB565_GREEN, 1);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = tft_draw_text(spi, 4, 40, line2, RGB565_YELLOW, 1);
    if (ret != ESP_OK) {
        return ret;
    }

    return tft_draw_text(spi, 4, 58, line3, RGB565_WHITE, 1);
}

static esp_err_t tft_draw_smoke_pattern(spi_device_handle_t spi)
{
    esp_err_t ret = tft_fill_rect(spi, 0, 0, TFT_WIDTH, TFT_HEIGHT, RGB565_BLACK);
    if (ret != ESP_OK) {
        return ret;
    }
    vTaskDelay(pdMS_TO_TICKS(300));

    ret = tft_fill_rect(spi, 0, 0, TFT_WIDTH, TFT_HEIGHT, RGB565_RED);
    if (ret != ESP_OK) {
        return ret;
    }
    vTaskDelay(pdMS_TO_TICKS(300));

    ret = tft_fill_rect(spi, 0, 0, TFT_WIDTH, TFT_HEIGHT, RGB565_GREEN);
    if (ret != ESP_OK) {
        return ret;
    }
    vTaskDelay(pdMS_TO_TICKS(300));

    ret = tft_fill_rect(spi, 0, 0, TFT_WIDTH, TFT_HEIGHT, RGB565_BLUE);
    if (ret != ESP_OK) {
        return ret;
    }
    vTaskDelay(pdMS_TO_TICKS(300));

    ret = tft_fill_rect(spi, 0, 0, TFT_WIDTH / 2, TFT_HEIGHT / 2, RGB565_RED);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = tft_fill_rect(spi, TFT_WIDTH / 2, 0, TFT_WIDTH / 2, TFT_HEIGHT / 2, RGB565_GREEN);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = tft_fill_rect(spi, 0, TFT_HEIGHT / 2, TFT_WIDTH / 2, TFT_HEIGHT / 2, RGB565_BLUE);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = tft_fill_rect(spi, TFT_WIDTH / 2, TFT_HEIGHT / 2, TFT_WIDTH / 2, TFT_HEIGHT / 2, RGB565_YELLOW);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = tft_fill_rect(spi, 0, 0, TFT_WIDTH, 3, RGB565_WHITE);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = tft_fill_rect(spi, 0, TFT_HEIGHT - 3, TFT_WIDTH, 3, RGB565_WHITE);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = tft_fill_rect(spi, 0, 0, 3, TFT_HEIGHT, RGB565_WHITE);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = tft_fill_rect(spi, TFT_WIDTH - 3, 0, 3, TFT_HEIGHT, RGB565_WHITE);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = tft_fill_rect(spi, 0, (TFT_HEIGHT / 2) - 2, TFT_WIDTH, 4, RGB565_WHITE);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = tft_fill_rect(spi, (TFT_WIDTH / 2) - 2, 0, 4, TFT_HEIGHT, RGB565_WHITE);
    if (ret != ESP_OK) {
        return ret;
    }
    vTaskDelay(pdMS_TO_TICKS(700));

    return tft_draw_status_screen(spi, "TRAIL MAPPER", "TFT OK", "GPS SD TEST", "WAITING");
}

static esp_err_t __attribute__((unused)) run_tft_smoke_test(void)
{
    gpio_hold_dis(TFT_CS_GPIO);

    gpio_config_t tft_outputs = {
        .pin_bit_mask = (1ULL << TFT_DC_GPIO) |
                        (1ULL << TFT_RST_GPIO) |
                        (1ULL << TFT_CS_GPIO) |
                        (1ULL << SD_CS_GPIO),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&tft_outputs));
    gpio_set_level(SD_CS_GPIO, 1);
    gpio_set_level(TFT_CS_GPIO, 1);
    gpio_set_level(TFT_DC_GPIO, 0);
    gpio_set_level(TFT_RST_GPIO, 1);

    spi_bus_config_t bus_cfg = {
        .mosi_io_num = SD_MOSI_GPIO,
        .miso_io_num = SD_MISO_GPIO,
        .sclk_io_num = SD_CLK_GPIO,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = TFT_WIDTH * TFT_HEIGHT * 2,
    };

    ESP_LOGI(TAG, "Initializing TFT SPI bus: CLK=%d MOSI=%d CS=%d DC=%d RST=%d",
             SD_CLK_GPIO, SD_MOSI_GPIO, TFT_CS_GPIO, TFT_DC_GPIO, TFT_RST_GPIO);
    esp_err_t ret = spi_bus_initialize(SPI2_HOST, &bus_cfg, SPI_DMA_CH_AUTO);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize TFT SPI bus: %s", esp_err_to_name(ret));
        return ret;
    }

    spi_device_interface_config_t dev_cfg = {
        .clock_speed_hz = 10 * 1000 * 1000,
        .mode = 0,
        .spics_io_num = TFT_CS_GPIO,
        .queue_size = 1,
    };

    spi_device_handle_t spi = NULL;
    ret = spi_bus_add_device(SPI2_HOST, &dev_cfg, &spi);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add TFT SPI device: %s", esp_err_to_name(ret));
        spi_bus_free(SPI2_HOST);
        return ret;
    }

    ret = tft_init_panel(spi);
    if (ret == ESP_OK) {
        ret = tft_draw_smoke_pattern(spi);
    }
    if (ret == ESP_OK) {
        ret = tft_cmd(spi, ST77XX_NOP);
    }

    spi_bus_remove_device(spi);
    lock_tft_deselected();
    spi_bus_free(SPI2_HOST);

    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "TFT smoke test drew color pattern");
    } else {
        ESP_LOGE(TAG, "TFT smoke test failed: %s", esp_err_to_name(ret));
    }

    return ret;
}

static esp_err_t run_tft_status_screen(const char *title, const char *line1, const char *line2, const char *line3)
{
    gpio_hold_dis(TFT_CS_GPIO);

    gpio_config_t tft_outputs = {
        .pin_bit_mask = (1ULL << TFT_DC_GPIO) |
                        (1ULL << TFT_RST_GPIO) |
                        (1ULL << TFT_CS_GPIO) |
                        (1ULL << SD_CS_GPIO),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&tft_outputs));
    gpio_set_level(SD_CS_GPIO, 1);
    gpio_set_level(TFT_CS_GPIO, 1);
    gpio_set_level(TFT_DC_GPIO, 0);
    gpio_set_level(TFT_RST_GPIO, 1);

    spi_bus_config_t bus_cfg = {
        .mosi_io_num = SD_MOSI_GPIO,
        .miso_io_num = SD_MISO_GPIO,
        .sclk_io_num = SD_CLK_GPIO,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = TFT_WIDTH * TFT_HEIGHT * 2,
    };

    esp_err_t ret = spi_bus_initialize(SPI2_HOST, &bus_cfg, SPI_DMA_CH_AUTO);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize TFT SPI bus: %s", esp_err_to_name(ret));
        return ret;
    }

    spi_device_interface_config_t dev_cfg = {
        .clock_speed_hz = 10 * 1000 * 1000,
        .mode = 0,
        .spics_io_num = TFT_CS_GPIO,
        .queue_size = 1,
    };

    spi_device_handle_t spi = NULL;
    ret = spi_bus_add_device(SPI2_HOST, &dev_cfg, &spi);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add TFT SPI device: %s", esp_err_to_name(ret));
        spi_bus_free(SPI2_HOST);
        return ret;
    }

    ret = tft_init_panel(spi);
    if (ret == ESP_OK) {
        ret = tft_draw_status_screen(spi, title, line1, line2, line3);
    }
    if (ret == ESP_OK) {
        ret = tft_cmd(spi, ST77XX_NOP);
    }

    spi_bus_remove_device(spi);
    lock_tft_deselected();
    spi_bus_free(SPI2_HOST);
    return ret;
}

static void log_sd_idle_levels(void)
{
    gpio_set_direction(SD_MISO_GPIO, GPIO_MODE_INPUT);
    gpio_set_direction(SD_MOSI_GPIO, GPIO_MODE_INPUT);
    gpio_set_direction(SD_CLK_GPIO, GPIO_MODE_INPUT);
    gpio_set_direction(SD_CS_GPIO, GPIO_MODE_INPUT);

    ESP_LOGI(TAG, "SD idle levels: CLK=%d MOSI=%d MISO=%d CS=%d",
             gpio_get_level(SD_CLK_GPIO),
             gpio_get_level(SD_MOSI_GPIO),
             gpio_get_level(SD_MISO_GPIO),
             gpio_get_level(SD_CS_GPIO));
}

static esp_err_t mount_sd_card(sd_mount_t *mount)
{
    lock_tft_deselected();
    gpio_set_pull_mode(SD_MISO_GPIO, GPIO_PULLUP_ONLY);
    gpio_set_pull_mode(SD_MOSI_GPIO, GPIO_PULLUP_ONLY);
    gpio_set_pull_mode(SD_CLK_GPIO, GPIO_PULLUP_ONLY);
    gpio_set_pull_mode(SD_CS_GPIO, GPIO_PULLUP_ONLY);
    log_sd_idle_levels();

    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 4,
        .allocation_unit_size = 16 * 1024,
    };

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SPI2_HOST;
    host.max_freq_khz = 400;

    spi_bus_config_t bus_cfg = {
        .mosi_io_num = SD_MOSI_GPIO,
        .miso_io_num = SD_MISO_GPIO,
        .sclk_io_num = SD_CLK_GPIO,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 4000,
    };

    ESP_LOGI(TAG, "Initializing SD SPI bus: CLK=%d MOSI=%d MISO=%d CS=%d",
             SD_CLK_GPIO, SD_MOSI_GPIO, SD_MISO_GPIO, SD_CS_GPIO);
    esp_err_t ret = spi_bus_initialize(host.slot, &bus_cfg, SPI_DMA_CH_AUTO);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize SPI bus: %s", esp_err_to_name(ret));
        return ret;
    }

    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_config.gpio_cs = SD_CS_GPIO;
    slot_config.host_id = host.slot;
    slot_config.wait_for_miso = 100;

    sdmmc_card_t *card = NULL;
    ESP_LOGI(TAG, "Mounting SD card filesystem");
    ret = esp_vfs_fat_sdspi_mount(SD_MOUNT_POINT, &host, &slot_config, &mount_config, &card);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to mount SD card: %s", esp_err_to_name(ret));
        spi_bus_free(host.slot);
        return ret;
    }

    sdmmc_card_print_info(stdout, card);
    mount->card = card;
    mount->host_slot = host.slot;
    return ESP_OK;
}

static void unmount_sd_card(sd_mount_t *mount)
{
    if (mount->card != NULL) {
        esp_vfs_fat_sdcard_unmount(SD_MOUNT_POINT, mount->card);
        mount->card = NULL;
    }
    spi_bus_free(mount->host_slot);
    ESP_LOGI(TAG, "SD card unmounted");
}

static void gps_uart_init(void)
{
    uart_config_t uart_config = {
        .baud_rate = GPS_BAUD_RATE,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    ESP_ERROR_CHECK(uart_driver_install(GPS_UART_NUM, UART_BUF_SIZE * 2, 0, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(GPS_UART_NUM, &uart_config));
    ESP_ERROR_CHECK(uart_set_pin(GPS_UART_NUM,
                                 GPS_TX_GPIO,
                                 GPS_RX_GPIO,
                                 UART_PIN_NO_CHANGE,
                                 UART_PIN_NO_CHANGE));
    ESP_LOGI(TAG, "GPS UART ready: UART2 baud=%d TX=%d RX=%d",
             GPS_BAUD_RATE, GPS_TX_GPIO, GPS_RX_GPIO);
}

static double nmea_to_decimal(const char *coord, char hemisphere)
{
    if (coord == NULL || coord[0] == '\0') {
        return 0.0;
    }

    double raw = atof(coord);
    int degrees = (int)(raw / 100.0);
    double minutes = raw - (degrees * 100.0);
    double decimal = degrees + (minutes / 60.0);

    if (hemisphere == 'S' || hemisphere == 'W') {
        decimal = -decimal;
    }

    return decimal;
}

static bool parse_gga(const char *line, gps_gga_fix_t *fix)
{
    if (strncmp(line, "$GPGGA", 6) != 0 && strncmp(line, "$GNGGA", 6) != 0) {
        return false;
    }

    char copy[LINE_BUF_SIZE];
    strncpy(copy, line, sizeof(copy) - 1);
    copy[sizeof(copy) - 1] = '\0';

    char *fields[16] = {0};
    int count = 0;
    char *saveptr = NULL;
    char *token = strtok_r(copy, ",", &saveptr);
    while (token != NULL && count < 16) {
        fields[count++] = token;
        token = strtok_r(NULL, ",", &saveptr);
    }

    if (count < 9 || fields[6] == NULL || fields[6][0] == '\0') {
        return false;
    }

    int fix_quality = atoi(fields[6]);
    if (fix_quality <= 0) {
        ESP_LOGI(TAG, "GGA received but no fix yet");
        return false;
    }

    memset(fix, 0, sizeof(*fix));
    strncpy(fix->utc_time, fields[1] != NULL ? fields[1] : "", sizeof(fix->utc_time) - 1);
    fix->latitude = nmea_to_decimal(fields[2], fields[3] != NULL ? fields[3][0] : '\0');
    fix->longitude = nmea_to_decimal(fields[4], fields[5] != NULL ? fields[5][0] : '\0');
    fix->fix_quality = fix_quality;
    fix->satellites = fields[7] != NULL ? atoi(fields[7]) : 0;
    fix->hdop = fields[8] != NULL ? atof(fields[8]) : 0.0;
    return true;
}

static esp_err_t __attribute__((unused)) run_gps_sd_test(gps_sd_result_t *result)
{
    memset(result, 0, sizeof(*result));

    sd_mount_t sd = {0};
    esp_err_t ret = mount_sd_card(&sd);
    if (ret != ESP_OK) {
        return ret;
    }

    FILE *nmea_file = fopen(SD_MOUNT_POINT "/gps_test.nmea", "w");
    FILE *csv_file = fopen(SD_MOUNT_POINT "/gps_fixes.csv", "w");
    if (nmea_file == NULL || csv_file == NULL) {
        ESP_LOGE(TAG, "Failed to open GPS log files on SD card");
        if (nmea_file != NULL) {
            fclose(nmea_file);
        }
        if (csv_file != NULL) {
            fclose(csv_file);
        }
        unmount_sd_card(&sd);
        return ESP_FAIL;
    }

    fprintf(csv_file, "utc_time,latitude,longitude,satellites,hdop,fix_quality\n");
    gps_uart_init();

    uint8_t *data = malloc(UART_BUF_SIZE);
    if (data == NULL) {
        ESP_LOGE(TAG, "Failed to allocate GPS UART buffer");
        fclose(nmea_file);
        fclose(csv_file);
        unmount_sd_card(&sd);
        return ESP_ERR_NO_MEM;
    }

    set_leds(0, 0, 1);
    beep(1);
    ESP_LOGI(TAG, "Recording GPS data to SD for %d seconds", GPS_TEST_SECONDS);

    char line[LINE_BUF_SIZE];
    int line_pos = 0;
    int nmea_lines = 0;
    int valid_fixes = 0;
    int gga_sentences = 0;
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(GPS_TEST_SECONDS * 1000);

    while (xTaskGetTickCount() < deadline) {
        int len = uart_read_bytes(GPS_UART_NUM, data, UART_BUF_SIZE, pdMS_TO_TICKS(1000));
        if (len <= 0) {
            ESP_LOGW(TAG, "No GPS bytes received in the last second");
            continue;
        }

        for (int i = 0; i < len; i++) {
            char c = (char)data[i];
            if (c == '\n') {
                line[line_pos] = '\0';
                line_pos = 0;

                if (line[0] != '$') {
                    continue;
                }

                fprintf(nmea_file, "%s\n", line);
                nmea_lines++;

                gps_gga_fix_t fix;
                if (strncmp(line, "$GPGGA", 6) == 0 || strncmp(line, "$GNGGA", 6) == 0) {
                    gga_sentences++;
                    if (parse_gga(line, &fix)) {
                        fprintf(csv_file, "%s,%.6f,%.6f,%d,%.2f,%d\n",
                                fix.utc_time,
                                fix.latitude,
                                fix.longitude,
                                fix.satellites,
                                fix.hdop,
                                fix.fix_quality);
                        valid_fixes++;
                        result->last_fix = fix;
                        result->has_fix = true;
                        ESP_LOGI(TAG, "Fix %d: lat=%.6f lon=%.6f sats=%d hdop=%.2f",
                                 valid_fixes, fix.latitude, fix.longitude, fix.satellites, fix.hdop);
                    }
                }

                if ((nmea_lines % 10) == 0) {
                    fflush(nmea_file);
                    fflush(csv_file);
                    ESP_LOGI(TAG, "Progress: NMEA lines=%d GGA=%d valid fixes=%d",
                             nmea_lines, gga_sentences, valid_fixes);
                }
            } else if (c != '\r') {
                if (line_pos < LINE_BUF_SIZE - 1) {
                    line[line_pos++] = c;
                } else {
                    line_pos = 0;
                    ESP_LOGW(TAG, "Dropped overlong GPS line");
                }
            }
        }
    }

    fflush(nmea_file);
    fflush(csv_file);
    fclose(nmea_file);
    fclose(csv_file);
    free(data);
    unmount_sd_card(&sd);

    result->nmea_lines = nmea_lines;
    result->gga_sentences = gga_sentences;
    result->valid_fixes = valid_fixes;

    ESP_LOGI(TAG, "GPS+SD test summary: NMEA lines=%d GGA=%d valid fixes=%d",
             nmea_lines, gga_sentences, valid_fixes);

    if (nmea_lines <= 0) {
        return ESP_ERR_TIMEOUT;
    }

    return valid_fixes > 0 ? ESP_OK : ESP_ERR_NOT_FOUND;
}

static esp_err_t make_session_paths(char *csv_path, size_t csv_size, char *nmea_path, size_t nmea_size)
{
    struct stat st;

    for (int index = 1; index <= 999; index++) {
        snprintf(csv_path, csv_size, SD_MOUNT_POINT "/track%03d.csv", index);
        if (stat(csv_path, &st) != 0) {
            snprintf(nmea_path, nmea_size, SD_MOUNT_POINT "/track%03d.nmea", index);
            return ESP_OK;
        }
    }

    return ESP_ERR_NO_MEM;
}

static esp_err_t run_recording_session(gps_sd_result_t *result, char *csv_path, size_t csv_size)
{
    memset(result, 0, sizeof(*result));
    csv_path[0] = '\0';

    sd_mount_t sd = {0};
    esp_err_t ret = mount_sd_card(&sd);
    if (ret != ESP_OK) {
        return ret;
    }

    char nmea_path[64];
    ret = make_session_paths(csv_path, csv_size, nmea_path, sizeof(nmea_path));
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to allocate a track filename");
        unmount_sd_card(&sd);
        return ret;
    }

    FILE *blackbox_file = fopen(BLACKBOX_LOG_PATH, "a");
    if (blackbox_file == NULL) {
        ESP_LOGW(TAG, "Failed to open blackbox log: %s", BLACKBOX_LOG_PATH);
    } else {
        blackbox_log(blackbox_file, "INFO", "session_prepare csv=%s nmea=%s", csv_path, nmea_path);
    }

    FILE *csv_file = fopen(csv_path, "w");
    FILE *nmea_file = fopen(nmea_path, "w");
    if (csv_file == NULL || nmea_file == NULL) {
        ESP_LOGE(TAG, "Failed to open track files: %s / %s", csv_path, nmea_path);
        blackbox_log(blackbox_file, "ERROR", "track_file_open_failed csv_ok=%d nmea_ok=%d",
                     csv_file != NULL,
                     nmea_file != NULL);
        if (csv_file != NULL) {
            fclose(csv_file);
        }
        if (nmea_file != NULL) {
            fclose(nmea_file);
        }
        if (blackbox_file != NULL) {
            fclose(blackbox_file);
        }
        unmount_sd_card(&sd);
        return ESP_FAIL;
    }

    fprintf(csv_file, "utc_time,latitude,longitude,satellites,hdop,fix_quality\n");
    ESP_LOGI(TAG, "Recording session started: %s", csv_path);
    ESP_LOGI(TAG, "Raw NMEA file: %s", nmea_path);
    blackbox_log(blackbox_file, "INFO", "session_started csv=%s nmea=%s", csv_path, nmea_path);

    uint8_t *data = malloc(UART_BUF_SIZE);
    if (data == NULL) {
        ESP_LOGE(TAG, "Failed to allocate GPS UART buffer");
        blackbox_log(blackbox_file, "ERROR", "gps_uart_buffer_alloc_failed");
        fclose(csv_file);
        fclose(nmea_file);
        if (blackbox_file != NULL) {
            fclose(blackbox_file);
        }
        unmount_sd_card(&sd);
        return ESP_ERR_NO_MEM;
    }

    set_leds(0, 1, 0);
    beep(2);

    char line[LINE_BUF_SIZE];
    int line_pos = 0;
    bool first_fix_seen = false;
    bool stop_requested = false;

    while (!stop_requested) {
        if (record_button_pressed_event()) {
            blackbox_log(blackbox_file, "INFO", "stop_button_pressed");
            stop_requested = true;
            break;
        }

        int len = uart_read_bytes(GPS_UART_NUM, data, UART_BUF_SIZE, pdMS_TO_TICKS(200));
        if (len <= 0) {
            continue;
        }

        for (int i = 0; i < len && !stop_requested; i++) {
            char c = (char)data[i];
            if (c == '\n') {
                line[line_pos] = '\0';
                line_pos = 0;

                if (line[0] != '$') {
                    continue;
                }

                fprintf(nmea_file, "%s\n", line);
                result->nmea_lines++;

                gps_gga_fix_t fix;
                if (strncmp(line, "$GPGGA", 6) == 0 || strncmp(line, "$GNGGA", 6) == 0) {
                    result->gga_sentences++;
                    if (parse_gga(line, &fix)) {
                        fprintf(csv_file, "%s,%.6f,%.6f,%d,%.2f,%d\n",
                                fix.utc_time,
                                fix.latitude,
                                fix.longitude,
                                fix.satellites,
                                fix.hdop,
                                fix.fix_quality);
                        result->valid_fixes++;
                        result->last_fix = fix;
                        result->has_fix = true;

                        if (!first_fix_seen) {
                            first_fix_seen = true;
                            set_leds(0, 0, 1);
                            beep(1);
                            blackbox_log(blackbox_file,
                                         "INFO",
                                         "first_fix utc=%s lat=%.6f lon=%.6f sats=%d hdop=%.2f",
                                         fix.utc_time,
                                         fix.latitude,
                                         fix.longitude,
                                         fix.satellites,
                                         fix.hdop);
                        }

                        ESP_LOGI(TAG, "Fix %d: lat=%.6f lon=%.6f sats=%d hdop=%.2f",
                                 result->valid_fixes, fix.latitude, fix.longitude,
                                 fix.satellites, fix.hdop);
                    }
                }

                if ((result->nmea_lines % 10) == 0) {
                    fflush(nmea_file);
                    fflush(csv_file);
                    blackbox_log(blackbox_file, "INFO", "progress nmea=%d gga=%d fixes=%d",
                                 result->nmea_lines,
                                 result->gga_sentences,
                                 result->valid_fixes);
                    ESP_LOGI(TAG, "Recording: NMEA=%d GGA=%d fixes=%d",
                             result->nmea_lines, result->gga_sentences, result->valid_fixes);
                }
            } else if (c != '\r') {
                if (line_pos < LINE_BUF_SIZE - 1) {
                    line[line_pos++] = c;
                } else {
                    line_pos = 0;
                    ESP_LOGW(TAG, "Dropped overlong GPS line");
                    blackbox_log(blackbox_file, "WARN", "dropped_overlong_gps_line");
                }
            }
        }
    }

    fflush(nmea_file);
    fflush(csv_file);
    fclose(nmea_file);
    fclose(csv_file);
    blackbox_log(blackbox_file, "INFO", "session_stopped nmea=%d gga=%d fixes=%d csv=%s",
                 result->nmea_lines,
                 result->gga_sentences,
                 result->valid_fixes,
                 csv_path);
    if (blackbox_file != NULL) {
        fclose(blackbox_file);
    }
    free(data);
    unmount_sd_card(&sd);

    ESP_LOGI(TAG, "Recording stopped: NMEA=%d GGA=%d fixes=%d file=%s",
             result->nmea_lines, result->gga_sentences, result->valid_fixes, csv_path);

    return ESP_OK;
}

void app_main(void)
{
    init_status_gpio();
    init_record_button();
    gps_uart_init();

    ESP_LOGI(TAG, "Street-test logger ready. Press record button to start/stop.");
    run_tft_status_screen("TRAIL MAPPER", "READY", "PRESS BUTTON", "TO RECORD");

    while (true) {
        set_leds(0, 1, 0);
        wait_for_record_button();

        set_leds(0, 1, 0);
        run_tft_status_screen("TRAIL MAPPER", "STARTING", "SD GPS LOG", "PLEASE WAIT");

        gps_sd_result_t result;
        char csv_path[64];
        esp_err_t ret = run_recording_session(&result, csv_path, sizeof(csv_path));

        char line1[28];
        char line2[28];
        char line3[28];

        if (ret == ESP_OK) {
            if (result.has_fix) {
                snprintf(line1, sizeof(line1), "SAVED FIX:%d", result.valid_fixes);
                snprintf(line2, sizeof(line2), "LAT:%.5f", result.last_fix.latitude);
                snprintf(line3, sizeof(line3), "LON:%.5f", result.last_fix.longitude);
            } else {
                snprintf(line1, sizeof(line1), "SAVED NMEA:%d", result.nmea_lines);
                snprintf(line2, sizeof(line2), "GGA:%d FIX:0", result.gga_sentences);
                snprintf(line3, sizeof(line3), "NO GPS FIX");
            }
            run_tft_status_screen("RECORD DONE", line1, line2, line3);
            set_leds(0, 1, 0);
            beep(2);
            vTaskDelay(pdMS_TO_TICKS(2500));
            run_tft_status_screen("TRAIL MAPPER", "READY", "PRESS BUTTON", "TO RECORD");
        } else {
            ESP_LOGE(TAG, "Recording session failed: %s", esp_err_to_name(ret));
            snprintf(line1, sizeof(line1), "ERROR:%s", esp_err_to_name(ret));
            snprintf(line2, sizeof(line2), "CHECK SD GPS");
            snprintf(line3, sizeof(line3), "PRESS RESET");
            run_tft_status_screen("RECORD FAIL", line1, line2, line3);
            set_leds(1, 0, 0);
            beep(3);
            vTaskDelay(pdMS_TO_TICKS(4000));
            run_tft_status_screen("TRAIL MAPPER", "READY", "PRESS BUTTON", "TO RECORD");
        }
    }
}
