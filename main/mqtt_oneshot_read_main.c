/*
 * SPDX-FileCopyrightText: 2022-2023 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "soc/soc_caps.h"

#include "esp_log.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "protocol_examples_common.h"
#include "lwip/sockets.h"
#include "lwip/dns.h"
#include "lwip/netdb.h"

#include "mqtt_client.h"

const static char *TAG = "MQTT_ADC_ONESHOT";

#define NOP() asm volatile("nop")

unsigned long IRAM_ATTR micros()
{
    return (unsigned long)(esp_timer_get_time());
}
void IRAM_ATTR delayMicroseconds(uint32_t us)
{
    uint32_t m = micros();
    if (us)
    {
        uint32_t e = (m + us);
        if (m > e)
        { // overflow
            while (micros() > e)
            {
                NOP();
            }
        }
        while (micros() < e)
        {
            NOP();
        }
    }
}

/*---------------------------------------------------------------
        ADC General Macros
---------------------------------------------------------------*/
// ADC1 Channels
#define EXAMPLE_ADC1_CHAN0 ADC_CHANNEL_0
#define EXAMPLE_ADC1_CHAN1 ADC_CHANNEL_3
#define EXAMPLE_ADC1_CHAN2 ADC_CHANNEL_6
#define EXAMPLE_ADC1_CHAN3 ADC_CHANNEL_5
#define EXAMPLE_ADC1_CHAN4 ADC_CHANNEL_4

#define EXAMPLE_ADC_ATTEN ADC_ATTEN_DB_11

// ultrasonic sensor setup

#define ULTRA_RX GPIO_NUM_4
#define ULTRA_TX GPIO_NUM_0

uint64_t rx_bitmask = (1ULL << ULTRA_RX);
uint64_t tx_bitmask = (1ULL << ULTRA_TX);


static uint64_t speed_of_sound = 343; // 


static int adc_raw[3];
static int voltage[3];
static bool example_adc_calibration_init(adc_unit_t unit, adc_channel_t channel, adc_atten_t atten, adc_cali_handle_t *out_handle);
static void example_adc_calibration_deinit(adc_cali_handle_t handle);

static void log_error_if_nonzero(const char *message, int error_code)
{
    if (error_code != 0)
    {
        ESP_LOGE(TAG, "Last error %s: 0x%x", message, error_code);
    }
}

struct ultra_measure_t {
    uint64_t time_start;
    uint64_t time_stop;
    bool measured; 
};

struct ultra_measure_t running_measurement = {0,0,false};

void ultra_isr()
{
    running_measurement.measured = true;
    // vTaskDelay(pdMS_TO_TICKS(1000));
}
void setup_gpio(){
    gpio_config_t ultra_rx_config = {
        .pin_bit_mask = rx_bitmask,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type = GPIO_INTR_POSEDGE};
    gpio_config_t ultra_tx_config = {
        .pin_bit_mask = tx_bitmask,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type = GPIO_INTR_DISABLE};

    ESP_ERROR_CHECK(gpio_config(&ultra_rx_config));
    ESP_ERROR_CHECK(gpio_config(&ultra_tx_config));
    ESP_ERROR_CHECK(gpio_install_isr_service(ESP_INTR_FLAG_EDGE));
    ESP_ERROR_CHECK(gpio_isr_handler_add(ULTRA_RX, ultra_isr, NULL));
    ESP_ERROR_CHECK(esp_timer_early_init());
}

