/*******************************************************************************
* File Name          : rtk_bt.c
* Author             : Daich
* Revision           : 1.0
* Date               : 13/05/2019
* Description        : bluetooth
*
* HISTORY***********************************************************************
* 13/05/2019  |                                             | Daich
*
*******************************************************************************/
#include "rtk_stdint.h"
#include "rtk_bt.h"
#include "tool.h"
#include "rtk_uart.h"
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_gap_bt_api.h"
#include "esp_bt_device.h"
#include "esp_spp_api.h"

#include <string.h>
#include "main.h"
//extern esp_spp_cb_param_t bt_spp_s;
//#define SEND_COUNT


extern esp_spp_cb_param_t* get_esp_spp_cb_param_t();
bt_manage_s esp_bt_manage_s;
extern esp_spp_cb_param_t bt_spp_s;
uint8_t type[2] = {0x00,0x01};
void bt_write(u32 handle,int len, unsigned char *p_data)
{

    s32 new_handle;
	s32 true_handle;
    u8* send_data;
    int send_len;
    get_ble_new_write_handle(&new_handle);
	get_ble_write_handle(&true_handle,handle);
#ifdef SEND_COUNT

    send_data = (unsigned char*) malloc(sizeof(char) * (len + 5));
    *(send_data) = ':';
    *(send_data + 1) = len / 256;
    *(send_data + 2) = len % 256;
    *(send_data + 3) = type[0];
    *(send_data + 4) = type[1];
    memcpy(send_data + 5,p_data,len);
    send_len = len + 5;
    if(get_this_bt_state(new_handle) == BT_CONNECT)
    {
        take_bt_mutex();
        esp_spp_write(true_handle, send_len, send_data);
        release_bt_mutex();
    }
    else
    {
    	DbgPrintf("bt disconnect\n");
    }
    free(send_data);

#else
	//printf("new_handle = %d,handle = %d,true_handle = %d\n",new_handle,handle,true_handle);
    //if(get_this_bt_state(new_handle) == BT_CONNECT)
    if(get_this_bt_state(true_handle) == BT_CONNECT)
    {
		//if(bt_spp_s->srv_open.status == ESP_SPP_SUCCESS)
		{
    		//esp_spp_write(bt_spp_s->srv_open.handle, len, p_data);
    		take_bt_mutex();
    		//esp_spp_write(new_handle, len, p_data);
			esp_spp_write(true_handle, len, p_data);
    		release_bt_mutex();
    		
		}
    }
    else
    {
    	DbgPrintf("bt disconnect\n");
    }
#endif    
}

int init_mul_bt(void)
{
	memset(&esp_bt_manage_s,0,sizeof(esp_bt_manage_s));
	esp_bt_manage_s.addr_len = ESP_BD_ADDR_LEN;
	return ESP_ERR_NONE;
}
int destroy_a_bt(void)
{
	return 1;
}

bt_handle_t add_bt(int state,esp_spp_cb_param_t* bt_info)
{
	int i=0;
	DbgPrintf("add bt\n");
	DbgPrintf("1add bt bt_info->srv_open.handle = %d\n",bt_info->srv_open.handle);
	for(i=0;i < MAX_BT_COUNT;i++)
	{
		for(int j = 0;j < ESP_BD_ADDR_LEN;j++)
		{
			//DbgPrintf("bt_info->srv_open.rem_bda[%d] = %x\n",j,bt_info->srv_open.rem_bda[j]);
		}
		for(int j = 0; j < ESP_BD_ADDR_LEN;j++)
		{
			//DbgPrintf("esp_bt_manage_s.sev_addr[%d] = %x\n",j,esp_bt_manage_s.bt_info[i].sev_addr[j]);
		}
		if(array_cmp(esp_bt_manage_s.bt_info[i].sev_addr,bt_info->srv_open.rem_bda,ESP_BD_ADDR_LEN,sizeof(char)) == 1)
		{
			
			esp_bt_manage_s.bt_info[i].bt_handle = bt_info->srv_open.handle;
			DbgPrintf("2add bt bt_info->srv_open.handle = %d\n",bt_info->srv_open.handle);
			DbgPrintf("111esp_bt_manage_s.bt_info[%d].bt_handle = %d\n",i,esp_bt_manage_s.bt_info[i].bt_handle);
			DbgPrintf("111esp_bt_manage_s.bt_info[%d].bt_handle = %d\n",i+1,esp_bt_manage_s.bt_info[i+1].bt_handle);
			esp_bt_manage_s.bt_info[i].state = BT_CONNECT;
			DbgPrintf("2add bt esp_bt_manage_s.bt_info[%d].bt_handle = %d\n",i,esp_bt_manage_s.bt_info[i].bt_handle);
			esp_bt_manage_s.new_handle = i;
			DbgPrintf("111esp_bt_manage_s.bt_info[%d].state = %d\n",i,esp_bt_manage_s.bt_info[i].state);
			break;
		}
	}
	if(i == MAX_BT_COUNT)
	{
		for(i=0;i<MAX_BT_COUNT;i++)
		{
			if(esp_bt_manage_s.bt_info[i].state == BT_NONE)
			{
				esp_bt_manage_s.bt_info[i].bt_handle = bt_info->srv_open.handle;
				memcpy(esp_bt_manage_s.bt_info[i].sev_addr,bt_info->srv_open.rem_bda,sizeof(esp_bt_manage_s.bt_info[i].sev_addr));
				esp_bt_manage_s.bt_info[i].state = BT_CONNECT;
				DbgPrintf("222esp_bt_manage_s.bt_info[%d].state = %d\n",i,esp_bt_manage_s.bt_info[i].state);
				for(int j=0;j<ESP_BD_ADDR_LEN;j++)
				{
					DbgPrintf("333esp_bt_manage_s.sev_addr[%d] = %x\n",j,esp_bt_manage_s.bt_info[i].sev_addr[j]);
				}
				esp_bt_manage_s.bt_num++;
				//printf("111esp_bt_manage_s.bt_num = %d\n",esp_bt_manage_s.bt_num);
				esp_bt_manage_s.new_handle = i;
				DbgPrintf("777777777777esp_bt_manage_s.new_handle = %d",esp_bt_manage_s.new_handle);
				break;
			}
		}
	}
	DbgPrintf("esp_bt_manage_s.bt_num = %d\n",esp_bt_manage_s.bt_num);
	//printf("esp_bt_manage_s.bt_num = %d\n",esp_bt_manage_s.bt_num);
	//set_bt_state(bt_info,state);
	return i;             //虚拟handle
}

