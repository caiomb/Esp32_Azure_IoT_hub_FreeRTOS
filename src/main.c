// Bibliotecas C
#include <stdio.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>

// Bibliotecas do ESP32
#include <esp_wifi.h>
#include <esp_event_loop.h>
#include <esp_system.h>
#include <nvs_flash.h>
#include <esp_event.h>
#include <esp_log.h>
#include <mqtt_client.h>
#include <driver/gpio.h>
#include <driver/ledc.h>

// Bibliotecas do FreeRTOS
#include <freertos\FreeRTOS.h>
#include <freertos\task.h>
#include <freertos\queue.h>
#include <freertos\event_groups.h>

// Bibliotecas de rede
#include "lwip/err.h"
#include "lwip/sys.h"

// Bibliotecas de rede
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "lwip/dns.h"

// Include da lib do sensor DHT
#include "DHT.h"

// Include da lib cJSON
#include <cJSON.h>

// Includes para usar o IoT Hub Azure
#include "iothub_client.h"
#include "iothub_device_client_ll.h"
#include "iothub_client_options.h"
#include "iothub_message.h"
#include "azure_c_shared_utility/threadapi.h"
#include "azure_c_shared_utility/crt_abstractions.h"
#include "azure_c_shared_utility/platform.h"
#include "azure_c_shared_utility/shared_util_options.h"
#include "iothubtransportmqtt.h"
#include "iothub_client_options.h"

// Include com algumas configurações
#include "constants.h"

// ID do dispositivo que será enviado na mensagem JSON
#define DEVICE_ID "ESP32_CAIO_BRAGA"

// Defines dos GPIOs utilizados
#define LED_PIN     GPIO_NUM_4
#define SERVO_PIN   GPIO_NUM_5
#define DHT22_PIN   GPIO_NUM_16
#define BT_PIN      GPIO_NUM_32

/* Referencia para saber status de conexão */
static EventGroupHandle_t wifi_event_group;
const static int CONNECTION_STATUS = BIT0;

/* Declaração do handle da fila que será usada para envio de dados da task de leitura
 * para a task de publicação de dados */
xQueueHandle data_queue;

/* Declaração do buffer que será utilizado para recuperar dados da fila e enviá-los
 * ao broker mqtt */
char mqtt_buffer[256];

int led_state = 0;
int pwm_value = 0;
static const char *TAG = "esp32-azure";
static const char *connectionString = AZURE_IOT_CONNECTION_STRING;
static int callbackCounter;

typedef struct EVENT_INSTANCE_TAG
{
    IOTHUB_MESSAGE_HANDLE messageHandle;
    size_t messageTrackingId; // For tracking the messages within the user callback.
} EVENT_INSTANCE;

static void getCmdFromJSON(const char* data)
{
    /* Objetos do tipo cJSON usados para realizar o parser das mensagens recebidas
     * através do broker mqtt */  
    cJSON* root = NULL;
    cJSON* led = NULL;
    cJSON* pwm = NULL;
    int pwm_aux = 0;

    root = cJSON_Parse(data);
    if(root == NULL)
    {
        ESP_LOGI(TAG, "Error parsing JSON");
        return;
    }

    // Parsea o JSON buscando a chave LED, e caso encontre o valor é atribuido para led
    led  = cJSON_GetObjectItem(root, "LED");
    if(led == NULL)
    {
        ESP_LOGI(TAG, "Error parsing JSON");
        return;
    }

    // Parsea o JSON buscando a chave PWM, e caso encontre o valor é atribuido para pwm
    pwm = cJSON_GetObjectItem(root, "PWM");
    if(pwm == NULL)
    {
        ESP_LOGI(TAG, "Error parsing JSON");
        return;
    }

    if(strcmp(led->valuestring, "1") == 0)
    {
        ESP_LOGI(TAG, "Comando liga led recebido");
        gpio_set_level(LED_PIN, 1); // Caso o valor recebido em CMD seja igual a 1, liga o LED
        led_state = 1; // Atualiza a variável que salva o estado do led
    }
    else if(strcmp(led->valuestring, "0") == 0)
    {
        ESP_LOGI(TAG, "Comando desliga led recebido");
        gpio_set_level(LED_PIN, 0); // Caso o valor recebido em CMD seja igual a 0, liga o LED
        led_state = 0; // Atualiza a variável que salva o estado do led
    }
    else
    {
        // Caso o valor recebido não seja 1 e nem 0, o comando é invalido
        ESP_LOGI(TAG, "Comando invalido para o led");
    }

    if(strcmp(pwm->valuestring, "") == 0)
    {
        ESP_LOGI(TAG, "Set de PWM vazio. PWM = 0");
        pwm_value = 0;
    }
    else
    {
        pwm_aux = atoi(pwm->valuestring);
        if( pwm_aux >= 0 && pwm_aux <= 100 ) // Os valores de PWM podem variar de 0 a 100%
        {
            pwm_value = pwm_aux;
            ESP_LOGI(TAG, "Set de PWM recebido: [%d] por cento", pwm_value);
        }
    }

    // Liberação dos ponteiros alocados para parsear o JSON
    if (root != NULL)
        cJSON_Delete(root);
}