static int get_ultrasonic()
{
    int level;
    int64_t result;
    int64_t distance;

    // struct ultra_measure
    // {
    //     /* data */
    //     uint64_t start,
    //     uint64_t stop,
    //     bool measured
    //     gpio_config_t gpio
    // };


    
    
    gpio_set_level(ULTRA_TX, 0);
    delayMicroseconds(2);
    gpio_set_level(ULTRA_TX, 1);
    delayMicroseconds(10);
    gpio_set_level(ULTRA_TX, 0);

    running_measurement.time_start = esp_timer_get_time();
    ESP_LOGI(TAG, "Measured time_start: %lld", running_measurement.time_start);
    ESP_ERROR_CHECK(gpio_intr_enable(ULTRA_RX));
    ESP_LOGI(TAG, "Measured intr_enable: %lld", running_measurement.time_start);
    level = gpio_get_level(ULTRA_RX);
    ESP_LOGI(TAG, "Measured gpio: %d", level);
    running_measurement.time_stop = esp_timer_get_time();
    ESP_LOGI(TAG, "Measured time_stop: %lld", running_measurement.time_stop);
    if (running_measurement.measured == true)
    {
        ESP_LOGI(TAG, "Interrupt!!!");
        running_measurement.time_stop = esp_timer_get_time();
        ESP_LOGI(TAG, "Measured time_stop: %lld", running_measurement.time_stop);
        if (running_measurement.time_stop > 0)
        {
            result = running_measurement.time_stop - running_measurement.time_start;
            distance = result * speed_of_sound / 1000000;
            ESP_LOGI(TAG, "Measured time: %lld", result);
            ESP_LOGI(TAG, "Measured distance: %lld", distance);
            vTaskDelay(pdMS_TO_TICKS(5000));
        };
        running_measurement.measured = false;
        ESP_ERROR_CHECK(gpio_intr_disable(ULTRA_RX));
    };

    return 1;
}

static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    ESP_LOGD(TAG, "Event dispatched from event loop base=%s, event_id=%" PRIi32 "", base, event_id);
    esp_mqtt_event_handle_t event = event_data;
    esp_mqtt_client_handle_t client = event->client;
    int msg_id;
    switch ((esp_mqtt_event_id_t)event_id)
    {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "MQTT_EVENT_CONNECTED");
        msg_id = esp_mqtt_client_publish(client, "/topic/qos1", "data_3", 0, 1, 0);
        ESP_LOGI(TAG, "sent publish successful, msg_id=%d", msg_id);

        msg_id = esp_mqtt_client_subscribe(client, "/topic/qos0", 0);
        ESP_LOGI(TAG, "sent subscribe successful, msg_id=%d", msg_id);

        msg_id = esp_mqtt_client_subscribe(client, "/topic/qos1", 1);
        ESP_LOGI(TAG, "sent subscribe successful, msg_id=%d", msg_id);

        msg_id = esp_mqtt_client_unsubscribe(client, "/topic/qos1");
        ESP_LOGI(TAG, "sent unsubscribe successful, msg_id=%d", msg_id);
        break;
    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGI(TAG, "MQTT_EVENT_DISCONNECTED");
        break;

    case MQTT_EVENT_SUBSCRIBED:
        ESP_LOGI(TAG, "MQTT_EVENT_SUBSCRIBED, msg_id=%d", event->msg_id);
        msg_id = esp_mqtt_client_publish(client, "/topic/qos0", "data", 0, 0, 0);
        ESP_LOGI(TAG, "sent publish successful, msg_id=%d", msg_id);
        break;
    case MQTT_EVENT_UNSUBSCRIBED:
        ESP_LOGI(TAG, "MQTT_EVENT_UNSUBSCRIBED, msg_id=%d", event->msg_id);
        break;
    case MQTT_EVENT_PUBLISHED:
        ESP_LOGI(TAG, "MQTT_EVENT_PUBLISHED, msg_id=%d", event->msg_id);
        break;
    case MQTT_EVENT_DATA:
        ESP_LOGI(TAG, "MQTT_EVENT_DATA");
        printf("TOPIC=%.*s\r\n", event->topic_len, event->topic);
        printf("DATA=%.*s\r\n", event->data_len, event->data);
        break;
    case MQTT_EVENT_ERROR:
        ESP_LOGI(TAG, "MQTT_EVENT_ERROR");
        if (event->error_handle->error_type == MQTT_ERROR_TYPE_TCP_TRANSPORT)
        {
            log_error_if_nonzero("reported from esp-tls", event->error_handle->esp_tls_last_esp_err);
            log_error_if_nonzero("reported from tls stack", event->error_handle->esp_tls_stack_err);
            log_error_if_nonzero("captured as transport's socket errno", event->error_handle->esp_transport_sock_errno);
            ESP_LOGI(TAG, "Last errno string (%s)", strerror(event->error_handle->esp_transport_sock_errno));
        }
        break;
    default:
        ESP_LOGI(TAG, "Other event id:%d", event->event_id);
        break;
    }
}

