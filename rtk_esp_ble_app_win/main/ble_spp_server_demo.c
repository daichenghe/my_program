/*
   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/


#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_bt.h"
#include "driver/uart.h"
#include "string.h"

#include "esp_gap_ble_api.h"
#include "esp_gatts_api.h"
#include "esp_bt_defs.h"
#include "esp_bt_main.h"
#include "ble_spp_server_demo.h"
#include "esp_gatt_common_api.h"
#include "esp_gap_ble_api.h"



#define UART_CHANNEL UART_NUM_0
#define BUF_SIZE (1024 * 1)

#define GATTS_TABLE_TAG  "ACEINNA_BLE_DEMO"

#define SPP_PROFILE_NUM             1
#define SPP_PROFILE_APP_IDX         0
#define ESP_SPP_APP_ID              0x57    //0x56
#define SAMPLE_DEVICE_NAME          "ACEINNA_BLE"
#define SPP_SVC_INST_ID	            0

/// SPP Service
static const uint16_t spp_service_uuid = 0xABF0;
/// Characteristic UUID TODO:
#define ESP_GATT_UUID_SPP_DATA_RECEIVE      0xABF1      //Attribute handle 自动生成128 bit 四种权限属性
#define ESP_GATT_UUID_SPP_DATA_NOTIFY       0xABF2      //发送数据
#define ESP_GATT_UUID_SPP_COMMAND_RECEIVE   0xABF3
#define ESP_GATT_UUID_SPP_COMMAND_NOTIFY    0xABF4

#ifdef SUPPORT_HEARTBEAT
#define ESP_GATT_UUID_SPP_HEARTBEAT         0xABF5
#endif

static const uint8_t spp_adv_data[24] = {   //TODO: 广播数据  ACEINNA_BLE_000
    0x02,0x01,0x06,
    0x03,0x03,0xF0,0xAB,
    0x10,0x09,0x41,0x43,0x45,0x49,0x4e,0x4e,0x41,0x5f,0x42,0x4c,0x45,0x5f,0x30,0x30,0x30,0x31
};
//0x45,0x53,0x50,0x5f,0x53,0x50,0x50,0x5f,0x53,0x45,0x52,0x56,0x45,0x52 ESP_SPP_SERVER

static int read_flag = 0;

//static uint16_t spp_mtu_size = 23;
static uint16_t spp_mtu_size = 23;
static uint16_t spp_conn_id = 0xffff;
static esp_gatt_if_t spp_gatts_if = 0xff;
QueueHandle_t spp_uart_queue = NULL;
static xQueueHandle cmd_cmd_queue = NULL;

#ifdef SUPPORT_HEARTBEAT
static xQueueHandle cmd_heartbeat_queue = NULL;
static uint8_t  heartbeat_s[9] = {'E','s','p','r','e','s','s','i','f'};
static bool enable_heart_ntf = false;
static uint8_t heartbeat_count_num = 0;
#endif

static bool enable_data_ntf = false;    //my_test
//static bool enable_data_ntf = true;
static bool is_connected = false;
static esp_bd_addr_t spp_remote_bda = {0x0,};

static uint16_t spp_handle_table[SPP_IDX_NB];

static esp_ble_adv_params_t spp_adv_params = {
    .adv_int_min        = 0x20,
    .adv_int_max        = 0x40,
    .adv_type           = ADV_TYPE_IND,
    .own_addr_type      = BLE_ADDR_TYPE_PUBLIC,
    .channel_map        = ADV_CHNL_ALL,
    .adv_filter_policy  = ADV_FILTER_ALLOW_SCAN_ANY_CON_ANY,
};

struct gatts_profile_inst {
    esp_gatts_cb_t gatts_cb;
    uint16_t gatts_if;
    uint16_t app_id;
    uint16_t conn_id;
    uint16_t service_handle;
    esp_gatt_srvc_id_t service_id;
    uint16_t char_handle;
    esp_bt_uuid_t char_uuid;
    esp_gatt_perm_t perm;
    esp_gatt_char_prop_t property;
    uint16_t descr_handle;
    esp_bt_uuid_t descr_uuid;
};

typedef struct spp_receive_data_node{
    int32_t len;
    uint8_t * node_buff;
    struct spp_receive_data_node * next_node;
}spp_receive_data_node_t;

static spp_receive_data_node_t * temp_spp_recv_data_node_p1 = NULL;
static spp_receive_data_node_t * temp_spp_recv_data_node_p2 = NULL;

typedef struct spp_receive_data_buff{
    int32_t node_num;
    int32_t buff_size;
    spp_receive_data_node_t * first_node;
}spp_receive_data_buff_t;

static spp_receive_data_buff_t SppRecvDataBuff = {
    .node_num   = 0,
    .buff_size  = 0,
    .first_node = NULL
};

static void gatts_profile_event_handler(esp_gatts_cb_event_t event, esp_gatt_if_t gatts_if, esp_ble_gatts_cb_param_t *param);

/* One gatt-based profile one app_id and one gatts_if, this array will store the gatts_if returned by ESP_GATTS_REG_EVT */
static struct gatts_profile_inst spp_profile_tab[SPP_PROFILE_NUM] = {
    [SPP_PROFILE_APP_IDX] = {
        .gatts_cb = gatts_profile_event_handler,
        .gatts_if = ESP_GATT_IF_NONE,       /* Not get the gatt_if, so initial is ESP_GATT_IF_NONE */
    },
};

/*
 *  SPP PROFILE ATTRIBUTES
 ****************************************************************************************
 */

#define CHAR_DECLARATION_SIZE   (sizeof(uint8_t))
static const uint16_t primary_service_uuid = ESP_GATT_UUID_PRI_SERVICE;
static const uint16_t character_declaration_uuid = ESP_GATT_UUID_CHAR_DECLARE;
static const uint16_t character_client_config_uuid = ESP_GATT_UUID_CHAR_CLIENT_CONFIG;