static IOTHUBMESSAGE_DISPOSITION_RESULT ReceiveMessageCallback(IOTHUB_MESSAGE_HANDLE message, void* userContextCallback)
{
    int* counter = (int*)userContextCallback;
    const char* buffer;
    size_t size;

    // Message content
    if (IoTHubMessage_GetByteArray(message, (const unsigned char**)&buffer, &size) != IOTHUB_MESSAGE_OK)
    {
        printf("unable to retrieve the message data\r\n");
    }
    else
    {
        printf("Received Message [%d]\r\n Data: [%.*s]\r\n", *counter, (int)size, buffer);
        getCmdFromJSON(buffer);
    }

    /* Some device specific action code goes here... */
    (*counter)++;
    return IOTHUBMESSAGE_ACCEPTED;
}

void connection_status_callback(IOTHUB_CLIENT_CONNECTION_STATUS result, IOTHUB_CLIENT_CONNECTION_STATUS_REASON reason, void *userContextCallback)
{
    printf("\n\nConnection Status result:%s, Connection Status reason: %s\n\n", ENUM_TO_STRING(IOTHUB_CLIENT_CONNECTION_STATUS, result),
           ENUM_TO_STRING(IOTHUB_CLIENT_CONNECTION_STATUS_REASON, reason));
}

static void SendConfirmationCallback(IOTHUB_CLIENT_CONFIRMATION_RESULT result, void *userContextCallback)
{
    EVENT_INSTANCE *eventInstance = (EVENT_INSTANCE *)userContextCallback;
    size_t id = eventInstance->messageTrackingId;

    if (result == IOTHUB_CLIENT_CONFIRMATION_OK)
    {
        (void)printf("Confirmation[%d] received for message tracking id = %d with result = %s\r\n", callbackCounter, (int)id, ENUM_TO_STRING(IOTHUB_CLIENT_CONFIRMATION_RESULT, result));
        /* Some device specific action code goes here... */
        callbackCounter++;
    }
    IoTHubMessage_Destroy(eventInstance->messageHandle);
}

// Função de callback que irá tratar eventos do WiFi
static esp_err_t wifi_event_handler(void *ctx, system_event_t *event)
{
    switch(event->event_id) {
    case SYSTEM_EVENT_STA_START:
        esp_wifi_connect(); // inicia conexao wifi
        break;
    case SYSTEM_EVENT_STA_GOT_IP:
        xEventGroupSetBits(wifi_event_group, CONNECTION_STATUS); // seta status de conexao
        break;
    case SYSTEM_EVENT_STA_DISCONNECTED:
        esp_wifi_connect(); // tenta conectar de novo
        xEventGroupClearBits(wifi_event_group, CONNECTION_STATUS); // limpa status de conexao
        break;
    default:
        break;
    } // fim do switch-case
    return ESP_OK;
}

// Inicializacao da conexao com a rede WiFi
void wifi_init(void)
{
    tcpip_adapter_init();
    wifi_event_group = xEventGroupCreate();
    ESP_ERROR_CHECK( esp_event_loop_init(wifi_event_handler, NULL) );
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();

    ESP_ERROR_CHECK( esp_wifi_init(&cfg) );
    ESP_ERROR_CHECK( esp_wifi_set_storage(WIFI_STORAGE_RAM) );

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,      // Nome da rede
            .password = WIFI_PASS,  // Senha da rede
        },
    };

    ESP_ERROR_CHECK( esp_wifi_set_mode(WIFI_MODE_STA) );
    ESP_ERROR_CHECK( esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_config) );
    ESP_LOGI(TAG, "Iniciando Conexao com Rede WiFi...");
    ESP_ERROR_CHECK( esp_wifi_start() );
    ESP_LOGI(TAG, "Conectando...");

    // Aguarda até que a conexão seja estabelecida com sucesso 
    xEventGroupWaitBits(wifi_event_group, CONNECTION_STATUS, false, true, portMAX_DELAY);
}

