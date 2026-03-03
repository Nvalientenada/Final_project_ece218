#include <stdio.h>
#include <string.h>
#include <stdbool.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_rom_sys.h"

////////////
// PIN MAP //
////////////

// LEDs
#define LED_BACK_LEFT    GPIO_NUM_4
#define LED_FRONT_LEFT   GPIO_NUM_5
#define LED_BACK_RIGHT   GPIO_NUM_2
#define LED_FRONT_RIGHT  GPIO_NUM_38

// Buzzer
#define BUZZER_PIN       GPIO_NUM_6

// Button (pull-up)
#define BUTTON_PIN       GPIO_NUM_16

// Ultrasonic sensors
#define FL_TRIG          GPIO_NUM_15
#define FL_ECHO          GPIO_NUM_7

#define BL_TRIG          GPIO_NUM_17
#define BL_ECHO          GPIO_NUM_18

#define FR_TRIG          GPIO_NUM_21
#define FR_ECHO          GPIO_NUM_1

#define BR_TRIG          GPIO_NUM_20
#define BR_ECHO          GPIO_NUM_19

// LCD (HD44780 16x2, 4-bit mode)
#define LCD_RS           GPIO_NUM_14
#define LCD_EN           GPIO_NUM_13
#define LCD_D4           GPIO_NUM_12
#define LCD_D5           GPIO_NUM_11
#define LCD_D6           GPIO_NUM_10
#define LCD_D7           GPIO_NUM_9

///////////////
// SETTINGS  //
///////////////
static const char *TAG = "PARK_ASSIST";

// Distance thresholds (cm)
static const int SAFE_DIST_CM   = 60;
static const int DANGER_DIST_CM = 30;

// Timeouts
static const int ECHO_TIMEOUT_US = 30000;  // 30 ms max wait for echo

// Buzzer PWM settings
#define LEDC_MODE_USED         LEDC_LOW_SPEED_MODE
#define LEDC_TIMER_USED        LEDC_TIMER_0
#define LEDC_CHANNEL_USED      LEDC_CHANNEL_0
#define LEDC_DUTY_RES          LEDC_TIMER_10_BIT  // 0..1023
#define BUZZER_DUTY            512                // ~50%

static bool buzzer_muted = false;

// For pulsing the buzzer in CAUTION state (beep-beep)
static int beep_phase = 0;

/////////////////
// LCD DRIVER  //
/////////////////

static void lcd_pulse_enable(void)
{
    // EN pulse needs to be clean and not too fast
    gpio_set_level(LCD_EN, 0);
    esp_rom_delay_us(5);

    gpio_set_level(LCD_EN, 1);
    esp_rom_delay_us(5);

    gpio_set_level(LCD_EN, 0);
    esp_rom_delay_us(100); // latch time
}

static void lcd_write4(uint8_t nibble)
{
    // Put nibble on D4..D7
    gpio_set_level(LCD_D4, (nibble >> 0) & 1);
    gpio_set_level(LCD_D5, (nibble >> 1) & 1);
    gpio_set_level(LCD_D6, (nibble >> 2) & 1);
    gpio_set_level(LCD_D7, (nibble >> 3) & 1);

    esp_rom_delay_us(5);   // let lines settle
    lcd_pulse_enable();
}

static void lcd_send(uint8_t value, bool is_data)
{
    gpio_set_level(LCD_RS, is_data ? 1 : 0);
    esp_rom_delay_us(5);

    // high nibble then low nibble
    lcd_write4(value >> 4);
    lcd_write4(value & 0x0F);
}

static void lcd_cmd(uint8_t cmd)
{
    lcd_send(cmd, false);

    // extra time
    if (cmd == 0x01 || cmd == 0x02) {
        vTaskDelay(pdMS_TO_TICKS(3));
    } else {
        esp_rom_delay_us(50);
    }
}

