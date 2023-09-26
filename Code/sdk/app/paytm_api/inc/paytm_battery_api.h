#ifndef PAYTM_BATTERY_API_H
#define PAYTM_BATTERY_API_H

#include "paytm_sys_api.h"
#include "paytm_typedef.h"

typedef enum battery_charging_userstate_t{
    STATE_BATTERY_CHARGER_DISCONNECTED = 0,
    STATE_BATTERY_CHARGER_CONNECTED
};

int32 Paytm_GetBatteryVoltage(uint16 *voltage);
uint8 Paytm_GetBatteryLevel(void);
uint8 Paytm_GetChargingStatus(void);
int32 Paytm_Battery_Initialise(uint16 sample_interval_sec);
int32 Paytm_BatteryLevelMonitoring(uint8 enable);

//0-PLUGOUT  1-CHARGING  2-FULL  3-OVERHEATING
void battery_charging_report_callback_register(message_callback_t cb);

#endif