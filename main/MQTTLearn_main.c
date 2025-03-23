// 控制端代码：
// 通过按钮控制，按钮按一下，发布开灯主题；按钮再按一下，发布关灯主题；
// 低功耗：按键按下瞬间发布主题，发布完主题就睡眠，等待下一次大的按键按下；
#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "esp_wifi.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "protocol_examples_common.h"

#include "esp_mac.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"

#include "lwip/sockets.h"
#include "lwip/dns.h"
#include "lwip/netdb.h"

#include "esp_log.h"
#include "mqtt_client.h"

#include "driver/ledc.h"
#include "esp_err.h"
#include "driver/gpio.h"

// 函数声明
void buttonSwitch(void);
static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data);
static void mqtt_app_start(void);
void buttonSwitch(void);

esp_mqtt_client_handle_t gMqttClient = NULL;
static const char *TAG = "mqtt_example";

#define GPIO_INPUT_PIN_SEL ((1ULL << GPIO_NUM_0)) // 按键
static int model = 0;                             // 0---开灯；1---关灯；
static uint8_t gpiosign = 0;

// 函数1：按键初始化
void btn0InitToGpio0(void)
{
    // zero-initialize the config structure.
    gpio_config_t io_conf = {};
    io_conf.intr_type = GPIO_INTR_DISABLE;
    io_conf.mode = GPIO_MODE_INPUT;
    io_conf.pin_bit_mask = GPIO_INPUT_PIN_SEL; // 即GPIO_NUM_0
    io_conf.pull_down_en = 0;
    io_conf.pull_up_en = 1; 
    //解释：设置按键初始状态为高电平所以需要上拉电阻，按下与GND导通，为低电平，这样就能知道是低电平为触发按键的信号；
    gpio_config(&io_conf);
}

static void log_error_if_nonzero(const char *message, int error_code)
{
    if (error_code != 0)
    {
        ESP_LOGE(TAG, "Last error %s: 0x%x", message, error_code);
    }
}

/*
 * @brief 注册事件处理程序以接收 MQTT 事件
 *
 * 此函数由 MQTT 客户端事件循环调用。
 *
 * @param handler_args 注册到事件的用户数据。
 * @param base 处理程序的事件基础（此示例中始终为 MQTT 基础）。
 * @param event_id 接收事件的 ID。
 * @param event_data 事件的数据，esp_mqtt_event_handle_t。
 */
static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    // ESP_LOGI(TAG, "Event dispatched from event loop base=%s, event_id=%" PRIi32 "", base, event_id);
    esp_mqtt_event_handle_t event = event_data;
    // esp_mqtt_client_handle_t client = event->client;
    // int msg_id;
    switch ((esp_mqtt_event_id_t)event_id)
    {
    case MQTT_EVENT_CONNECTED:
        gMqttClient = event->client;
        ESP_LOGI(TAG, "MQTT_EVENT_CONNECTED");
        break;
    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGI(TAG, "MQTT_EVENT_DISCONNECTED");
        break;
    case MQTT_EVENT_SUBSCRIBED:
        ESP_LOGI(TAG, "MQTT_EVENT_SUBSCRIBED, msg_id=%d", event->msg_id);
        break;
    case MQTT_EVENT_UNSUBSCRIBED:
        ESP_LOGI(TAG, "MQTT_EVENT_UNSUBSCRIBED, msg_id=%d", event->msg_id);
        break;
    case MQTT_EVENT_PUBLISHED:
        // ESP_LOGI(TAG, "MQTT_EVENT_PUBLISHED, msg_id=%d", event->msg_id);
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

// 函数2：MQTT启动
static void mqtt_app_start(void)
{
    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = "mqtt://222.186.21.32",
    };
    esp_mqtt_client_handle_t client = esp_mqtt_client_init(&mqtt_cfg); /* 最后一个参数可用于将数据传递给事件处理程序，在此示例中为 mqtt_event_handler */
    esp_mqtt_client_register_event(client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    // 这个函数充当一个门铃的作用，当客户端（client）发生SP_EVENT_ANY_ID事件时，调用回调函数mqtt_event_handler，让这个回调函数做出相对应的处理；
    // ESP_EVENT_ANY_ID是事件 ID，表示“监听所有类型的 MQTT 事件”。如果是监听特定事件，比如MQTT_EVENT_CONNECTED，可替换ANY_ID
    esp_mqtt_client_start(client);
}

// 函数3：检测按键状态
void buttonSwitch(void)
{
    if (gpio_get_level(GPIO_NUM_0) == 0) // 低电平，即按键按下
    {
        esp_rom_delay_us(5000);                               // 5ms用于按键消抖
        if (gpio_get_level(GPIO_NUM_0) == 0 && gpiosign == 0) // 说明按键按下
        {
            gpiosign = 1;
        }
    }
    else
    {
        if (gpiosign != 0) // 即按键按下才会使得gpiosign转变为1
        {
            model = (model == 0 ? 1 : 0); // 按键按下切换灯的开关状态，若此时为开灯，按下按键则关灯；
            if (gMqttClient != NULL)
            {
                if (model == 1) // GB2312 中国 4B   //UTF-8 不是中国的 1-4B
                {
                    esp_mqtt_client_publish(gMqttClient, "ControlSign", "开", 0, 1, 0);
                    printf("我发布了开灯信息\r\n");
                }
                else
                {
                    esp_mqtt_client_publish(gMqttClient, "ControlSign", "关", 3, 1, 0);
                    printf("我发布了关灯信息\r\n");
                }
            }
            printf("model=%d\r\n", model);
            gpiosign = 0; // 复原，等下一次按键按下再变为1
        }
    }
}

// ————————————————————————————————main函数——————————————————————————————————————
void app_main(void)
{
    ESP_LOGI(TAG, "[APP] Startup..");
    ESP_LOGI(TAG, "[APP] Free memory: %" PRIu32 " bytes", esp_get_free_heap_size()); //%" PRIu32" 是 C 语言中用于格式化打印 uint32_t 类型变量的宏
    ESP_LOGI(TAG, "[APP] IDF version: %s", esp_get_idf_version());

    esp_log_level_set("*", ESP_LOG_INFO);
    esp_log_level_set("mqtt_client", ESP_LOG_VERBOSE);
    esp_log_level_set("mqtt_example", ESP_LOG_VERBOSE);
    esp_log_level_set("transport_base", ESP_LOG_VERBOSE);
    esp_log_level_set("esp-tls", ESP_LOG_VERBOSE);
    esp_log_level_set("transport", ESP_LOG_VERBOSE);
    esp_log_level_set("outbox", ESP_LOG_VERBOSE);

    // 1. 按键初始化
    btn0InitToGpio0();
    // 2. 连接WiFi
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(example_connect()); // 此辅助函数配置 Wi-Fi 或以太网，还需在menuconfig 中配置wifi的名称和密码；
    // 3. 连接MQTT服务器
    mqtt_app_start();
    // 4. 循环检测按键的状态
    while (1)
    {
        buttonSwitch();
        vTaskDelay(pdMS_TO_TICKS(10)); // 延时，喂狗
    }
}