static void lcd_data(uint8_t data)
{
    lcd_send(data, true);
    esp_rom_delay_us(50);
}

// Safe, slow init 
static void lcd_init_16x2(void)
{
    // force all LCD lines LOW before anything else
    gpio_set_level(LCD_RS, 0);
    gpio_set_level(LCD_EN, 0);
    gpio_set_level(LCD_D4, 0);
    gpio_set_level(LCD_D5, 0);
    gpio_set_level(LCD_D6, 0);
    gpio_set_level(LCD_D7, 0);

    vTaskDelay(pdMS_TO_TICKS(200)); // BIG power-up wait

    // "0x33, 0x32" init
    lcd_write4(0x03); vTaskDelay(pdMS_TO_TICKS(10));
    lcd_write4(0x03); vTaskDelay(pdMS_TO_TICKS(10));
    lcd_write4(0x03); vTaskDelay(pdMS_TO_TICKS(10));
    lcd_write4(0x02); vTaskDelay(pdMS_TO_TICKS(10)); // 4-bit mode

    lcd_cmd(0x28); vTaskDelay(pdMS_TO_TICKS(5));  // 4-bit, 2-line
    lcd_cmd(0x08); vTaskDelay(pdMS_TO_TICKS(5));  // display OFF
    lcd_cmd(0x01); vTaskDelay(pdMS_TO_TICKS(10)); // clear
    lcd_cmd(0x06); vTaskDelay(pdMS_TO_TICKS(5));  // entry mode
    lcd_cmd(0x0C); vTaskDelay(pdMS_TO_TICKS(5));  // display ON
}

// Set cursor position (col 0-15, row 0-1)
static void lcd_set_cursor(int col, int row)
{
    uint8_t addr = (row == 0) ? 0x00 : 0x40;
    addr += col;
    lcd_cmd(0x80 | addr);
}

// Print a normal C string to LCD
static void lcd_print(const char *s)
{
    while (*s) {
        lcd_data((uint8_t)*s);
        s++;
    }
}

// Print one full 16-char line (pads with spaces so old chars disappear)
static void lcd_print_line(int row, const char *text)
{
    char buf[17];
    snprintf(buf, sizeof(buf), "%-16s", text);  // pad to 16 chars
    lcd_set_cursor(0, row);
    lcd_print(buf);
}

///////////////////////////////////////////////////////////////
// ------------- ULTRASONIC READING -------------------------//
///////////////////////////////////////////////////////////////

// Returns distance in cm. If timeout/fail, returns 999.
static int read_distance_cm(gpio_num_t trig, gpio_num_t echo)
{
    // Trigger pulse: LOW 2us, HIGH 10us, LOW
    gpio_set_level(trig, 0);
    esp_rom_delay_us(2);
    gpio_set_level(trig, 1);
    esp_rom_delay_us(10);
    gpio_set_level(trig, 0);

    // Wait for echo to go HIGH
    int64_t start_wait = esp_timer_get_time();
    while (gpio_get_level(echo) == 0) {
        if ((esp_timer_get_time() - start_wait) > ECHO_TIMEOUT_US) {
            return 999; // no echo start
        }
    }

    // Measure how long echo stays HIGH
    int64_t echo_start = esp_timer_get_time();
    while (gpio_get_level(echo) == 1) {
        if ((esp_timer_get_time() - echo_start) > ECHO_TIMEOUT_US) {
            return 999; // echo too long
        }
    }
    int64_t echo_end = esp_timer_get_time();

    int64_t pulse_us = echo_end - echo_start;

    // Convert microseconds to cm
    int dist_cm = (int)((pulse_us * 0.0343) / 2.0);

    // Sanity clamp: ignore nonsense values to reduce LCD/LED jitter
    if (dist_cm < 2 || dist_cm > 400) return 999;

    return dist_cm;
}

///////////////////////////////////////////////////////////////
// -------------------- BUZZER CONTROL -----------------------
///////////////////////////////////////////////////////////////

