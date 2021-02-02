/*   example:
 *   nc -ul 7777 | aplay -r 16000 -f S16_BE
 *
 */


#include "freertos/FreeRTOS.h"
#include "esp_wifi.h"
#include "esp_system.h"
#include "esp_event.h"
#include "esp_event_loop.h"
#include "nvs_flash.h"
#include "driver/gpio.h"

#include "driver/gpio.h"
#include "driver/adc.h"
#include "driver/timer.h"
#include <soc/sens_reg.h>
#include <soc/sens_struct.h>
#include "lwip/err.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"
#include <lwip/netdb.h>
#include "esp_log.h"
#include "driver/i2s.h"

// local setup
#define ADC_SAMPLES_COUNT 512
#define SAMPLE_RATE  16000
#define HOST_IP_ADDR "192.168.86.31"
#define HOST_PORT 7777

#define I2S_SAMPLE_RATE 44100
#define I2S_BUFFER 512

// ADC buffer
uint8_t abuf[ADC_SAMPLES_COUNT];
uint8_t abuf_tx[ADC_SAMPLES_COUNT];
int16_t abufPos = 0;

// timer stuff
static intr_handle_t s_timer_handle;
portMUX_TYPE DRAM_ATTR timerMux = portMUX_INITIALIZER_UNLOCKED;

// RTOS stuff
TaskHandle_t udp_task_handle;
TaskHandle_t i2s_task_handle;

// debug tag
static const char *TAG = "Scope1:";
int wifi_init = false;



//----------------------------------------------------------------------------------------//
// I2S setup for ADC sampling
//----------------------------------------------------------------------------------------//
void init_i2s()
{
	// configure I2S
	i2s_config_t i2s_config;
	i2s_config.mode = I2S_MODE_MASTER | I2S_MODE_RX | I2S_MODE_ADC_BUILT_IN;
	i2s_config.sample_rate = I2S_SAMPLE_RATE;
	i2s_config.dma_buf_len = I2S_BUFFER;
	i2s_config.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
	//i2s_config.channel_format = I2S_CHANNEL_FMT_ONLY_LEFT;
	i2s_config.channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT;
	i2s_config.use_apll = false,
	i2s_config.communication_format = I2S_COMM_FORMAT_PCM;
	i2s_config.intr_alloc_flags = 0;
	i2s_config.dma_buf_count = 5;

	// install and start i2s driver
	ESP_ERROR_CHECK( adc_gpio_init(ADC_UNIT_1, ADC_CHANNEL_0) );
	ESP_ERROR_CHECK( i2s_driver_install(I2S_NUM_0, &i2s_config,  0, NULL) );
	ESP_ERROR_CHECK( i2s_set_adc_mode(ADC_UNIT_1, ADC1_CHANNEL_0) );

	//vTaskDelay(5000 / portTICK_RATE_MS);
	//printf("Done waiting, enable ADC... \n");

	ESP_ERROR_CHECK( i2s_adc_enable(I2S_NUM_0) );
}


void disp_buf(uint8_t* buf, int length)
{
    printf("======\n");
    for (int i = 0; i < length; i++) {
        printf("%02x ", buf[i]);
        if ((i + 1) % 16 == 0) {
            printf("\n");
        }
    }
    printf("======\n");
}


//----------------------------------------------------------------------------------------//
// ADC sampling through I2S
//----------------------------------------------------------------------------------------//
void sample_i2s()
{
	ESP_LOGI(TAG, "Task called.");
	//uint16_t i2s_buff[I2S_BUFFER];
	int i2s_read_len = I2S_BUFFER;
	char* i2s_buff = (char*) calloc(i2s_read_len, sizeof(char));

	// send UDP data
	int addr_family = 0;
	int ip_protocol = 0;
	char host_ip[] = HOST_IP_ADDR;

	struct sockaddr_in dest_addr;
	dest_addr.sin_addr.s_addr = inet_addr(HOST_IP_ADDR);
	dest_addr.sin_family = AF_INET;
	dest_addr.sin_port = htons(HOST_PORT);
	addr_family = AF_INET;
	ip_protocol = IPPROTO_IP;
	int sock = -1;
	sock = socket(addr_family, SOCK_DGRAM, ip_protocol);
	if (sock < 0) {
		ESP_LOGE(TAG, "Unable to create socket: errno %d", errno);
	}
	ESP_LOGI(TAG, "Socket created, sending to %s:%d", HOST_IP_ADDR, HOST_PORT);


	while (true) {

		size_t bytes_read = 0;
		//ESP_ERROR_CHECK( i2s_adc_enable(I2S_NUM_0) );

		//ESP_LOGI(TAG, "before call.");
		ESP_ERROR_CHECK( i2s_read(I2S_NUM_0, (void*) i2s_buff, i2s_read_len, &bytes_read, portMAX_DELAY) );
		//ESP_LOGI(TAG, "after call.");

		//ESP_LOGI(TAG,"I2s bytes read: %d", bytes_read);

		if (bytes_read == 0)
			continue;

		//vTaskDelay(10 / portTICK_RATE_MS);

		memcpy(abuf_tx, i2s_buff, bytes_read);

//		// convert to bytes
//		for (int i=0; i < bytes_read; i++) {
//			abuf_tx[abufPos++] = (i2s_buff[i] >> 8) & 0xFF;
//			abuf_tx[abufPos++] = i2s_buff[i] & 0xFF;
//		}

//		// notify ADC task that the buffer is full
//		BaseType_t xHigherPriorityTaskWoken = pdFALSE;
//		vTaskNotifyGiveFromISR(udp_task_handle, &xHigherPriorityTaskWoken);
//		if (xHigherPriorityTaskWoken) {
//		  portYIELD_FROM_ISR();
//		}

		// send UDP payload
		int err = sendto(sock, (const uint8_t *)abuf_tx, ADC_SAMPLES_COUNT, 0, (struct sockaddr *)&dest_addr, sizeof(dest_addr));
		if (err < 0)
		{
			ESP_LOGE(TAG, "Error occurred during sending: errno %d", errno);
		}
	}

	shutdown(sock, 0);
	close(sock);
}