static const uint8_t char_prop_read_notify = ESP_GATT_CHAR_PROP_BIT_READ|ESP_GATT_CHAR_PROP_BIT_NOTIFY;
static const uint8_t char_prop_read_write = ESP_GATT_CHAR_PROP_BIT_WRITE_NR|ESP_GATT_CHAR_PROP_BIT_READ;

#ifdef SUPPORT_HEARTBEAT
static const uint8_t char_prop_read_write_notify = ESP_GATT_CHAR_PROP_BIT_READ|ESP_GATT_CHAR_PROP_BIT_WRITE_NR|ESP_GATT_CHAR_PROP_BIT_NOTIFY;
#endif

///SPP Service - data receive characteristic, read&write without response
static const uint16_t spp_data_receive_uuid = ESP_GATT_UUID_SPP_DATA_RECEIVE;
static const uint8_t  spp_data_receive_val[20] = {0x01};

///SPP Service - data notify characteristic, notify&read
static const uint16_t spp_data_notify_uuid = ESP_GATT_UUID_SPP_DATA_NOTIFY;
static const uint8_t  spp_data_notify_val[20] = {0x30,0x31,0x32};
static const uint8_t  spp_data_notify_ccc[2] = {0x30, 0x31};

///SPP Service - command characteristic, read&write without response
static const uint16_t spp_command_uuid = ESP_GATT_UUID_SPP_COMMAND_RECEIVE;
static const uint8_t  spp_command_val[10] = {0x03};

///SPP Service - status characteristic, notify&read
static const uint16_t spp_status_uuid = ESP_GATT_UUID_SPP_COMMAND_NOTIFY;
static const uint8_t  spp_status_val[10] = {0x04};
static const uint8_t  spp_status_ccc[2] = {0x30,0x31,0x32};;

#ifdef SUPPORT_HEARTBEAT
///SPP Server - Heart beat characteristic, notify&write&read
static const uint16_t spp_heart_beat_uuid = ESP_GATT_UUID_SPP_HEARTBEAT;
static const uint8_t  spp_heart_beat_val[2] = {0x00, 0x00};
static const uint8_t  spp_heart_beat_ccc[2] = {0x00, 0x00};
#endif


/**********************************function*********************************************/
void uart_task(void *pvParameters);
void uart_recevive_task_create();
void uart_receive_task(void *pvParameters);



//esp_ble_gatts_send_indicate   TODO: 写数据 notify或indicate 
///Full HRS Database Description - Used to add attributes into the database
static const esp_gatts_attr_db_t spp_gatt_db[SPP_IDX_NB] =      //服务协议 characteristic
{

    //SPP -  Service Declaration
    [SPP_IDX_SVC]                      	=
    {{ESP_GATT_AUTO_RSP}, {ESP_UUID_LEN_16, (uint8_t *)&primary_service_uuid, ESP_GATT_PERM_READ,
    sizeof(spp_service_uuid), sizeof(spp_service_uuid), (uint8_t *)&spp_service_uuid}},

    //SPP -  data receive characteristic Declaration
    [SPP_IDX_SPP_DATA_RECV_CHAR]            =
    {{ESP_GATT_AUTO_RSP}, {ESP_UUID_LEN_16, (uint8_t *)&character_declaration_uuid, ESP_GATT_PERM_READ,
    CHAR_DECLARATION_SIZE,CHAR_DECLARATION_SIZE, (uint8_t *)&char_prop_read_write}},

    //SPP -  data receive characteristic Value  TODO:   UUID
    [SPP_IDX_SPP_DATA_RECV_VAL]             	=
    {{ESP_GATT_AUTO_RSP}, {ESP_UUID_LEN_16, (uint8_t *)&spp_data_receive_uuid, ESP_GATT_PERM_READ|ESP_GATT_PERM_WRITE,  //1
    SPP_DATA_MAX_LEN,sizeof(spp_data_receive_val), (uint8_t *)spp_data_receive_val}},

    //SPP -  data notify characteristic Declaration
    [SPP_IDX_SPP_DATA_NOTIFY_CHAR]  =
    {{ESP_GATT_AUTO_RSP}, {ESP_UUID_LEN_16, (uint8_t *)&character_declaration_uuid, ESP_GATT_PERM_READ,
    CHAR_DECLARATION_SIZE,CHAR_DECLARATION_SIZE, (uint8_t *)&char_prop_read_notify}},


    //SPP -  data notify characteristic Value
    [SPP_IDX_SPP_DATA_NTY_VAL]   =      //TODO: UUID
    {{ESP_GATT_AUTO_RSP}, {ESP_UUID_LEN_16, (uint8_t *)&spp_data_notify_uuid, ESP_GATT_PERM_READ,   //2
    SPP_DATA_MAX_LEN, sizeof(spp_data_notify_val), (uint8_t *)spp_data_notify_val}},


    //SPP -  data notify characteristic - Client Characteristic Configuration Descriptor
    [SPP_IDX_SPP_DATA_NTF_CFG]         =
    {{ESP_GATT_AUTO_RSP}, {ESP_UUID_LEN_16, (uint8_t *)&character_client_config_uuid, ESP_GATT_PERM_READ|ESP_GATT_PERM_WRITE,
    sizeof(uint16_t),sizeof(spp_data_notify_ccc), (uint8_t *)spp_data_notify_ccc}},

#if 0
    //SPP -  command characteristic Declaration
    [SPP_IDX_SPP_COMMAND_CHAR]            =
    {{ESP_GATT_AUTO_RSP}, {ESP_UUID_LEN_16, (uint8_t *)&character_declaration_uuid, ESP_GATT_PERM_READ,
    CHAR_DECLARATION_SIZE,CHAR_DECLARATION_SIZE, (uint8_t *)&char_prop_read_write}},

    //SPP -  command characteristic Value       TODO:   UUID
    [SPP_IDX_SPP_COMMAND_VAL]                 =
    {{ESP_GATT_AUTO_RSP}, {ESP_UUID_LEN_16, (uint8_t *)&spp_command_uuid, ESP_GATT_PERM_READ|ESP_GATT_PERM_WRITE,   //3
    SPP_CMD_MAX_LEN,sizeof(spp_command_val), (uint8_t *)spp_command_val}},


    //SPP -  status characteristic Declaration
    [SPP_IDX_SPP_STATUS_CHAR]            =
    {{ESP_GATT_AUTO_RSP}, {ESP_UUID_LEN_16, (uint8_t *)&character_declaration_uuid, ESP_GATT_PERM_READ,
    CHAR_DECLARATION_SIZE,CHAR_DECLARATION_SIZE, (uint8_t *)&char_prop_read_notify}},

    //SPP -  status characteristic Value        TODO:   UUID
    [SPP_IDX_SPP_STATUS_VAL]                 =
    {{ESP_GATT_AUTO_RSP}, {ESP_UUID_LEN_16, (uint8_t *)&spp_status_uuid, ESP_GATT_PERM_READ,    //4
    SPP_STATUS_MAX_LEN,sizeof(spp_status_val), (uint8_t *)spp_status_val}},

    //SPP -  status characteristic - Client Characteristic Configuration Descriptor
    [SPP_IDX_SPP_STATUS_CFG]         =
    {{ESP_GATT_AUTO_RSP}, {ESP_UUID_LEN_16, (uint8_t *)&character_client_config_uuid, ESP_GATT_PERM_READ|ESP_GATT_PERM_WRITE,
    sizeof(uint16_t),sizeof(spp_status_ccc), (uint8_t *)spp_status_ccc}},
#endif


#ifdef SUPPORT_HEARTBEAT
    //SPP -  Heart beat characteristic Declaration
    [SPP_IDX_SPP_HEARTBEAT_CHAR]  =
    {{ESP_GATT_AUTO_RSP}, {ESP_UUID_LEN_16, (uint8_t *)&character_declaration_uuid, ESP_GATT_PERM_READ,
    CHAR_DECLARATION_SIZE,CHAR_DECLARATION_SIZE, (uint8_t *)&char_prop_read_write_notify}},

    //SPP -  Heart beat characteristic Value
    [SPP_IDX_SPP_HEARTBEAT_VAL]   =
    {{ESP_GATT_AUTO_RSP}, {ESP_UUID_LEN_16, (uint8_t *)&spp_heart_beat_uuid, ESP_GATT_PERM_READ|ESP_GATT_PERM_WRITE,
    sizeof(spp_heart_beat_val), sizeof(spp_heart_beat_val), (uint8_t *)spp_heart_beat_val}},

    //SPP -  Heart beat characteristic - Client Characteristic Configuration Descriptor
    [SPP_IDX_SPP_HEARTBEAT_CFG]         =
    {{ESP_GATT_AUTO_RSP}, {ESP_UUID_LEN_16, (uint8_t *)&character_client_config_uuid, ESP_GATT_PERM_READ|ESP_GATT_PERM_WRITE,
    sizeof(uint16_t),sizeof(spp_data_notify_ccc), (uint8_t *)spp_heart_beat_ccc}},
#endif
};