static void hw_init()
{
    ESP_LOGI(TAG, "Configurando GPIOs...");

    // Configuração do DHT22
    setDHTgpio(DHT22_PIN);

    // Configuração do pino onde o LED será conectado
    gpio_pad_select_gpio(LED_PIN);
    gpio_set_direction(LED_PIN, GPIO_MODE_OUTPUT);

    // Configuração do pino onde o botão será conectado
    gpio_set_direction(BT_PIN,GPIO_MODE_INPUT);
    gpio_set_pull_mode(BT_PIN, GPIO_PULLUP_ONLY);
}

void azure_task()
{
    IOTHUB_CLIENT_LL_HANDLE iotHubClientHandle;
    EVENT_INSTANCE message;
    srand((unsigned int)time(NULL));
    callbackCounter = 0;
    int receiveContext = 0;
    int iterator = 0;
    bool traceOn = true;
    time_t sent_time = 0;
    time_t current_time = 0;

    if (platform_init() != 0)
    {
        printf("Failed to initialize the platform\r\n");
        return;
    }

    iotHubClientHandle = IoTHubClient_LL_CreateFromConnectionString(connectionString, MQTT_Protocol);
    if (iotHubClientHandle  == NULL)
    {
        printf("ERROR: iotHubClientHandle is NULL!\r\n");
        return;
    }

    IoTHubClient_LL_SetOption(iotHubClientHandle, OPTION_LOG_TRACE, &traceOn);
    IoTHubClient_LL_SetConnectionStatusCallback(iotHubClientHandle, connection_status_callback, NULL);

    /* Setting Message call back, so we can receive Commands. */
    if (IoTHubClient_LL_SetMessageCallback(iotHubClientHandle, ReceiveMessageCallback, &receiveContext) != IOTHUB_CLIENT_OK)
    {
        printf("ERROR: IoTHubClient_LL_SetMessageCallback..........FAILED!\r\n");
        return;
    }

    while (1)
    {
        time(&current_time);
        if( iterator <= callbackCounter)
        {
            if(difftime(current_time, sent_time) > TX_INTERVAL_SECOND)
            {
                memset(mqtt_buffer, '\0', sizeof(mqtt_buffer)); // Limpa o buffer
                /* Recebe os dados enviados pela task read_data para a fila data_queue
                * e atribui para a variavel mqtt_bufer*/
                xQueueReceive(data_queue, mqtt_buffer, portMAX_DELAY);
                message.messageHandle = IoTHubMessage_CreateFromByteArray((const unsigned char *)mqtt_buffer, strlen(mqtt_buffer));
                if (message.messageHandle == NULL)
                {
                    printf("ERROR: iotHubMessageHandle is NULL!\r\n");
                }
                else
                {
                    message.messageTrackingId = iterator;
                    if (IoTHubClient_LL_SendEventAsync(iotHubClientHandle, message.messageHandle, SendConfirmationCallback, &message) != IOTHUB_CLIENT_OK)
                    {
                        printf("ERROR: IoTHubClient_LL_SendEventAsync..........FAILED!\r\n");
                    }
                    else
                    {
                        time(&sent_time);
                        printf("IoTHubClient_LL_SendEventAsync accepted message [%d] for transmission to IoT Hub.\r\n", (int)iterator);
                    }
                }
                iterator++;
            }
        }
        else
        {
            IoTHubClient_LL_DoWork(iotHubClientHandle);
        }
        vTaskDelay(1000/portTICK_RATE_MS);
    }
    IoTHubClient_LL_Destroy(iotHubClientHandle);
    platform_deinit();

    vTaskDelete(NULL);
}

void servo_control_task(void* pvParameter)
{
    ESP_LOGD(TAG, "Servo control task...");

    ledc_timer_config_t ledc_timer_conf = {
        .freq_hz         = 50,
        .speed_mode      = LEDC_HIGH_SPEED_MODE,
        .timer_num       = LEDC_TIMER_0,
        .duty_resolution = LEDC_TIMER_15_BIT
    };
    ledc_timer_config(&ledc_timer_conf);

    int duty_max = ((1 << ledc_timer_conf.duty_resolution)-1);
    int pwm_aux = 0;

    ledc_channel_config_t ledc_channel_conf = {
        .channel    = LEDC_CHANNEL_0,
        .duty       = pwm_aux,
        .gpio_num   = SERVO_PIN,
        .intr_type  = LEDC_INTR_DISABLE,
        .speed_mode = LEDC_HIGH_SPEED_MODE,
        .timer_sel  = LEDC_TIMER_0
    };
    ledc_channel_config(&ledc_channel_conf);

    float porcentagem = 0.0;
    int duty_aux = 0;

    while(1)
    {
        if(pwm_aux != pwm_value)
        {
            porcentagem = (pwm_value*0.01);
            duty_aux = (int)(duty_max * porcentagem);
            ledc_set_duty(LEDC_HIGH_SPEED_MODE, LEDC_CHANNEL_0, duty_aux); // Altera duty cycle
            ledc_update_duty(LEDC_HIGH_SPEED_MODE, LEDC_CHANNEL_0);
            pwm_aux = pwm_value;
            ESP_LOGI(TAG, "Duty cycle alterado para: [%d]", duty_aux);
        }
        vTaskDelay( 2000/portTICK_PERIOD_MS );
    }
}

