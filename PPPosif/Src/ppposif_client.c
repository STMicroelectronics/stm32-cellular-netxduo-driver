/**
  ******************************************************************************
  * @file    ppposif_client.c
  * @author  MCD Application Team
  * @brief   This file contains pppos client adatation layer
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2018-2021 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */

/* Includes ------------------------------------------------------------------*/

#include <stdbool.h>
#include "ppposif_client.h"
#include "plf_config.h"

#if (USE_SOCKETS_TYPE == USE_SOCKETS_LWIP)
/* NetX is a Third Party so MISRAC messages linked to it are ignored */
/*cstat -MISRAC2012-* */
#include "nx_api.h"
#include "nx_ip.h"
/*cstat +MISRAC2012-* */

#include "ppposif.h"
#include "ipc_uart.h"
#include "ppposif_ipc.h"
#include "rtosal.h"

#include "main.h"
#include "error_handler.h"
#include "trace_interface.h"
#include "dc_common.h"
#include "cellular_service_datacache.h"

/* Private defines -----------------------------------------------------------*/
#define IPC_DEVICE IPC_DEVICE_0
#define PPPOSIF_CONFIG_TIMEOUT_VALUE 15000U
#define PPPOSIF_CONFIG_FAIL_MAX 3

/* Private typedef -----------------------------------------------------------*/
/* Private macros ------------------------------------------------------------*/
/* Private variables ---------------------------------------------------------*/

static ppp_pcb      *ppp_pcb_client;
static osTimerId     ppposif_config_timeout_timer_handle;
static osSemaphoreId sem_ppp_init_client = NULL;

/* Global variables ----------------------------------------------------------*/

/* Private function prototypes -----------------------------------------------*/
static void ppposif_reconf(void);
static void ppposif_client_thread(void *argument);
static void ppposif_config_timeout_timer_callback(void const *argument);

static void ppposif_byte_output(UCHAR byte);
static void link_up_callback(NX_PPP *ppp_ptr);
static void link_down_callback(NX_PPP *ppp_ptr);

/* Functions Definition ------------------------------------------------------*/

/* Private Functions Definition ------------------------------------------------------*/

/* ppposif thread */
/**
  * @brief  ppposif client thread init
  * @param  argument Not used
  * @retval none
  */
static void ppposif_client_thread(void *argument)
{
  UNUSED(argument);
  static struct netif_t gnetif_ppp_client;

  (void)rtosalSemaphoreAcquire(sem_ppp_init_client, RTOSAL_WAIT_FOREVER);
  while (true)
  {
    ppposif_input(&gnetif_ppp_client, ppp_pcb_client, IPC_DEVICE);
  }
}

/**
  * @brief  ppposif client thread reconfiguration
  * @param  none
  * @retval none
  */
static void ppposif_reconf(void)
{
  dc_cellular_info_t cellular_info;

  if (nx_ppp_stop(ppp_pcb_client) != (uint32_t)NX_SUCCESS)
  {
    PRINT_PPPOSIF("ERROR: Unable to stop PPP instance")
  }

  PRINT_PPPOSIF("ppposif_config_timeout_timer_callback")
  (void)dc_com_read(&dc_com_db, DC_CELLULAR_INFO, (void *)&cellular_info, sizeof(cellular_info));
  cellular_info.rt_state_ppp = DC_SERVICE_FAIL;
  (void)dc_com_write(&dc_com_db, DC_CELLULAR_INFO, (void *)&cellular_info, sizeof(cellular_info));
}

/**
  * @brief  callback function for PPPOSIF_CONFIG_TIMEOUT_timer
  * @param  argument
  * @retval none
  */
static void ppposif_config_timeout_timer_callback(void const *argument)
{
  UNUSED(argument);
  ppposif_reconf();
}

/**
  * @brief  byte output routine
  * @note   This function calls the PPPoS serial output callback
  * @param  byte character to write
  * @retval none
  */
static void ppposif_byte_output(UCHAR byte)
{
  (void) ppposif_output_cb(ppp_pcb_client, &byte, sizeof(byte), (void *)IPC_DEVICE);
}

/**
  * @brief  Link Up callback function
  * @param  ppp_ptr Pointer to main PPP data structure
  * @retval none
  */