static uint8_t find_char_and_desr_index(uint16_t handle)
{
    uint8_t error = 0xff;

    for(int i = 0; i < SPP_IDX_NB ; i++){
        if(handle == spp_handle_table[i]){
            return i;
        }
    }

    return error;
}

static bool store_wr_buffer(esp_ble_gatts_cb_param_t *p_data)
{
    temp_spp_recv_data_node_p1 = (spp_receive_data_node_t *)malloc(sizeof(spp_receive_data_node_t));

    if(temp_spp_recv_data_node_p1 == NULL){
        ESP_LOGI(GATTS_TABLE_TAG, "malloc error %s %d\n", __func__, __LINE__);
        return false;
    }
    if(temp_spp_recv_data_node_p2 != NULL){
        temp_spp_recv_data_node_p2->next_node = temp_spp_recv_data_node_p1;
    }
    temp_spp_recv_data_node_p1->len = p_data->write.len;
    SppRecvDataBuff.buff_size += p_data->write.len;
    temp_spp_recv_data_node_p1->next_node = NULL;
    temp_spp_recv_data_node_p1->node_buff = (uint8_t *)malloc(p_data->write.len);
    temp_spp_recv_data_node_p2 = temp_spp_recv_data_node_p1;
    memcpy(temp_spp_recv_data_node_p1->node_buff,p_data->write.value,p_data->write.len);
    if(SppRecvDataBuff.node_num == 0){
        SppRecvDataBuff.first_node = temp_spp_recv_data_node_p1;
        SppRecvDataBuff.node_num++;
    }else{
        SppRecvDataBuff.node_num++;
    }

    return true;
}

static void free_write_buffer(void)
{
    temp_spp_recv_data_node_p1 = SppRecvDataBuff.first_node;

    while(temp_spp_recv_data_node_p1 != NULL){
        temp_spp_recv_data_node_p2 = temp_spp_recv_data_node_p1->next_node;
        free(temp_spp_recv_data_node_p1->node_buff);
        free(temp_spp_recv_data_node_p1);
        temp_spp_recv_data_node_p1 = temp_spp_recv_data_node_p2;
    }

    SppRecvDataBuff.node_num = 0;
    SppRecvDataBuff.buff_size = 0;
    SppRecvDataBuff.first_node = NULL;
}

static void print_write_buffer(void)
{
    temp_spp_recv_data_node_p1 = SppRecvDataBuff.first_node;

    while(temp_spp_recv_data_node_p1 != NULL){
        uart_write_bytes(UART_NUM_0, (char *)(temp_spp_recv_data_node_p1->node_buff), temp_spp_recv_data_node_p1->len);
        temp_spp_recv_data_node_p1 = temp_spp_recv_data_node_p1->next_node;
    }
}