void set_bt_state(esp_spp_cb_param_t* bt_info,BT_STAT_E bt_state,esp_spp_cb_event_t event)     //通过关闭事件的结构体判断哪个蓝牙关闭
{
#if 1
	int i = 0;
	switch(event)
	{
		case ESP_SPP_SRV_OPEN_EVT:
			for(i=0;i < MAX_BT_COUNT;i++)
			{	
				if(array_cmp(esp_bt_manage_s.bt_info[i].sev_addr,\
				bt_info->srv_open.rem_bda,ESP_BD_ADDR_LEN,sizeof(char)) == 1)
				{
					esp_bt_manage_s.bt_info[i].state = bt_state;
					DbgPrintf("44444444 esp_bt_manage_s.bt_info[%d].state = %d\n",i,esp_bt_manage_s.bt_info[i].state);
				}
			}
			break;
		case ESP_SPP_CLOSE_EVT:
			for(i=0;i < MAX_BT_COUNT;i++)
			{	
				if(esp_bt_manage_s.bt_info[i].bt_handle == bt_info->close.handle)
				{
					esp_bt_manage_s.bt_info[i].state = bt_state;
					DbgPrintf("55555555 esp_bt_manage_s.bt_info[%d].state = %d\n",i,esp_bt_manage_s.bt_info[i].state);
				}
			}
			break;
		default:
			break;
	}
#endif

	//esp_bt_manage_s.bt_info[i].state = bt_state;
}

BT_STAT_E get_this_bt_state(bt_handle_t bt_handle)       //find none
{
	int get_handle = -1;
	for(int i = 0;i < MAX_BT_COUNT;i++)
	{
		if(esp_bt_manage_s.bt_info[i].bt_handle == bt_handle)
		{
			get_handle = i;
			break;
		}
	}
	if(get_handle >= 0)
	{
		//DbgPrintf("find esp_bt_manage_s.bt_info[%d].state = %d\n",get_handle,esp_bt_manage_s.bt_info[get_handle].state);
		return esp_bt_manage_s.bt_info[get_handle].state;
	}
	else
	{
		return BT_NONE;
	}
}



void set_ble_new_write_handle(esp_spp_cb_param_t* des,esp_spp_cb_param_t* src)
{
	//write_handle = ble_write_handle->srv_open.handle;
	memcpy(des,src,sizeof(esp_spp_cb_param_t));	
	//DbgPrintf("write_handle = %d\n",write_handle);
}


int get_ble_new_write_handle(s32* w_handle)                        //获取实际句柄
{
	*w_handle = esp_bt_manage_s.bt_info[esp_bt_manage_s.new_handle].bt_handle;
	
	return true;
}


int get_ble_write_handle(s32* w_handle,int virtuau_handle)                        //获取实际句柄
{
	*w_handle = esp_bt_manage_s.bt_info[virtuau_handle].bt_handle;
	
	return true;
}


int get_ble_num()                        //获取蓝牙数量
{	
	return esp_bt_manage_s.bt_num;
}


bool get_name_from_eir(uint8_t *eir, char *bdname, uint8_t *bdname_len)
{
    uint8_t *rmt_bdname = NULL;
    uint8_t rmt_bdname_len = 0;

    if (!eir) {
        return false;
    }

    rmt_bdname = esp_bt_gap_resolve_eir_data(eir, ESP_BT_EIR_TYPE_CMPL_LOCAL_NAME, &rmt_bdname_len);
    if (!rmt_bdname) {
        rmt_bdname = esp_bt_gap_resolve_eir_data(eir, ESP_BT_EIR_TYPE_SHORT_LOCAL_NAME, &rmt_bdname_len);
    }

    if (rmt_bdname) {
        if (rmt_bdname_len > ESP_BT_GAP_MAX_BDNAME_LEN) {
            rmt_bdname_len = ESP_BT_GAP_MAX_BDNAME_LEN;
        }

        if (bdname) {
            memcpy(bdname, rmt_bdname, rmt_bdname_len);
            bdname[rmt_bdname_len] = '\0';
        }
        if (bdname_len) {
            *bdname_len = rmt_bdname_len;
        }
        return true;
    }

    return false;
}



