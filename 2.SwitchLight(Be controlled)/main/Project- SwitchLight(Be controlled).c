/*
被控端：
1.订阅“ControSign”的主题发布的消息；
2.如果接收到的是“开”，顺时针转动10%，如果接收到的是“关”，逆时针转动10%；
3.初始化本机按钮，按一下改变舵机方向，从而也能达到开关灯的效果；
 */
#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "esp_wifi.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_mac.h"
#include "esp_log.h"
#include "esp_err.h"
#include "protocol_examples_common.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"

#include "lwip/sockets.h"
#include "lwip/dns.h"
#include "lwip/netdb.h"

#include "driver/gpio.h"

#include "mqtt_client.h"

#include "driver/ledc.h"

#define GPIO_INPUT_PIN_SEL ((1ULL << GPIO_NUM_0)) // 按键
#define LEDC_TIMER LEDC_TIMER_0
#define LEDC_MODE LEDC_LOW_SPEED_MODE
#define LEDC_OUTPUT_IO (2) // Define the output GPIO
#define LEDC_CHANNEL LEDC_CHANNEL_0
#define LEDC_DUTY_RES LEDC_TIMER_13_BIT // Set duty resolution to 13 bits, 2的13次方就是8192
#define LEDC_FREQUENCY (50)             // Frequency in Hertz. Set frequency at 50Hz，这样一周期就是20ms；

static const char *TAG = "mqtt_example";
static int model = 0; // 0--开灯；1--关灯；
static uint8_t gpiosign = 0;
esp_mqtt_client_handle_t gMqttClient = NULL;
int msg_id;

// 函数1：舵机Servo_SG90初始化
static void SG90_init(void)
{
    // Prepare and then apply the LEDC PWM timer configuration
    ledc_timer_config_t ledc_timer = {
        .speed_mode = LEDC_MODE,          // 1. 为什么是要低速？---考虑不同硬件的兼容性
        .duty_resolution = LEDC_DUTY_RES, // 2. 为什么是要13bit？---也是考虑兼容性
        .timer_num = LEDC_TIMER,
        .freq_hz = LEDC_FREQUENCY, // Set output frequency at 50Hz---SG90文档规定以20ms为一个周期进行通讯；
        .clk_cfg = LEDC_AUTO_CLK};
    ESP_ERROR_CHECK(ledc_timer_config(&ledc_timer));

    // Prepare and then apply the LEDC PWM channel configuration
    ledc_channel_config_t ledc_channel = {
        .speed_mode = LEDC_MODE,
        .channel = LEDC_CHANNEL,
        .timer_sel = LEDC_TIMER,
        .intr_type = LEDC_INTR_DISABLE, // 中断禁用
        .gpio_num = LEDC_OUTPUT_IO,
        .duty = 0,    // Set duty to 0%---初始化占空比为0，duty最大值=2**duty_resolution即2的13次方=8192
        .hpoint = 0}; // 高电平从周期起始位置开始
    ESP_ERROR_CHECK(ledc_channel_config(&ledc_channel));
}

// 函数2：按键初始化
void btn0InitToGpio0(void)
{
    // zero-initialize the config structure.
    gpio_config_t io_conf = {};
    io_conf.intr_type = GPIO_INTR_DISABLE;
    io_conf.mode = GPIO_MODE_INPUT;
    io_conf.pin_bit_mask = GPIO_INPUT_PIN_SEL; // GPIO_NUM_0
    io_conf.pull_down_en = 0;
    io_conf.pull_up_en = 1;
    gpio_config(&io_conf);
}

// 函数3：按键控制舵机来实现改变开关灯
void buttonControl(void)
{
    if (gpio_get_level(GPIO_NUM_0) == 0)
    {
        esp_rom_delay_us(5000); // 消抖
        if (gpio_get_level(GPIO_NUM_0) == 0 && gpiosign == 0)
        {
            gpiosign = 1;
        }
    }
    else
    {
        if (gpiosign != 0)
        {
            model = (model == 0 ? 1 : 0); // 按键按下切换灯的开关状态，若此时为开灯，按下按键则关灯；
            printf("model=%d\r\n", model);
            gpiosign = 0;
        }
    }
}