void ble_test_task(void *pvParameters)
{
    for(;;)
    {
        if(read_flag == 1)
        {
            //esp_ble_gatts_send_indicate(spp_gatts_if, spp_conn_id, spp_handle_table[SPP_IDX_SPP_DATA_NTY_VAL],strlen("aceinna_ble_test\n"), (uint8_t*)"aceinna_ble_test\n", false);
        }
        vTaskDelay(500 / portTICK_PERIOD_MS);
    }
}


static void spp_uart_init(void)
{
    uart_config_t uart_config = {
        //.baud_rate = 115200,

        .baud_rate = 460800,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_RTS,
        .rx_flow_ctrl_thresh = 122,
    };

    //Set UART parameters
    uart_param_config(UART_NUM_0, &uart_config);
    //Set UART pins
    uart_set_pin(UART_NUM_0, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    //Install UART driver, and get the queue.
    uart_driver_install(UART_NUM_0, 4096, 8192, 10,&spp_uart_queue,0);
    xTaskCreate(uart_task, "uTask", 2048, (void*)UART_NUM_0, 8, NULL);
    xTaskCreate(ble_test_task, "testTask", 2048, (void*)UART_NUM_0, 8, NULL);    
}

#ifdef SUPPORT_HEARTBEAT
void spp_heartbeat_task(void * arg)
{
    uint16_t cmd_id;

    for(;;) {
        vTaskDelay(50 / portTICK_PERIOD_MS);
        if(xQueueReceive(cmd_heartbeat_queue, &cmd_id, portMAX_DELAY)) {
            while(1){
                heartbeat_count_num++;
                vTaskDelay(5000/ portTICK_PERIOD_MS);
                if((heartbeat_count_num >3)&&(is_connected)){
                    esp_ble_gap_disconnect(spp_remote_bda);
                }
                if(is_connected && enable_heart_ntf){
                    esp_ble_gatts_send_indicate(spp_gatts_if, spp_conn_id, spp_handle_table[SPP_IDX_SPP_HEARTBEAT_VAL],sizeof(heartbeat_s), heartbeat_s, false);
                }else if(!is_connected){
                    break;
                }
            }
        }
    }
    vTaskDelete(NULL);
}
#endif

void spp_cmd_task(void * arg)
{
    uint8_t * cmd_id;

    for(;;){
        vTaskDelay(50 / portTICK_PERIOD_MS);
        if(xQueueReceive(cmd_cmd_queue, &cmd_id, portMAX_DELAY)) {
            esp_log_buffer_char(GATTS_TABLE_TAG,(char *)(cmd_id),strlen((char *)cmd_id));
            free(cmd_id);
        }
    }
    vTaskDelete(NULL);
}

static void spp_task_init(void)
{
    spp_uart_init();

#ifdef SUPPORT_HEARTBEAT
    cmd_heartbeat_queue = xQueueCreate(10, sizeof(uint32_t));
    xTaskCreate(spp_heartbeat_task, "spp_heartbeat_task", 2048, NULL, 10, NULL);
#endif

    cmd_cmd_queue = xQueueCreate(10, sizeof(uint32_t));
    xTaskCreate(spp_cmd_task, "spp_cmd_task", 2048, NULL, 10, NULL);
}

static void gap_event_handler(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param)
{
    esp_err_t err;
    ESP_LOGE(GATTS_TABLE_TAG, "GAP_EVT, event %d\n", event);

    switch (event) {
    case ESP_GAP_BLE_ADV_DATA_RAW_SET_COMPLETE_EVT:
        esp_ble_gap_start_advertising(&spp_adv_params);
        printf("test!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!\n");
        break;
    case ESP_GAP_BLE_ADV_START_COMPLETE_EVT:
        //advertising start complete event to indicate advertising start successfully or failed
        if((err = param->adv_start_cmpl.status) != ESP_BT_STATUS_SUCCESS) {
            ESP_LOGE(GATTS_TABLE_TAG, "Advertising start failed: %s\n", esp_err_to_name(err));
        }
        break;
    default:
        break;
    }
}

esp_ble_conn_update_params_t conn_params = {0};
//esp_err_t local_mtu_ret;
static void gatts_profile_event_handler(esp_gatts_cb_event_t event, esp_gatt_if_t gatts_if, esp_ble_gatts_cb_param_t *param)
{
    esp_ble_gatts_cb_param_t *p_data = (esp_ble_gatts_cb_param_t *) param;
    uint8_t res = 0xff;
    int ret = -1;
    //ESP_LOGI(GATTS_TABLE_TAG, "event = %x\n",event);    //TODO:连接
    switch (event) {
    	case ESP_GATTS_REG_EVT:     //广播
    	    ESP_LOGI(GATTS_TABLE_TAG, "%s %d\n", __func__, __LINE__);
        	ret = esp_ble_gap_set_device_name(SAMPLE_DEVICE_NAME);
            printf("ret = %d###################################\n",ret);
            //ble_init_adv_data(SAMPLE_DEVICE_NAME);          //TODO: 设备名称
        	ESP_LOGI(GATTS_TABLE_TAG, "%s %d\n", __func__, __LINE__);
        	esp_ble_gap_config_adv_data_raw((uint8_t *)spp_adv_data, sizeof(spp_adv_data)); //蓝牙名称
            printf("ESP_GATTS_REG_EVT!!!!!!!!!!!!!\n");

        	ESP_LOGI(GATTS_TABLE_TAG, "%s %d\n", __func__, __LINE__);
        	esp_ble_gatts_create_attr_tab(spp_gatt_db, gatts_if, SPP_IDX_NB, SPP_SVC_INST_ID);  //TODO:注册Characteristic 表
       	break;
    	case ESP_GATTS_READ_EVT:        //TODO: 客户端请求读操作 
            res = find_char_and_desr_index(p_data->read.handle);
            printf("ESP_GATTS_READ_EVT!!!!!!!!!!!!!!!!!!!!!!!!\n");
            esp_ble_gatts_send_indicate(spp_gatts_if, spp_conn_id, spp_handle_table[SPP_IDX_SPP_DATA_NTY_VAL],strlen("aceinna_ble_test\n"), (uint8_t*)"aceinna_ble_test\n", false);
//发送两个uuid
            esp_ble_gatts_send_indicate(spp_gatts_if, spp_conn_id, spp_handle_table[SPP_IDX_SPP_STATUS_VAL],strlen("aceinna_ble_test\n"), (uint8_t*)"aceinna_ble_test\n", false);
            if((res == SPP_IDX_SPP_STATUS_VAL) || (res ==SPP_IDX_SPP_DATA_NTY_VAL)){  //服务3
//            if((res ==SPP_IDX_SPP_DATA_NTY_VAL)){  //服务3
                read_flag = 1;
                //TODO:client read the status characteristic
                printf("ESP_GATTS_READ_EVT********************");
                                printf("res = %d\n",res);
            }
#if 0
            esp_gatt_rsp_t *gatt_rsp = (esp_gatt_rsp_t *)malloc(sizeof(esp_gatt_rsp_t));
            gatt_rsp->attr_value.len = param->write.len;
            gatt_rsp->attr_value.handle = param->write.handle;
            gatt_rsp->attr_value.offset = param->write.offset;
            gatt_rsp->attr_value.auth_req = ESP_GATT_AUTH_REQ_NONE;
            memcpy(gatt_rsp->attr_value.value, param->write.value, param->write.len);
            esp_err_t response_err = esp_ble_gatts_send_response(gatts_if, param->write.conn_id, param->write.trans_id, status, gatt_rsp);
#endif

       	 break;
    	case ESP_GATTS_WRITE_EVT: {         //TODO: 客户端请求写操作
    	    res = find_char_and_desr_index(p_data->write.handle);
            if(p_data->write.is_prep == false){
                ESP_LOGI(GATTS_TABLE_TAG, "ESP_GATTS_WRITE_EVT : handle = %d\n", res);
           
                if(res == SPP_IDX_SPP_COMMAND_VAL){
                    //if(0){
                    uint8_t * spp_cmd_buff = NULL;
                    spp_cmd_buff = (uint8_t *)malloc((spp_mtu_size - 3) * sizeof(uint8_t));
                    if(spp_cmd_buff == NULL){
                        ESP_LOGE(GATTS_TABLE_TAG, "%s malloc failed\n", __func__);
                        break;
                    }

                    memset(spp_cmd_buff,0x0,(spp_mtu_size - 3));
                    memcpy(spp_cmd_buff,p_data->write.value,p_data->write.len);
                    printf("p_data->write.value = %s\n",p_data->write.value);   //TODO:
                    xQueueSend(cmd_cmd_queue,&spp_cmd_buff,10/portTICK_PERIOD_MS);
                }else if(res == SPP_IDX_SPP_DATA_NTF_CFG){
                    if((p_data->write.len == 2)&&(p_data->write.value[0] == 0x01)&&(p_data->write.value[1] == 0x00)){
                        enable_data_ntf = true;
                        printf("00_enable_data_ntf = %d\n",enable_data_ntf);
                    }else if((p_data->write.len == 2)&&(p_data->write.value[0] == 0x00)&&(p_data->write.value[1] == 0x00)){
                        enable_data_ntf = false;
                        printf("01_enable_data_ntf = %d\n",enable_data_ntf);
                    }
                }
#if 0
                    uint16_t notify_en = 1;
                    esp_ble_gattc_write_char( gatts_if,
                                              p_data->connect.conn_id,
                                              (db+SPP_IDX_SPP_DATA_RECV_VAL)->attribute_handle,
                                              event.size,
                                              temp,
                                              ESP_GATT_WRITE_TYPE_RSP,
                                              ESP_GATT_AUTH_REQ_NONE);
#endif                                                              
#ifdef SUPPORT_HEARTBEAT
                else if(res == SPP_IDX_SPP_HEARTBEAT_CFG){
                    if((p_data->write.len == 2)&&(p_data->write.value[0] == 0x01)&&(p_data->write.value[1] == 0x00)){
                        enable_heart_ntf = true;
                    }else if((p_data->write.len == 2)&&(p_data->write.value[0] == 0x00)&&(p_data->write.value[1] == 0x00)){
                        enable_heart_ntf = false;
                    }
                }else if(res == SPP_IDX_SPP_HEARTBEAT_VAL){
                    if((p_data->write.len == sizeof(heartbeat_s))&&(memcmp(heartbeat_s,p_data->write.value,sizeof(heartbeat_s)) == 0)){
                        heartbeat_count_num = 0;
                    }
                }
#endif
                else if(res == SPP_IDX_SPP_DATA_RECV_VAL){
#ifdef SPP_DEBUG_MODE
                    esp_log_buffer_char(GATTS_TABLE_TAG,(char *)(p_data->write.value),p_data->write.len);
#else
                    uart_write_bytes(UART_NUM_0, (char *)(p_data->write.value), p_data->write.len);
#endif
                }else{
                    //TODO:
                }
            }else if((p_data->write.is_prep == true)&&(res == SPP_IDX_SPP_DATA_RECV_VAL)){
                ESP_LOGI(GATTS_TABLE_TAG, "ESP_GATTS_PREP_WRITE_EVT : handle = %d\n", res);
                store_wr_buffer(p_data);
            }
      	 	break;
    	}
    	case ESP_GATTS_EXEC_WRITE_EVT:{
    	    ESP_LOGI(GATTS_TABLE_TAG, "ESP_GATTS_EXEC_WRITE_EVT\n");
    	    if(p_data->exec_write.exec_write_flag){
    	        print_write_buffer();
    	        free_write_buffer();
    	    }
    	    break;
    	}
    	case ESP_GATTS_MTU_EVT:
    	    spp_mtu_size = p_data->mtu.mtu;
            printf("set mtu_size#####################\n");
    	    break;
    	case ESP_GATTS_CONF_EVT:
    	    break;
    	case ESP_GATTS_UNREG_EVT:
        	break;
    	case ESP_GATTS_DELETE_EVT:
        	break;
    	case ESP_GATTS_START_EVT:
        	break;
    	case ESP_GATTS_STOP_EVT:
        	break;
    	case ESP_GATTS_CONNECT_EVT:         //TODO:连接事件
#if 0 //TODO: 

            memcpy(conn_params.bda, param->connect.remote_bda, sizeof(esp_bd_addr_t));
            /* For the IOS system, please reference the apple official documents about the ble connection parameters restrictions. */
            conn_params.latency = 0;
            conn_params.max_int = 0x5;    // max_int = 0x20*1.25ms = 40ms
            conn_params.min_int = 0x2;    // min_int = 0x10*1.25ms = 20ms
            conn_params.timeout = 400;    // timeout = 400*10ms = 4000ms
            ESP_LOGI(GATTS_TABLE_TAG, "ESP_GATTS_CONNECT_EVT, conn_id %d, remote %02x:%02x:%02x:%02x:%02x:%02x:",
                    param->connect.conn_id,
                    param->connect.remote_bda[0], param->connect.remote_bda[1], param->connect.remote_bda[2],
                    param->connect.remote_bda[3], param->connect.remote_bda[4], param->connect.remote_bda[5]);

            //start sent the update connection parameters to the peer device.
            esp_ble_gap_update_conn_params(&conn_params);
            local_mtu_ret = esp_ble_gatt_set_local_mtu(spp_mtu_size);
            if (local_mtu_ret){
                ESP_LOGE(GATTS_TABLE_TAG, "set local  MTU failed, error code = %x", local_mtu_ret);
            }
#endif
    	    spp_conn_id = p_data->connect.conn_id;      //获取连接ble id
            printf("connect***************************\n");
    	    spp_gatts_if = gatts_if;
    	    is_connected = true;
    	    memcpy(&spp_remote_bda,&p_data->connect.remote_bda,sizeof(esp_bd_addr_t));
#ifdef SUPPORT_HEARTBEAT
    	    uint16_t cmd = 0;
            xQueueSend(cmd_heartbeat_queue,&cmd,10/portTICK_PERIOD_MS);
#endif
        	break;
    	case ESP_GATTS_DISCONNECT_EVT:
    	    is_connected = false;
    	    enable_data_ntf = false;
            //enable_data_ntf = true;
            printf("02_enable_data_ntf = %d\n",enable_data_ntf);
#ifdef SUPPORT_HEARTBEAT
    	    enable_heart_ntf = false;
    	    heartbeat_count_num = 0;
#endif
    	    esp_ble_gap_start_advertising(&spp_adv_params);
    	    break;
    	case ESP_GATTS_OPEN_EVT:
    	    break;
    	case ESP_GATTS_CANCEL_OPEN_EVT:
    	    break;
    	case ESP_GATTS_CLOSE_EVT:
    	    break;
    	case ESP_GATTS_LISTEN_EVT:
    	    break;
    	case ESP_GATTS_CONGEST_EVT:
    	    break;
    	case ESP_GATTS_CREAT_ATTR_TAB_EVT:{     //GATT建表完成
    	    ESP_LOGI(GATTS_TABLE_TAG, "The number handle =%x\n",param->add_attr_tab.num_handle);
    	    if (param->add_attr_tab.status != ESP_GATT_OK){
    	        ESP_LOGE(GATTS_TABLE_TAG, "Create attribute table failed, error code=0x%x", param->add_attr_tab.status);
    	    }
    	    else if (param->add_attr_tab.num_handle != SPP_IDX_NB){
    	        ESP_LOGE(GATTS_TABLE_TAG, "Create attribute table abnormally, num_handle (%d) doesn't equal to HRS_IDX_NB(%d)", param->add_attr_tab.num_handle, SPP_IDX_NB);
    	    }
    	    else {
    	        memcpy(spp_handle_table, param->add_attr_tab.handles, sizeof(spp_handle_table));
                printf("caeate spp_handle_table******************************\n");
    	        esp_ble_gatts_start_service(spp_handle_table[SPP_IDX_SVC]);
    	    }
    	    break;
    	}
    	default:
    	    break;
    }
}


static void gatts_event_handler(esp_gatts_cb_event_t event, esp_gatt_if_t gatts_if, esp_ble_gatts_cb_param_t *param)
{
    //ESP_LOGI(GATTS_TABLE_TAG, "EVT %d, gatts if %d\n", event, gatts_if);  //TODO:
    //连接 GATTS_SPP_DEMO: EVT 14, gatts if 3
    /* If event is register event, store the gatts_if for each profile */
    if (event == ESP_GATTS_REG_EVT) {
        if (param->reg.status == ESP_GATT_OK) {
            spp_profile_tab[SPP_PROFILE_APP_IDX].gatts_if = gatts_if;
        } else {
            ESP_LOGI(GATTS_TABLE_TAG, "Reg app failed, app_id %04x, status %d\n",param->reg.app_id, param->reg.status);
            return;
        }
    }

    do {
        int idx;
        for (idx = 0; idx < SPP_PROFILE_NUM; idx++) {
            if (gatts_if == ESP_GATT_IF_NONE || /* ESP_GATT_IF_NONE, not specify a certain gatt_if, need to call every profile cb function */
                    gatts_if == spp_profile_tab[idx].gatts_if) {
                if (spp_profile_tab[idx].gatts_cb) {
                    spp_profile_tab[idx].gatts_cb(event, gatts_if, param);
                }
            }
        }
    } while (0);
}

void app_main()
{
    esp_err_t ret;
    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();

    // Initialize NVS
    ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK( ret );

    ESP_ERROR_CHECK(esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT));

    ret = esp_bt_controller_init(&bt_cfg);
    if (ret) {
        ESP_LOGE(GATTS_TABLE_TAG, "%s enable controller failed: %s\n", __func__, esp_err_to_name(ret));
        return;
    }

    ret = esp_bt_controller_enable(ESP_BT_MODE_BLE);
    if (ret) {
        ESP_LOGE(GATTS_TABLE_TAG, "%s enable controller failed: %s\n", __func__, esp_err_to_name(ret));
        return;
    }

    ESP_LOGI(GATTS_TABLE_TAG, "%s init bluetooth\n", __func__);
    ret = esp_bluedroid_init();
    if (ret) {
        ESP_LOGE(GATTS_TABLE_TAG, "%s init bluetooth failed: %s\n", __func__, esp_err_to_name(ret));
        return;
    }
    ret = esp_bluedroid_enable();
    if (ret) {
        ESP_LOGE(GATTS_TABLE_TAG, "%s enable bluetooth failed: %s\n", __func__, esp_err_to_name(ret));
        return;
    }

    esp_ble_gatts_register_callback(gatts_event_handler);       //应用程序回调
    esp_ble_gap_register_callback(gap_event_handler);       //gap 事件回调
    esp_ble_gatts_app_register(ESP_SPP_APP_ID);     //应用程序标识符

    spp_task_init();
	uart_recevive_task_create();

#if 0
    esp_err_t local_mtu_ret = esp_ble_gatt_set_local_mtu(spp_mtu_size);     //TODO:此段代码无法设置MTU？原因?
    if (local_mtu_ret){
        ESP_LOGE(GATTS_TABLE_TAG, "set local  MTU failed, error code = %x", local_mtu_ret);
    }
    else {
        printf("set local mtu\n");
    }
#endif
    return;
}