void read_data(void *pvParameter)
{
    ESP_LOGI(TAG, "Iniciando task de leitura dos dados...");
    int dht_data = 0; // Variável que armazenará o dado do DHT a cada leitura
    int btn_state = 0;  // Variável que armazenará o estado do botão a cada leitura
    unsigned int sample_number = 0; // Váriavel que salvará o número da amostra
    cJSON *root; // Objeto que salvará o objeto JSON criado com os dados da amostra
    while (1)
    {
        /* Retorna a amostra para 0 caso o número máximo que a variável possa armazenar
         * tenha sido atingido, ou incrementa ela caso contrário */
        sample_number = (sample_number < UINT32_MAX) ? (sample_number+1) : 0;

        // Leitura dos dados do sensor DHT22
        ESP_LOGI(TAG, "Lendo dados de temperatura e umidade do DHT22...");
        dht_data = readDHT();
        errorHandler(dht_data);

        // Leitura do estado do botão
        ESP_LOGI(TAG, "Lendo estado do botao...");
        btn_state = gpio_get_level(BT_PIN);

        // Criação da mensagem em formato JSON onde os dados serão enviados
        root=cJSON_CreateObject();
        cJSON_AddStringToObject(root, "ID", DEVICE_ID);          // Concatena o device id no JSON
        cJSON_AddNumberToObject(root, "SAMPLE", sample_number);  // Concatena o numero da amostra no JSON
        cJSON_AddNumberToObject(root, "TEMP", getTemperature()); // Concatena a temperatura no JSON
        cJSON_AddNumberToObject(root, "UMID", getHumidity());    // Concatena a umidade no JSON
        cJSON_AddNumberToObject(root, "BTN", btn_state);         // Concatena o estado do botao no JSON
        cJSON_AddNumberToObject(root, "LED", led_state);         // Concatena o estado do led no JSON
        cJSON_AddNumberToObject(root, "PWM", pwm_value);         // Concatena o estado do PWM no JSON

        /* Adiciona a string formatada pela lib cJSON na ultima posição da fila usada para troca de mensagens
         * entre esta task e a task send_data */
        xQueueSend(data_queue, cJSON_Print(root), 0);
        cJSON_Delete(root);
        // Espera 60 segundos para realizar uma nova leitura
        vTaskDelay(60000/portTICK_RATE_MS);
    }
}

void app_main()
{
    ESP_LOGI(TAG, "Iniciando ESP32 IoT App...");
    
    esp_log_level_set("*", ESP_LOG_INFO);
    esp_log_level_set("MQTT_CLIENT", ESP_LOG_VERBOSE);

    esp_err_t ret = nvs_flash_init();
    if(ret == ESP_ERR_NVS_NO_FREE_PAGES)
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Criação da fila usada para troca de mensagens entre as tasks read_data e send_data
    data_queue = xQueueCreate(20, sizeof(mqtt_buffer));
    if(data_queue == NULL)
    {
        ESP_LOGE("controle_init", "Erro na inicialização da fila de dados");
        return;
    }

    hw_init();   // Chamada para a função que configura o hardware
    wifi_init(); // Chamada para a função que inicializa a conexão WiFi

    // Criação das tasks
    if (xTaskCreate(&read_data, "Read Data", 2048, NULL, 5, NULL) != pdPASS)
    {
        ESP_LOGE(TAG, "Create read_data task failed");
        return;
    }

    if (xTaskCreate(&azure_task, "azure_task", 1024 * 5, NULL, 5, NULL) != pdPASS)
    {
        ESP_LOGE(TAG, "create azure task failed");
        return;
    }

    if (xTaskCreate(&servo_control_task, "Servo control", 2048, NULL, 1, NULL) != pdPASS)
    {
        ESP_LOGE(TAG, "create servo control task failed");
        return;
    }
}