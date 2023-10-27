#include "paytm_dev_info_api.h"
#include "paytm_net_api.h"
#include "paytm_sim_api.h"
#include "paytm_typedef.h"

#include <stdlib.h>
#include <stdio.h>
#include "osi_api.h"

void simCb(void * p)
{
    Paytm_TRACE("This is sim cb, val = ");
}

void gprsCb(void * p)
{
    Paytm_TRACE("This is gprs cb, val = ");
}

void net_status_task(void* p)
{
    
}

void testNetWorkReconected(void)
{

    Paytm_GPRS_Connect(Paytm__IPVERSION_IPV4, NULL);

    network_state_callback_register(simCb);

    gprs_state_callback_register(gprsCb);

    Paytm_TRACE("Paytm_GPRS_Disconnect");
    osiThreadSleep(15000 / 5);
    Paytm_GPRS_Disconnect();

    osiThreadSleep(15000 / 5);
    Paytm_TRACE("Paytm_GPRS_Reconnect");
    Paytm_GPRS_Connect(Paytm__IPVERSION_IPV4, NULL);


    char op[32] = {0};
    Paytm_GetOperator(op, 32);
    Paytm_TRACE("op = %s", op);
}

void testNetWorkDisconected(void)
{
    Paytm_GPRS_Connect(Paytm__IPVERSION_IPV4, NULL);

    network_state_callback_register(simCb);

    gprs_state_callback_register(gprsCb);

    Paytm_TRACE("Paytm_GPRS_Disconnect");
    osiThreadSleep(15000 / 5);
    Paytm_GPRS_Disconnect();
}

void simMonitor(void * p)
{
    int result = *(int*)p;

    RTI_LOG1(">>>>");
    if(result == URC_SIM_REMOVED)
    {
        Paytm_TRACE("Sim card ejected");
    }else if(result == URC_SIM_INSERTED){
        Paytm_TRACE("Sim card inserted");
    }else if(result == URC_SIM_EJECTED_FOR_LONG_TIME){
        Paytm_TRACE("Sim card ejected for 15 minutes");
    }else if(result == URC_PDP_ACTIVE )
    {
        Paytm_TRACE("The pdp is active");
    }else if(result == URC_NET_ACTIVE ){
        Paytm_TRACE("The net connected successfully");
    }else if(result == URC_NET_DISCONNECTED ){
        Paytm_TRACE("The net is disconected");
    }else if(result == URC_SIM_READY){
        Paytm_TRACE("The sim card is ready");
    }
}

void testSim(void)
{
    Paytm_GPRS_Connect(Paytm__IPVERSION_IPV4, NULL);

    Paytm_GetModemFunction(simMonitor);

    while (!Paytm_Net_IsConnected())
    {
        Paytm_TRACE("Networking connecting");
        Paytm_delayMilliSeconds(2000);
    }

    char imsi[33] = {0};
    Paytm_ReadIMSI(imsi);
    Paytm_TRACE("IMSI = %s", imsi);

    char sim_no[23] = {0};
    Paytm_ReadSimNumber(sim_no);
    Paytm_TRACE("CCID: %s", sim_no);

    int csq = Paytm_GetSignalStrength();
    Paytm_TRACE("CSQ: %d", csq);

    int32 gsmstate = 0;
    Paytm_GetGSMState(&gsmstate);
    Paytm_TRACE("gprs state %d", gsmstate);
}

void readSimState(void)
{
    Paytm_GPRS_Connect(Paytm__IPVERSION_IPV4, NULL);

    Paytm_delayMilliSeconds(8000);

    int sim_state = Paytm_GetSimState();
    Paytm_TRACE("SIM state: %d", sim_state);
}

void readPDPInfo(void)
{
    Paytm_GPRS_Connect(Paytm__IPVERSION_IPV4, NULL);

    Paytm_delayMilliSeconds(8000);

    char addr[16] = {0};
    Paytm_GetCGPADDR(addr);
    
    Paytm_TRACE("PDP info : %s", addr);
}

void readAPN(void)
{
    Paytm_GPRS_Connect(Paytm__IPVERSION_IPV4, NULL);

    Paytm_delayMilliSeconds(8000);

    char addr[64] = {0};
    PaytmGetAPN(addr);
    
    Paytm_TRACE("APN info : %s", addr);
}