//----------------------------------------------------------------------------------------//
// ADC setup
//----------------------------------------------------------------------------------------//
void init_adc( void )
{
	adc1_config_width(ADC_WIDTH_BIT_12);
	adc1_config_channel_atten(ADC1_CHANNEL_0, ADC_ATTEN_DB_0);
}


//----------------------------------------------------------------------------------------//
// timer setup
//----------------------------------------------------------------------------------------//
static void timer_isr(void* arg)
{
	portENTER_CRITICAL_ISR(&timerMux);
    TIMERG0.int_clr_timers.t0 = 1;
    TIMERG0.hw_timer[0].config.alarm_en = 1;

    // read ADC
    uint16_t adc_value = adc1_get_raw(ADC1_CHANNEL_0);

    // convert to bytes
    abuf[abufPos++] = (adc_value >> 8) & 0xFF;
    abuf[abufPos++] = adc_value & 0xFF;

    // copy buffer into a second buffer for transmission
    if (abufPos >= ADC_SAMPLES_COUNT) {
        abufPos = 0;
	    memcpy(abuf_tx, abuf, ADC_SAMPLES_COUNT);

        // notify ADC task that the buffer is full
        BaseType_t xHigherPriorityTaskWoken = pdFALSE;
        vTaskNotifyGiveFromISR(udp_task_handle, &xHigherPriorityTaskWoken);
        if (xHigherPriorityTaskWoken) {
          portYIELD_FROM_ISR();
        }
    }

	portEXIT_CRITICAL_ISR(&timerMux);
}


//----------------------------------------------------------------------------------------//
// init timer
//----------------------------------------------------------------------------------------//
void init_timer(int timer_period_us)
{
    timer_config_t config = {
            .alarm_en = true,
            .counter_en = false,
            .intr_type = TIMER_INTR_LEVEL,
            .counter_dir = TIMER_COUNT_UP,
            .auto_reload = true,
            .divider = 80   /* 1 us per tick */
    };

    timer_init(TIMER_GROUP_0, TIMER_0, &config);
    timer_set_counter_value(TIMER_GROUP_0, TIMER_0, 0);
    timer_set_alarm_value(TIMER_GROUP_0, TIMER_0, timer_period_us);
    timer_enable_intr(TIMER_GROUP_0, TIMER_0);
    timer_isr_register(TIMER_GROUP_0, TIMER_0, &timer_isr, NULL, 0, &s_timer_handle);
    timer_start(TIMER_GROUP_0, TIMER_0);
}


