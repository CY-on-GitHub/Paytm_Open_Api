#ifndef PAYTM_BATTERY_API_H
#define PAYTM_BATTERY_API_H

#include "paytm_sys_api.h"
#include "paytm_typedef.h"

typedef enum {
    STATE_BATTERY_CHARGER_DISCONNECTED = 0,
    STATE_BATTERY_CHARGER_CONNECTED
}battery_charging_userstate_t;

int32 Paytm_GetBatteryVoltage(uint16 *voltage);

/**
 * @description: 
 * @return {*}      return battery level percentage
 */
uint8 Paytm_GetBatteryLevel(void);

/**
 * @description: 
 * @return {*}      true:charging, false:not charging
 */
uint8 Paytm_GetChargingStatus(void);

/**
 * @description: Please not call this function in paytm application, we will do this in kernel code
 *                  The default sample_interval_sec is 500ms
 * @param {uint16} gap_num 1 gap = 500ms
 * @return {*}  0-success, 1-already init
 */
int32 Paytm_Battery_Initialise(uint16 gap_num);
int32 Paytm_BatteryLevelMonitoring(uint8 enable);

/**
 * @description: 
 * @return {*} true:fully charge; false:not fully charge
 */
bool Paytm_GetChargingFullStatus(void);

//0-PLUGOUT  1-CHARGING  2-FULL  3-OVERHEATING
void battery_charging_report_callback_register(message_callback_t cb);

#endif