static void buzzer_init_pwm(void)
{
    ledc_timer_config_t timer = {
        .speed_mode       = LEDC_MODE_USED,
        .timer_num        = LEDC_TIMER_USED,
        .duty_resolution  = LEDC_DUTY_RES,
        .freq_hz          = 1000, // default
        .clk_cfg          = LEDC_AUTO_CLK
    };
    ledc_timer_config(&timer);

    ledc_channel_config_t ch = {
        .gpio_num   = BUZZER_PIN,
        .speed_mode = LEDC_MODE_USED,
        .channel    = LEDC_CHANNEL_USED,
        .timer_sel  = LEDC_TIMER_USED,
        .duty       = 0,
        .hpoint     = 0
    };
    ledc_channel_config(&ch);
}

static void buzzer_off(void)
{
    ledc_set_duty(LEDC_MODE_USED, LEDC_CHANNEL_USED, 0);
    ledc_update_duty(LEDC_MODE_USED, LEDC_CHANNEL_USED);
}

static void buzzer_tone(int freq_hz)
{
    ledc_set_freq(LEDC_MODE_USED, LEDC_TIMER_USED, freq_hz);
    ledc_set_duty(LEDC_MODE_USED, LEDC_CHANNEL_USED, BUZZER_DUTY);
    ledc_update_duty(LEDC_MODE_USED, LEDC_CHANNEL_USED);
}

static void update_buzzer(int min_dist_cm)
{
    if (buzzer_muted) {
        buzzer_off();
        return;
    }

    // SAFE: silent
    if (min_dist_cm > SAFE_DIST_CM) {
        buzzer_off();
        beep_phase = 0; // reset beep pattern when safe
        return;
    }

    // CAUTION: beep-beep 
    if (min_dist_cm > DANGER_DIST_CM) {
        beep_phase = (beep_phase + 1) % 4; // 0..9
        if (beep_phase < 2) buzzer_tone(1000); // ON ~30% of the time
        else buzzer_off();
        return;
    }

    // DANGER: continuous loud tone
    buzzer_tone(2000);
}

///////////////////////////////////////////////////////////////
// -------------------- STATUS LOGIC -------------------------
///////////////////////////////////////////////////////////////

static const char* status_from_distance(int d)
{
    if (d > SAFE_DIST_CM) return "SAFE";
    if (d > DANGER_DIST_CM) return "CAUTION";
    return "DANGER";
}

static void led_update(gpio_num_t led_pin, int dist_cm)
{
    // LED ON when NOT safe (close obstacle)
    gpio_set_level(led_pin, (dist_cm < SAFE_DIST_CM) ? 1 : 0);
}

///////////////////////////////////////////////////////////////
// ------------------ BUTTON (debounced) ---------------------
///////////////////////////////////////////////////////////////

static void handle_button_toggle_mute(void)
{
    static int last_level = 1; // pull-up is idle HIGH
    static int64_t last_change_us = 0;

    int level = gpio_get_level(BUTTON_PIN);
    int64_t now = esp_timer_get_time();

    // debounce: accept change only if stable
    if (level != last_level) {
        last_change_us = now;
        last_level = level;
    }

    // detect press (LOW) stable for 200ms
    if (level == 0 && (now - last_change_us) > 200000) {
        // wait until release so it toggles once
        while (gpio_get_level(BUTTON_PIN) == 0) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        buzzer_muted = !buzzer_muted;
        ESP_LOGI(TAG, "Buzzer muted = %s", buzzer_muted ? "TRUE" : "FALSE");
    }
}

///////////////////////////////////////////////////////////////
// -------------------------- MAIN ---------------------------
///////////////////////////////////////////////////////////////

