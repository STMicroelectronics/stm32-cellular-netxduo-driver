/**************************************************************************/
/*                                                                        */
/*       Copyright (c) Microsoft Corporation. All rights reserved.        */
/*                                                                        */
/*       This software is licensed under the Microsoft Software License   */
/*       Terms for Microsoft Azure RTOS. Full text of the license can be  */
/*       found in the LICENSE file at https://aka.ms/AzureRTOS_EULA       */
/*       and in the root directory of this software.                      */
/*                                                                        */
/**************************************************************************/


/**************************************************************************/
/**************************************************************************/
/**                                                                       */
/** NetX Component                                                        */
/**                                                                       */
/**   Cellular driver for the STM32 family of microprocessors             */
/**   when using NetX PPP                                                 */
/**                                                                       */
/**************************************************************************/
/**************************************************************************/

/* Indicate that driver source is being compiled.  */
/*cstat -MISRAC2012-* */
#include "nx_api.h"
/*cstat +MISRAC2012-* */
#include "nx_driver_stm32_cellular_ppp.h"
#include "plf_config.h"
#include "cellular_control_api.h"
#include "cellular_runtime_custom.h"

/**
  * @brief  Configure and start driver for PPP
  * @author MCD Application Team - STMicroelectonics.
  * @param  input:  ip_ptr          - ip pointer
  * @param  output: dns_address_ptr - dns address pointer
  * @retval none
  */
void nx_driver_stm32_cellular_configure_ppp(NX_IP *ip_ptr, ULONG *dns_address_ptr)
{
  UCHAR ip_addr[4];  /* Used to analyse DNS server address configuration */
  ULONG ip_address;
  ULONG network_mask = 0;
  ULONG gateway_address = 0;
  static NX_IP *sample_ip_ptr;

  /* DNS value update. */
  if (dns_address_ptr != NULL)
  {
    if ((crc_get_ip_addr(PLF_CELLULAR_DNS_SERVER_IP_ADDR, ip_addr, NULL) == 0U)
        && (ip_addr[0] != 0U) && (ip_addr[1] != 0U) && (ip_addr[2] != 0U) && (ip_addr[3] != 0U))
    {
      *dns_address_ptr = IP_ADDRESS(ip_addr[0], ip_addr[1], ip_addr[2], ip_addr[3]);
    }
    else
    {
      /* Invalid DNS default value */
      printf("Invalid default configuration for DNS server address\r\n");
      *dns_address_ptr = 0U;
    }
  }

  /* FIXME: this is a workaround since IP was created before PPP. A ram network interface was attached.  */
  (void) nx_ip_interface_detach(ip_ptr, 0);

  /* initalize cellular */
  cellular_init();

  /* start cellular */
  cellular_start();

  /* Wait until gateway address is set.  */
  sample_ip_ptr = ip_ptr;
  while (nx_ip_gateway_address_get(sample_ip_ptr, &ip_address) != (uint32_t)NX_SUCCESS)
  {
    (void) tx_thread_sleep(NX_IP_PERIODIC_RATE);
  }

  /* Get IP address and gateway address.  */
  (void) nx_ip_address_get(ip_ptr, &ip_address, &network_mask);
  (void) nx_ip_gateway_address_get(ip_ptr, &gateway_address);

}
/****** DRIVER SPECIFIC ****** Start of part/vendor specific internal driver functions.  */