// 函数4：调整百分比控制舵机旋转角度的函数
char SG90_angle_control(char percent)
{
    ESP_ERROR_CHECK(ledc_set_duty(LEDC_MODE, LEDC_CHANNEL, 8192 * (0.025f + (percent * 0.001f))));
    ESP_ERROR_CHECK(ledc_update_duty(LEDC_MODE, LEDC_CHANNEL));
    vTaskDelay(50 / portTICK_PERIOD_MS); // 延时50ms
    return 0;
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
    switch ((esp_mqtt_event_id_t)event_id)
    {
    case MQTT_EVENT_CONNECTED:
        gMqttClient = event->client;
        ESP_LOGI(TAG, "MQTT_EVENT_CONNECTED");
        esp_mqtt_client_subscribe(gMqttClient, "ControlSign", 1); // 订阅控制端的开关灯消息
        ESP_LOGI(TAG, "MQTT_EVENT_SUBSCRIBED, msg_id=%d", event->msg_id);
        break;
    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGI(TAG, "MQTT_EVENT_DISCONNECTED");
        break;
    case MQTT_EVENT_SUBSCRIBED:
        // esp_mqtt_client_subscribe(gMqttClient, "ControlSign", 1); // 订阅控制端的开关灯消息
        // ESP_LOGI(TAG, "MQTT_EVENT_SUBSCRIBED, msg_id=%d", event->msg_id);
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
        if (strncmp(event->data, "开", event->data_len) == 0)
        {
            ESP_LOGI(TAG, "Message=开灯");
            model = 0; // 把灯的状态更新为开灯；
            printf("model=%d\r\n", model);
        }
        else if (strncmp(event->data, "关", event->data_len) == 0)
        {
            ESP_LOGI(TAG, "Message=关灯");
            model = 1; // 把灯的状态更新为关灯；
            printf("model=%d\r\n", model);
        }
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

// 函数5：启动MQTT
static void mqtt_app_start(void)
{
    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = "mqtt://222.186.21.32",
    };
    esp_mqtt_client_handle_t client = esp_mqtt_client_init(&mqtt_cfg); /* 最后一个参数可用于将数据传递给事件处理程序，在此示例中为 mqtt_event_handler */
    esp_mqtt_client_register_event(client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    esp_mqtt_client_start(client);
}

// 函数6：根据不同model切换舵机，旋转到一定角度使其达到开关灯的效果，然后舵机复位
uint8_t model_old = 0;
void modelControl(void)
{
    if (model == 1 && model_old != model) // 关灯
    {
        SG90_angle_control(70);
        vTaskDelay(pdMS_TO_TICKS(300));
        SG90_angle_control(60); // 使舵机恢复中间位置
        model_old = model;
        msg_id = esp_mqtt_client_publish(gMqttClient, "model_status", "1", 0, 0, 0);//状态同步：将model的状态更新发布给控制端；
    }
    else if (model == 0 && model_old != model) // 开灯
    {
        SG90_angle_control(50);
        vTaskDelay(pdMS_TO_TICKS(300));
        SG90_angle_control(60); // 使舵机恢复中间位置
        model_old = model;
        msg_id = esp_mqtt_client_publish(gMqttClient, "model_status", "0", 0, 0, 0);
    }
}

// —————————————————————————————————————主函数——————————————————————————————————————
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

    // 1.连接WiFi
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(example_connect()); // 此辅助函数配置 Wi-Fi 或以太网，还需在menuconfig 中配置wifi的名称和密码；
    // 2.按键初始化
    btn0InitToGpio0();
    // 3.舵机初始化
    SG90_init();
    // 4.连接MQTT服务器
    mqtt_app_start(); // 初始化只要一次；
    // 5.循环检查按键状态，切换不同状态；
    while (1)
    {
        buttonControl(); // 本机按键控制切换model状态；
        modelControl();

        vTaskDelay(pdMS_TO_TICKS(10)); // 延时，喂狗
    }
}