void app_main(void)
{
    // -------- GPIO CONFIG --------

    // Outputs: LEDs + Trigs + LCD pins
    gpio_config_t out_cfg = {
        .pin_bit_mask = (1ULL << LED_BACK_LEFT) |
                        (1ULL << LED_FRONT_LEFT) |
                        (1ULL << LED_BACK_RIGHT) |
                        (1ULL << LED_FRONT_RIGHT) |
                        (1ULL << FL_TRIG) | (1ULL << BL_TRIG) |
                        (1ULL << FR_TRIG) | (1ULL << BR_TRIG) |
                        (1ULL << LCD_RS) | (1ULL << LCD_EN) |
                        (1ULL << LCD_D4) | (1ULL << LCD_D5) |
                        (1ULL << LCD_D6) | (1ULL << LCD_D7),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    gpio_config(&out_cfg);

    // Echo pins input (NO pullups)
    gpio_config_t echo_cfg = {
        .pin_bit_mask = (1ULL << FL_ECHO) |
                        (1ULL << BL_ECHO) |
                        (1ULL << FR_ECHO) |
                        (1ULL << BR_ECHO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    gpio_config(&echo_cfg);

    // Button input (WITH pullup)
    gpio_config_t btn_cfg = {
        .pin_bit_mask = (1ULL << BUTTON_PIN),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    gpio_config(&btn_cfg);

    // Turn off LEDs initially
    gpio_set_level(LED_BACK_LEFT, 0);
    gpio_set_level(LED_FRONT_LEFT, 0);
    gpio_set_level(LED_BACK_RIGHT, 0);
    gpio_set_level(LED_FRONT_RIGHT, 0);

    // Init buzzer PWM
    buzzer_init_pwm();
    buzzer_off();

    // Init LCD
    lcd_init_16x2();
    lcd_print_line(0, "Parking Assist");
    lcd_print_line(1, "Starting...");
    vTaskDelay(pdMS_TO_TICKS(1500));

    // -------- MAIN LOOP --------
    while (1) {
        // Button check
        handle_button_toggle_mute();

        // Read sensors ONE AT A TIME 
        int dFL = read_distance_cm(FL_TRIG, FL_ECHO);
        vTaskDelay(pdMS_TO_TICKS(30));

        int dFR = read_distance_cm(FR_TRIG, FR_ECHO);
        vTaskDelay(pdMS_TO_TICKS(30));

        int dBL = read_distance_cm(BL_TRIG, BL_ECHO);
        vTaskDelay(pdMS_TO_TICKS(30));

        int dBR = read_distance_cm(BR_TRIG, BR_ECHO);
        vTaskDelay(pdMS_TO_TICKS(30));

        // Find minimum distance (closest obstacle)
        int min1 = (dFL < dFR) ? dFL : dFR;
        int min2 = (dBL < dBR) ? dBL : dBR;
        int minD = (min1 < min2) ? min1 : min2;

        const char *status = status_from_distance(minD);

        // LEDs show which side is close
        led_update(LED_FRONT_LEFT,  dFL);
        led_update(LED_FRONT_RIGHT, dFR);
        led_update(LED_BACK_LEFT,   dBL);
        led_update(LED_BACK_RIGHT,  dBR);

        // Buzzer based on MIN distance
        update_buzzer(minD);

        // LCD update (2 lines)
        char line0[17];
        char line1[17];

        snprintf(line0, sizeof(line0), "Dist:%3d cm", minD);
        if (buzzer_muted) {
            snprintf(line1, sizeof(line1), "%s MUTED", status);
        } else {
            snprintf(line1, sizeof(line1), "%s", status);
        }

        lcd_print_line(0, line0);
        lcd_print_line(1, line1);

        // Serial log in monitor
        ESP_LOGI(TAG, "FL:%d FR:%d BL:%d BR:%d | MIN:%d (%s)%s",
                 dFL, dFR, dBL, dBR, minD, status, buzzer_muted ? " [MUTED]" : "");

        // This loop delay also controls the beep timing for CAUTION
        vTaskDelay(pdMS_TO_TICKS(200));
    }
}