static void link_up_callback(NX_PPP *ppp_ptr)
{
  ULONG local_ip_address;
  ULONG network_mask;
  UINT interface_index;
  dc_cellular_info_t cellular_info;

  if (_nx_ip_created_ptr != NX_NULL)
  {
    (void) nx_ppp_interface_index_get(ppp_ptr, &interface_index);
    (void) nx_ip_interface_address_get(_nx_ip_created_ptr, interface_index, &local_ip_address, &network_mask);


    (void)rtosalTimerStop(ppposif_config_timeout_timer_handle);

    (void)dc_com_read(&dc_com_db, DC_CELLULAR_INFO, (void *)&cellular_info, sizeof(cellular_info));
    cellular_info.rt_state_ppp = DC_SERVICE_ON;
    cellular_info.ip_addr.addr = COM_NTOHL(local_ip_address);

    (void)dc_com_write(&dc_com_db, DC_CELLULAR_INFO, (void *)&cellular_info, sizeof(cellular_info));

    PRINT_PPPOSIF("\n\r")
    PRINT_PPPOSIF("   our_ipaddr  = %ld.%ld.%ld.%ld",
                  (local_ip_address >> 24) & 0xFFU,
                  (local_ip_address >> 16) & 0xFFU,
                  (local_ip_address >> 8) & 0xFFU,
                  local_ip_address & 0xFFU)
    PRINT_PPPOSIF("   his_ipaddr  = %d.%d.%d.%d",
                  ppp_ptr -> nx_ppp_ipcp_peer_ip[0],
                  ppp_ptr -> nx_ppp_ipcp_peer_ip[1],
                  ppp_ptr -> nx_ppp_ipcp_peer_ip[2],
                  ppp_ptr -> nx_ppp_ipcp_peer_ip[3])
    PRINT_PPPOSIF("   netmask     = %ld.%ld.%ld.%ld",
                  (network_mask >> 24) & 0xFFU,
                  (network_mask >> 16) & 0xFFU,
                  (network_mask >> 8) & 0xFFU,
                  network_mask & 0xFFU)
    PRINT_PPPOSIF("\n\r")
  }
  else
  {
    PRINT_PPPOSIF("ERROR: IP instance not initialized")
  }
}

/**
  * @brief  Link Down callback function
  * @param  ppp_ptr Pointer to main PPP data structure
  * @retval none
  */
static void link_down_callback(NX_PPP *ppp_ptr)
{
  /* Restart the PPP instance.  */
  (void) nx_ppp_restart(ppp_ptr);
}
/*  Exported functions Definition ------------------------------------------------------*/

/**
  * @brief  component init
  * @param  none
  * @retval ppposif_status_t    return status
  */
ppposif_status_t ppposif_client_init(void)
{
  ppposif_ipc_init(IPC_DEVICE);
  return PPPOSIF_OK;
}

/**
  * @brief  component start
  * @param  none
  * @retval ppposif_status_t    return status
  */
ppposif_status_t ppposif_client_start(void)
{
  static osThreadId pppClientThreadId = NULL;

  ppposif_status_t ret = PPPOSIF_OK;

  PRINT_PPPOSIF("ppposif_client_config")

  sem_ppp_init_client = rtosalSemaphoreNew((const rtosal_char_t *)"SEM_PPP_CLIENT_INIT", (uint16_t) 1U);
  (void)rtosalSemaphoreAcquire(sem_ppp_init_client, RTOSAL_WAIT_FOREVER);

  ppposif_config_timeout_timer_handle = rtosalTimerNew((const rtosal_char_t *)"PPPOSIF_CONFIG_TIMEOUT_timer",
                                                       (os_ptimer)ppposif_config_timeout_timer_callback,
                                                       osTimerOnce,
                                                       NULL);

  pppClientThreadId = rtosalThreadNew((const rtosal_char_t *)"PPPosifClt",
                                      (os_pthread) ppposif_client_thread,
                                      PPPOSIF_CLIENT_THREAD_PRIO,
                                      (uint32_t)PPPOSIF_CLIENT_THREAD_STACK_SIZE,
                                      NULL);
  if (pppClientThreadId == NULL)
  {
    ERROR_Handler(DBG_CHAN_PPPOSIF, 1, ERROR_FATAL);
  }

  return ret;
}

