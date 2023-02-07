/**
  ******************************************************************************
  * @file    ppposif.c
  * @author  MCD Application Team
  * @brief   This file contains pppos adatation layer
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
#include "plf_config.h"

#include "ppposif.h"
#include "ppposif_client.h"

#if (USE_SOCKETS_TYPE == USE_SOCKETS_LWIP)

#include "ppposif_ipc.h"
#include "cellular_service_os.h"
#include "error_handler.h"

/* Private defines -----------------------------------------------------------*/
#define RCV_SIZE_MAX 100

/* Private typedef -----------------------------------------------------------*/
/* Private macros ------------------------------------------------------------*/
/* Private variables ---------------------------------------------------------*/
/* Global variables ----------------------------------------------------------*/
/* Private function prototypes -----------------------------------------------*/

/* Functions Definition ------------------------------------------------------*/
/**
  * @brief  read data from serial and send it to PPP
  * @param  ppp_netif      (in) netif reference
  * @param  p_ppp_pcb      (in) pcb reference
  * @param  pDevice        (in) serial device id
  * @retval none
  */

void ppposif_input(const struct netif_t *ppp_netif, ppp_pcb  *p_ppp_pcb, IPC_Device_t pDevice)
{
  UNUSED(ppp_netif);
  int32_t rcv_size;
  uint8_t rcvChar[RCV_SIZE_MAX];

  rcv_size = ppposif_ipc_read(pDevice, rcvChar, RCV_SIZE_MAX);
  if (rcv_size != 0)
  {
    /* traceIF_hexPrint(DBG_CHAN_PPPOSIF, DBL_LVL_P0, rcvChar, rcv_size) */
    for (int32_t i = 0; i < rcv_size; i++)
    {
      (void) nx_ppp_byte_receive(p_ppp_pcb, rcvChar[i]);
    }
  }
}

/**
  * @brief  PPPoS serial output callback
  * @param  pcb            (in) PPP control block
  * @param  data           (in) data, buffer to write to serial port
  * @param  len            (in) length of the data buffer
  * @param  ctx            (in) user context : contains serial device id
  * @retval number of char sent if write succeed - 0 otherwise
  */

u32_t ppposif_output_cb(ppp_pcb *pcb, u8_t *data, u32_t len, void *ctx)
{
  UNUSED(pcb);
  IPC_Device_t device;
  u32_t ret;

  if ((int32_t)ctx == (int32_t)IPC_DEVICE_0)
  {
    device = IPC_DEVICE_0;
  }
  else if ((int32_t)ctx == (int32_t)IPC_DEVICE_1)
  {
    device = IPC_DEVICE_1;
  }
  else
  {
    device = IPC_DEVICE_0;
    ERROR_Handler(DBG_CHAN_PPPOSIF, __LINE__, ERROR_FATAL);
  }

  osCCS_get_wait_cs_resource();
  ret = (u32_t)ppposif_ipc_write(device, data, (int16_t)len);
  osCCS_get_release_cs_resource();
  return ret;
}


/**
  * @brief      Closing PPP connection
  * @note       Initiate the end of the PPP session, without carrier lost signal
  *             (nocarrier=0), meaning a clean shutdown of PPP protocols.
  *             You can call this function at anytime.
  * @param  ppp_pcb_struct            (in) pcb reference
  * @retval ppposif_status_t    return status
  */
ppposif_status_t ppposif_close(ppp_pcb *ppp_pcb_struct)
{
  ppposif_status_t status = PPPOSIF_OK;
  if (nx_ppp_stop(ppp_pcb_struct) != (uint32_t)NX_SUCCESS)
  {
    PRINT_PPPOSIF("ERROR: Unable to stop PPP instance")
    status =  PPPOSIF_ERROR;
  }

  return (status);
}
#endif /* USE_SOCKETS_TYPE == USE_SOCKETS_LWIP  */