void app_main(void)
{

    int msg_id;
    uint64_t test;
    setup_gpio();
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    /* This helper function configures Wi-Fi or Ethernet, as selected in menuconfig.
     * Read "Establishing Wi-Fi or Ethernet Connection" section in
     * examples/protocols/README.md for more information about this function.
     */
    // ESP_ERROR_CHECK(example_connect());

    // esp_mqtt_client_config_t mqtt_cfg = {
    //     .broker.address.uri = CONFIG_BROKER_URL,
    // };

    // esp_mqtt_client_handle_t client = esp_mqtt_client_init(&mqtt_cfg);
    // /* The last argument may be used to pass data to the event handler, in this example mqtt_event_handler */
    // esp_mqtt_client_register_event(client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    // esp_mqtt_client_start(client);

    //-------------ADC1 Init---------------//
    adc_oneshot_unit_handle_t adc1_handle;
    adc_oneshot_unit_init_cfg_t init_config1 = {
        .unit_id = ADC_UNIT_1,
    };
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&init_config1, &adc1_handle));

    //-------------ADC1 Config---------------//
    adc_oneshot_chan_cfg_t config = {
        .bitwidth = ADC_BITWIDTH_DEFAULT,
        .atten = EXAMPLE_ADC_ATTEN,
    };
    ESP_ERROR_CHECK(adc_oneshot_config_channel(adc1_handle, EXAMPLE_ADC1_CHAN0, &config));
    ESP_ERROR_CHECK(adc_oneshot_config_channel(adc1_handle, EXAMPLE_ADC1_CHAN1, &config));
    ESP_ERROR_CHECK(adc_oneshot_config_channel(adc1_handle, EXAMPLE_ADC1_CHAN2, &config));
    ESP_ERROR_CHECK(adc_oneshot_config_channel(adc1_handle, EXAMPLE_ADC1_CHAN3, &config));
    ESP_ERROR_CHECK(adc_oneshot_config_channel(adc1_handle, EXAMPLE_ADC1_CHAN4, &config));

    //-------------ADC1 Calibration Init---------------//
    adc_cali_handle_t adc1_cali_chan0_handle = NULL;
    adc_cali_handle_t adc1_cali_chan1_handle = NULL;
    adc_cali_handle_t adc1_cali_chan2_handle = NULL;
    adc_cali_handle_t adc1_cali_chan3_handle = NULL;
    adc_cali_handle_t adc1_cali_chan4_handle = NULL;
    bool do_calibration1_chan0 = example_adc_calibration_init(ADC_UNIT_1, EXAMPLE_ADC1_CHAN0, EXAMPLE_ADC_ATTEN, &adc1_cali_chan0_handle);
    bool do_calibration1_chan1 = example_adc_calibration_init(ADC_UNIT_1, EXAMPLE_ADC1_CHAN1, EXAMPLE_ADC_ATTEN, &adc1_cali_chan1_handle);
    bool do_calibration1_chan2 = example_adc_calibration_init(ADC_UNIT_1, EXAMPLE_ADC1_CHAN2, EXAMPLE_ADC_ATTEN, &adc1_cali_chan2_handle);
    bool do_calibration1_chan3 = example_adc_calibration_init(ADC_UNIT_1, EXAMPLE_ADC1_CHAN3, EXAMPLE_ADC_ATTEN, &adc1_cali_chan3_handle);
    bool do_calibration1_chan4 = example_adc_calibration_init(ADC_UNIT_1, EXAMPLE_ADC1_CHAN4, EXAMPLE_ADC_ATTEN, &adc1_cali_chan4_handle);

    while (1)
    {
        // ESP_ERROR_CHECK(adc_oneshot_read(adc1_handle, EXAMPLE_ADC1_CHAN0, &adc_raw[0]));
        // ESP_LOGI(TAG, "ADC%d Channel[%d] Raw Data: %d", ADC_UNIT_1 + 1, EXAMPLE_ADC1_CHAN0, adc_raw[0]);
        // if (do_calibration1_chan0)
        // {
        //     ESP_ERROR_CHECK(adc_cali_raw_to_voltage(adc1_cali_chan0_handle, adc_raw[0], &voltage[0]));
        //     ESP_LOGI(TAG, "ADC%d Channel[%d] Cali Voltage: %d mV", ADC_UNIT_1 + 1, EXAMPLE_ADC1_CHAN0, voltage[0]);

        //     char chanNum_char[32];
        //     char data_char[32];
        //     // as per comment from LS_dev, platform is int 16bits
        //     sprintf(chanNum_char, "%i", 0);
        //     sprintf(data_char, "%i", voltage[0]);
        //     char topic[50];
        //     strcpy(topic, "SOIL/MOISTURE1/");
        //     strcat(topic, chanNum_char);
        //     msg_id = esp_mqtt_client_publish(client, topic, data_char, 0, 0, 0);
        // }
        // vTaskDelay(pdMS_TO_TICKS(100));

        // ESP_ERROR_CHECK(adc_oneshot_read(adc1_handle, EXAMPLE_ADC1_CHAN1, &adc_raw[1]));
        // ESP_LOGI(TAG, "ADC%d Channel[%d] Raw Data: %d", ADC_UNIT_1 + 1, EXAMPLE_ADC1_CHAN1, adc_raw[1]);
        // if (do_calibration1_chan1)
        // {
        //     ESP_ERROR_CHECK(adc_cali_raw_to_voltage(adc1_cali_chan1_handle, adc_raw[1], &voltage[1]));
        //     ESP_LOGI(TAG, "ADC%d Channel[%d] Cali Voltage: %d mV", ADC_UNIT_1 + 1, EXAMPLE_ADC1_CHAN1, voltage[1]);
        //     char chanNum_char[32];
        //     char data_char[32];
        //     // as per comment from LS_dev, platform is int 16bits
        //     sprintf(chanNum_char, "%i", 1);
        //     sprintf(data_char, "%i", voltage[1]);
        //     char topic[50];
        //     strcpy(topic, "SOIL/MOISTURE1/");
        //     strcat(topic, chanNum_char);
        //     msg_id = esp_mqtt_client_publish(client, topic, data_char, 0, 0, 0);
        // }
        // vTaskDelay(pdMS_TO_TICKS(100));

        // ESP_ERROR_CHECK(adc_oneshot_read(adc1_handle, EXAMPLE_ADC1_CHAN2, &adc_raw[2]));
        // ESP_LOGI(TAG, "ADC%d Channel[%d] Raw Data: %d", ADC_UNIT_1 + 1, EXAMPLE_ADC1_CHAN2, adc_raw[2]);
        // if (do_calibration1_chan2)
        // {
        //     ESP_ERROR_CHECK(adc_cali_raw_to_voltage(adc1_cali_chan2_handle, adc_raw[2], &voltage[2]));
        //     ESP_LOGI(TAG, "ADC%d Channel[%d] Cali Voltage: %d mV", ADC_UNIT_1 + 1, EXAMPLE_ADC1_CHAN2, voltage[2]);
        //     char chanNum_char[32];
        //     char data_char[32];
        //     // as per comment from LS_dev, platform is int 16bits
        //     sprintf(chanNum_char, "%i", 2);
        //     sprintf(data_char, "%i", voltage[2]);
        //     char topic[50];
        //     strcpy(topic, "SOIL/MOISTURE1/");
        //     strcat(topic, chanNum_char);
        //     msg_id = esp_mqtt_client_publish(client, topic, data_char, 0, 0, 0);
        // }
        // vTaskDelay(pdMS_TO_TICKS(100));

        // ESP_ERROR_CHECK(adc_oneshot_read(adc1_handle, EXAMPLE_ADC1_CHAN3, &adc_raw[3]));
        // ESP_LOGI(TAG, "ADC%d Channel[%d] Raw Data: %d", ADC_UNIT_1 + 1, EXAMPLE_ADC1_CHAN3, adc_raw[3]);
        // if (do_calibration1_chan3)
        // {
        //     ESP_ERROR_CHECK(adc_cali_raw_to_voltage(adc1_cali_chan3_handle, adc_raw[3], &voltage[3]));
        //     ESP_LOGI(TAG, "ADC%d Channel[%d] Cali Voltage: %d mV", ADC_UNIT_1 + 1, EXAMPLE_ADC1_CHAN3, voltage[3]);
        //     char chanNum_char[32];
        //     char data_char[32];
        //     // as per comment from LS_dev, platform is int 16bits
        //     sprintf(chanNum_char, "%i", 3);
        //     sprintf(data_char, "%i", voltage[3]);
        //     char topic[50];
        //     strcpy(topic, "SOIL/MOISTURE1/");
        //     strcat(topic, chanNum_char);
        //     msg_id = esp_mqtt_client_publish(client, topic, data_char, 0, 0, 0);
        // }
        // vTaskDelay(pdMS_TO_TICKS(100));

        // ESP_ERROR_CHECK(adc_oneshot_read(adc1_handle, EXAMPLE_ADC1_CHAN4, &adc_raw[4]));
        // ESP_LOGI(TAG, "ADC%d Channel[%d] Raw Data: %d", ADC_UNIT_1 + 1, EXAMPLE_ADC1_CHAN4, adc_raw[4]);
        // if (do_calibration1_chan4)
        // {
        //     ESP_ERROR_CHECK(adc_cali_raw_to_voltage(adc1_cali_chan4_handle, adc_raw[4], &voltage[4]));
        //     ESP_LOGI(TAG, "ADC%d Channel[%d] Cali Voltage: %d mV", ADC_UNIT_1 + 1, EXAMPLE_ADC1_CHAN4, voltage[4]);
        //     char chanNum_char[32];
        //     char data_char[32];
        //     // as per comment from LS_dev, platform is int 16bits
        //     sprintf(chanNum_char, "%i", 4);
        //     sprintf(data_char, "%i", voltage[4]);
        //     char topic[50];
        //     strcpy(topic, "SOIL/MOISTURE1/");
        //     strcat(topic, chanNum_char);
        //     msg_id = esp_mqtt_client_publish(client, topic, data_char, 0, 0, 0);
        // }
        get_ultrasonic();
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    // Tear Down
    ESP_ERROR_CHECK(adc_oneshot_del_unit(adc1_handle));
    if (do_calibration1_chan0)
    {
        example_adc_calibration_deinit(adc1_cali_chan0_handle);
    }
    if (do_calibration1_chan1)
    {
        example_adc_calibration_deinit(adc1_cali_chan1_handle);
    }
    if (do_calibration1_chan2)
    {
        example_adc_calibration_deinit(adc1_cali_chan2_handle);
    }
}