void uart_recevive_task_create()
{
    int uart_num = UART_CHANNEL;
	//uart_init(uart_num);
    xTaskCreate(uart_task, "uart_task", 2048 * 5, (void*)uart_num, 5, NULL);
    xTaskCreate(uart_receive_task, "uart_receive_task", 2048 * 5, (void*)uart_num, 10, NULL);
    //process data
}


void uart_receive_task(void *pvParameters)  // TODO: uart接收
{
    uint8_t total_num = 0;
    uint8_t current_num = 0;
    int uart_num = (int) pvParameters;
	unsigned char* uart_data = (uint8_t*) malloc(BUF_SIZE * 2);
	signed int Write_handle = 0;
    uint8_t * ntf_value_p = NULL;
    uint8_t* temp = (uint8_t*) malloc(BUF_SIZE * 2);
    memset(temp,0x0,BUF_SIZE * 2);
	for(;;)
	{
#if 1
		//int len = uart_read_bytes(uart_num, uart_data, BUF_SIZE, 10/ portTICK_RATE_MS);
        int len = uart_read_bytes(UART_NUM_0,temp,BUF_SIZE,10/ portTICK_RATE_MS);
		if(len > 0)
		{
            //printf("len = %d\n",len);
        
#if 1
#ifdef SUPPORT_HEARTBEAT
            if(!enable_heart_ntf){
                ESP_LOGE(GATTS_TABLE_TAG, "%s do not enable heartbeat Notify\n", __func__);
                continue;
            }
#endif
            if(!enable_data_ntf){
                //ESP_LOGE(GATTS_TABLE_TAG, "%s do not enable data Notify\n", __func__);
                continue;
            }
            //temp = (uint8_t *)malloc(sizeof(uint8_t)*len);
            if(temp == NULL){
                ESP_LOGE(GATTS_TABLE_TAG, "%s malloc.1 failed\n", __func__);
                continue;
            }
            //memset(temp,0x0,len); //test
            //uart_read_bytes(UART_NUM_0,temp,event.size,portMAX_DELAY);
            //esp_ble_gatts_send_indicate(spp_gatts_if, spp_conn_id, spp_handle_table[SPP_IDX_SPP_DATA_NTY_VAL],(event.size - (current_num - 1)*(spp_mtu_size - 7) + 4), ntf_value_p, false);
            if(len <= (spp_mtu_size - 3)){
                esp_ble_gatts_send_indicate(spp_gatts_if, spp_conn_id, spp_handle_table[SPP_IDX_SPP_DATA_NTY_VAL],len, temp, false); //TODO:
                printf("spp_mtu_size111 = %d\n",spp_mtu_size);
            }else if(len > (spp_mtu_size - 3)){
                if((len%(spp_mtu_size - 4)) == 0){
                    total_num = len/(spp_mtu_size - 4);
                }else{
                    total_num = len/(spp_mtu_size - 4) + 1;
                }
                current_num = 1;
                ntf_value_p = (uint8_t *)malloc((spp_mtu_size-3)*sizeof(uint8_t));
                if(ntf_value_p == NULL){
                    ESP_LOGE(GATTS_TABLE_TAG, "%s malloc.2 failed\n", __func__);
                    free(temp);
                    continue;
                }
                while(current_num <= total_num){
                    if(current_num < total_num){
                        ntf_value_p[0] = ':';
                        //ntf_value_p[1] = ':';
                        //ntf_value_p[2] = total_num;
                        //ntf_value_p[3] = current_num;
                        memcpy(ntf_value_p + 1,temp + (current_num - 1)*(spp_mtu_size-4),(spp_mtu_size-4));
                        esp_ble_gatts_send_indicate(spp_gatts_if, spp_conn_id, spp_handle_table[SPP_IDX_SPP_DATA_NTY_VAL],(spp_mtu_size-3), ntf_value_p, false);
                    }else if(current_num == total_num){
                        ntf_value_p[0] = ':';
                        //ntf_value_p[1] = ':';
                        //ntf_value_p[2] = total_num;
                        //ntf_value_p[3] = current_num;
                        memcpy(ntf_value_p + 1,temp + (current_num - 1)*(spp_mtu_size-4),(len - (current_num - 1)*(spp_mtu_size - 4)));
                        esp_ble_gatts_send_indicate(spp_gatts_if, spp_conn_id, spp_handle_table[SPP_IDX_SPP_DATA_NTY_VAL],(len - (current_num - 1)*(spp_mtu_size - 4) + 1), ntf_value_p, false);
                    }
                    //printf("total_num = %d\n",total_num);
                    //printf("current_num = %d\n",current_num);
                    //vTaskDelay(20 / portTICK_PERIOD_MS);
                    current_num++;
                }
                free(ntf_value_p);
            }

#endif        
		}
		//uart_write_bytes(uart_num, (const char*)(data), sizeof(data));

#endif
	}
	free(uart_data);
	uart_data = NULL;
	vTaskDelete(NULL);
    free(temp);
}
/*
        rsp.attr_value.handle = param->read.handle;
        rsp.attr_value.len = 4;
        rsp.attr_value.value[0] = 0xde;
        rsp.attr_value.value[1] = 0xed;
        rsp.attr_value.value[2] = 0xbe;
        rsp.attr_value.value[3] = 0xef;
        esp_ble_gatts_send_response(gatts_if, param->read.conn_id, param->read.trans_id,
                                    ESP_GATT_OK, &rsp);
*/