/**
  * @brief  Create a new PPPoS client interface
  * @param  none
  * @retval ppposif_status_t    return status
  */
ppposif_status_t ppposif_client_config(void)
{
  static uint32_t ppposif_create_done = 0U;
  static NX_PPP        ppp_0;
  static ULONG         ppp_stack[PPP_THREAD_STACK_SIZE >> 2];

  ppposif_status_t ret = PPPOSIF_OK;

  PRINT_PPPOSIF("ppposif_client_config")
  ppposif_ipc_select(IPC_DEVICE);

  if (ppposif_create_done == 0U)
  {
    ppp_pcb_client = &ppp_0;
    if (_nx_ip_created_ptr == NX_NULL)
    {
      PRINT_PPPOSIF("ERROR: IP instance not initialized")
      ret =  PPPOSIF_ERROR;
      ERROR_Handler(DBG_CHAN_PPPOSIF, 2, ERROR_FATAL);
    }
    else if (nx_ppp_create(ppp_pcb_client, (CHAR *)"PPP Instance", _nx_ip_created_ptr, ppp_stack, sizeof(ppp_stack),
                           PPP_THREAD_PRIORITY, _nx_ip_created_ptr -> nx_ip_default_packet_pool,
                           NX_NULL, ppposif_byte_output) != (UINT)NX_SUCCESS)
    {
      PRINT_PPPOSIF("ERROR: PPP instance not created")
      ret =  PPPOSIF_ERROR;
      ERROR_Handler(DBG_CHAN_PPPOSIF, 3, ERROR_FATAL);
    }

    /* Register the link up callback. */
    else if (nx_ppp_link_up_notify(ppp_pcb_client, link_up_callback) != (UINT)NX_SUCCESS)
    {
      PRINT_PPPOSIF("ERROR: PPP link up callback not registered")
      ret =  PPPOSIF_ERROR;
      ERROR_Handler(DBG_CHAN_PPPOSIF, 4, ERROR_FATAL);
    }
    /* Register the link down callback. */
    else if (nx_ppp_link_down_notify(ppp_pcb_client, link_down_callback) != (UINT)NX_SUCCESS)
    {
      PRINT_PPPOSIF("ERROR: PPP link down callback not registered")
      ret =  PPPOSIF_ERROR;
      ERROR_Handler(DBG_CHAN_PPPOSIF, 5, ERROR_FATAL);
    }
    else if (nx_ip_interface_attach(_nx_ip_created_ptr, (CHAR *)"PPP Interface", 0, 0xFFFFFFFFU, nx_ppp_driver)
             != (UINT)NX_SUCCESS)
    {
      PRINT_PPPOSIF("ERROR: PPP interface not attached")
      ret =  PPPOSIF_ERROR;
      ERROR_Handler(DBG_CHAN_PPPOSIF, 6, ERROR_FATAL);
    }
    else
    {
      /* for Misra */
    }
  }

  if (ret ==  PPPOSIF_OK)
  {
    (void)rtosalTimerStart(ppposif_config_timeout_timer_handle, PPPOSIF_CONFIG_TIMEOUT_VALUE);
    (void)rtosalSemaphoreRelease(sem_ppp_init_client);
  }
  return ret;
}


/**
  * @brief  close a PPPoS client interface
  * @param  none
  * @retval ppposif_status_t    return status
  */
ppposif_status_t ppposif_client_close(uint8_t cause)
{
  dc_cellular_info_t cellular_info;
  PRINT_PPPOSIF("ppposif_client_close")

  (void)rtosalTimerStop(ppposif_config_timeout_timer_handle);

  if (cause == PPPOSIF_CAUSE_POWER_OFF)
  {
    PRINT_PPPOSIF("ppposif_client_close : Closing PPP for POWER OFF")
    (void) ppposif_close(ppp_pcb_client);
  }
  else
  {
    (void)dc_com_read(&dc_com_db, DC_CELLULAR_INFO, (void *)&cellular_info, sizeof(cellular_info));
    cellular_info.rt_state_ppp = DC_SERVICE_FAIL;
    (void)dc_com_write(&dc_com_db, DC_CELLULAR_INFO, (void *)&cellular_info, sizeof(cellular_info));
  }



  return PPPOSIF_OK;
}


#endif /* USE_SOCKETS_TYPE == USE_SOCKETS_LWIP */