/*---------------------------------------------------------------
        ADC Calibration
---------------------------------------------------------------*/
static bool example_adc_calibration_init(adc_unit_t unit, adc_channel_t channel, adc_atten_t atten, adc_cali_handle_t *out_handle)
{
    adc_cali_handle_t handle = NULL;
    esp_err_t ret = ESP_FAIL;
    bool calibrated = false;

#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    if (!calibrated)
    {
        ESP_LOGI(TAG, "calibration scheme version is %s", "Curve Fitting");
        adc_cali_curve_fitting_config_t cali_config = {
            .unit_id = unit,
            .chan = channel,
            .atten = atten,
            .bitwidth = ADC_BITWIDTH_DEFAULT,
        };
        ret = adc_cali_create_scheme_curve_fitting(&cali_config, &handle);
        if (ret == ESP_OK)
        {
            calibrated = true;
        }
    }
#endif

#if ADC_CALI_SCHEME_LINE_FITTING_SUPPORTED
    if (!calibrated)
    {
        ESP_LOGI(TAG, "calibration scheme version is %s", "Line Fitting");
        adc_cali_line_fitting_config_t cali_config = {
            .unit_id = unit,
            .atten = atten,
            .bitwidth = ADC_BITWIDTH_DEFAULT,
        };
        ret = adc_cali_create_scheme_line_fitting(&cali_config, &handle);
        if (ret == ESP_OK)
        {
            calibrated = true;
        }
    }
#endif

    *out_handle = handle;
    if (ret == ESP_OK)
    {
        ESP_LOGI(TAG, "Calibration Success");
    }
    else if (ret == ESP_ERR_NOT_SUPPORTED || !calibrated)
    {
        ESP_LOGW(TAG, "eFuse not burnt, skip software calibration");
    }
    else
    {
        ESP_LOGE(TAG, "Invalid arg or no memory");
    }

    return calibrated;
}

static void example_adc_calibration_deinit(adc_cali_handle_t handle)
{
#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    ESP_LOGI(TAG, "deregister %s calibration scheme", "Curve Fitting");
    ESP_ERROR_CHECK(adc_cali_delete_scheme_curve_fitting(handle));

#elif ADC_CALI_SCHEME_LINE_FITTING_SUPPORTED
    ESP_LOGI(TAG, "deregister %s calibration scheme", "Line Fitting");
    ESP_ERROR_CHECK(adc_cali_delete_scheme_line_fitting(handle));
#endif
}