void uart_task(void *pvParameters)
{
    uart_event_t event;
    uint8_t total_num = 0;
    uint8_t current_num = 0;

    for (;;) {
        //Waiting for UART event.
        if (xQueueReceive(spp_uart_queue, (void * )&event, (portTickType)portMAX_DELAY)) {
            //printf("uart_test*************************************\n");
            switch (event.type) {
            //Event of UART receving data
            case UART_DATA:
#if 0            
                if ((event.size)&&(is_connected)) {
                    uint8_t * temp = NULL;
                    uint8_t * ntf_value_p = NULL;
#ifdef SUPPORT_HEARTBEAT
                    if(!enable_heart_ntf){
                        ESP_LOGE(GATTS_TABLE_TAG, "%s do not enable heartbeat Notify\n", __func__);
                        break;
                    }
#endif
                    if(!enable_data_ntf){
                        ESP_LOGE(GATTS_TABLE_TAG, "%s do not enable data Notify\n", __func__);
                        break;
                    }
                    temp = (uint8_t *)malloc(sizeof(uint8_t)*event.size);
                    if(temp == NULL){
                        ESP_LOGE(GATTS_TABLE_TAG, "%s malloc.1 failed\n", __func__);
                        break;
                    }
                    memset(temp,0x0,event.size);
                    uart_read_bytes(UART_NUM_0,temp,event.size,portMAX_DELAY);
                    //esp_ble_gatts_send_indicate(spp_gatts_if, spp_conn_id, spp_handle_table[SPP_IDX_SPP_DATA_NTY_VAL],(event.size - (current_num - 1)*(spp_mtu_size - 7) + 4), ntf_value_p, false);
                    if(event.size <= (spp_mtu_size - 3)){
                        esp_ble_gatts_send_indicate(spp_gatts_if, spp_conn_id, spp_handle_table[SPP_IDX_SPP_DATA_NTY_VAL],event.size, temp, false); //TODO:
                        printf("spp_mtu_size = %d\n",spp_mtu_size);
                    }else if(event.size > (spp_mtu_size - 3)){
                        if((event.size%(spp_mtu_size - 4)) == 0){
                            total_num = event.size/(spp_mtu_size - 4);
                        }else{
                            total_num = event.size/(spp_mtu_size - 4) + 1;
                        }
                        current_num = 1;
                        ntf_value_p = (uint8_t *)malloc((spp_mtu_size-3)*sizeof(uint8_t));
                        if(ntf_value_p == NULL){
                            ESP_LOGE(GATTS_TABLE_TAG, "%s malloc.2 failed\n", __func__);
                            free(temp);
                            break;
                        }
                        while(current_num <= total_num){
                            if(current_num < total_num){
                                ntf_value_p[0] = ':';
                                //ntf_value_p[1] = ':';
                                //ntf_value_p[2] = total_num;
                                //ntf_value_p[3] = current_num;
                                memcpy(ntf_value_p + 1,temp + (current_num - 1)*(spp_mtu_size-4),(spp_mtu_size-4));
                                esp_ble_gatts_send_indicate(spp_gatts_if, spp_conn_id, spp_handle_table[SPP_IDX_SPP_DATA_NTY_VAL],(spp_mtu_size-3), ntf_value_p, false);
                            }else if(current_num == total_num){
                                ntf_value_p[0] = ':';
                                //ntf_value_p[1] = ':';
                                //ntf_value_p[2] = total_num;
                                //ntf_value_p[3] = current_num;
                                memcpy(ntf_value_p + 1,temp + (current_num - 1)*(spp_mtu_size-4),(event.size - (current_num - 1)*(spp_mtu_size - 4)));
                                esp_ble_gatts_send_indicate(spp_gatts_if, spp_conn_id, spp_handle_table[SPP_IDX_SPP_DATA_NTY_VAL],(event.size - (current_num - 1)*(spp_mtu_size - 4) + 4), ntf_value_p, false);
                            }
                            //vTaskDelay(20 / portTICK_PERIOD_MS);
                            current_num++;
                        }
                        free(ntf_value_p);
                    }
                    free(temp);
                }
#endif
                break;
            default:
                break;
            }
        }
    }
    vTaskDelete(NULL);
}