//----------------------------------------------------------------------------------------//
// send UDP data
//----------------------------------------------------------------------------------------//
void udp_task(void *param) {
	int addr_family = 0;
	int ip_protocol = 0;
	char host_ip[] = HOST_IP_ADDR;

	struct sockaddr_in dest_addr;
	dest_addr.sin_addr.s_addr = inet_addr(HOST_IP_ADDR);
	dest_addr.sin_family = AF_INET;
	dest_addr.sin_port = htons(HOST_PORT);
	addr_family = AF_INET;
	ip_protocol = IPPROTO_IP;
	int sock = -1;
	while (sock < 0) {
		sock = socket(addr_family, SOCK_DGRAM, ip_protocol);
		if (sock < 0)
			ESP_LOGE(TAG, "Unable to create socket: errno %d", errno);
	}
	ESP_LOGI(TAG, "Socket created, sending to %s:%d", HOST_IP_ADDR, HOST_PORT);
	while (true) {
		// sleep until the ISR gives us something to do
		uint32_t tcount = ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

//	    for (int i = 0; i < 10; i++) {
//	    	ESP_LOGI(TAG, "ADC value: %d", abuf_tx[i]);
//	    }

		// send UDP payload
		ESP_LOGI(TAG, "UDPTask got called.");
		int err = sendto(sock, (const uint8_t *)abuf_tx, ADC_SAMPLES_COUNT, 0, (struct sockaddr *)&dest_addr, sizeof(dest_addr));
		if (err < 0)
		{
			ESP_LOGE(TAG, "Error occurred during sending: errno %d", errno);
			ESP_LOGI(TAG, "Closing socket. Trying again...");
			close(sock);
			sock = -1;
			while (sock < 0) {
				sock = socket(addr_family, SOCK_DGRAM, ip_protocol);
				if (sock < 0)
					ESP_LOGE(TAG, "Unable to create socket: errno %d", errno);
			}
			ESP_LOGI(TAG, "Connected again.");
		}
		else
			ESP_LOGI(TAG, "Packet sent.");
	}
	shutdown(sock, 0);
	close(sock);
}


//----------------------------------------------------------------------------------------//
// UDP client socket
//----------------------------------------------------------------------------------------//
void init_socket(void){
	struct sockaddr_in dest_addr;
    dest_addr.sin_addr.s_addr = inet_addr(HOST_IP_ADDR);
    dest_addr.sin_family = AF_INET;
    dest_addr.sin_port = htons(HOST_PORT);
    int addr_family = AF_INET;
    int ip_protocol = IPPROTO_IP;

    // connect to UDF server
    while (1) {
		int sock = socket(addr_family, SOCK_DGRAM, ip_protocol);
		if (sock < 0) {
			ESP_LOGE(TAG, "Unable to create socket: errno %d", errno);
		}
		ESP_LOGI(TAG, "Socket created, sending to %s:%d", HOST_IP_ADDR, HOST_PORT);
		break;
    }
}


//----------------------------------------------------------------------------------------//
// event handler
//----------------------------------------------------------------------------------------//
esp_err_t event_handler(void *ctx, system_event_t *event)
{
    switch(event->event_id) {
    case SYSTEM_EVENT_STA_START:
		esp_wifi_connect();
		ESP_LOGI(TAG,"Disconnected! Trying to connect to WiFi network...");
		break;
    case SYSTEM_EVENT_STA_GOT_IP:
		ESP_LOGI(TAG, "Received assigned IP Address:%s",
				 ip4addr_ntoa(&event->event_info.got_ip.ip_info.ip));
		if (!wifi_init) {
			wifi_init = true;

			// init timer TBD <<< enable again
			//float calls = (float)1/(float)SAMPLE_RATE * 1000000;
			//ESP_LOGI(TAG, "Timer is set to: %f", calls);
			//init_timer(calls);
		}
	    break;
    case SYSTEM_EVENT_STA_DISCONNECTED:
		esp_wifi_connect();
		ESP_LOGI(TAG,"Disconnected! Trying to connect to WiFi network...");
		break;
    default:
        break;
    }
    return ESP_OK;
}


//----------------------------------------------------------------------------------------//
// init wifi connection
//----------------------------------------------------------------------------------------//
void init_wifi(void)
{
    nvs_flash_init();
    tcpip_adapter_init();
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK( esp_event_loop_init(event_handler, NULL) );
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK( esp_wifi_init(&cfg) );
    ESP_ERROR_CHECK( esp_wifi_set_storage(WIFI_STORAGE_RAM) );
    ESP_ERROR_CHECK( esp_wifi_set_mode(WIFI_MODE_STA) );
    wifi_config_t sta_config = {
        .sta = {
            .ssid = CONFIG_ESP_WIFI_SSID,
            .password = CONFIG_ESP_WIFI_PASSWORD,
            .bssid_set = false
        }
    };
    ESP_ERROR_CHECK( esp_wifi_set_config(WIFI_IF_STA, &sta_config) );
    ESP_ERROR_CHECK( esp_wifi_start() );
    ESP_ERROR_CHECK( esp_wifi_connect() );

    // wait until IP is assigned
    ESP_LOGI(TAG,"Waiting for IP address...");
    while (!wifi_init) {
    	vTaskDelay(100 / portTICK_RATE_MS);
    }

}


//----------------------------------------------------------------------------------------//
// main app logic
//----------------------------------------------------------------------------------------//
void app_main(void)
{
	// init wifi
	init_wifi();

    // init ADC
    init_adc();

	// init I2S
	init_i2s();

	// UDP caller task
	//xTaskCreate(udp_task, "UDP Client Task", 8192, NULL, 5, &udp_task_handle);

	// I2S sampler task
	xTaskCreate(sample_i2s, "I2S Task", 8192, NULL, 5, &i2s_task_handle);
}

