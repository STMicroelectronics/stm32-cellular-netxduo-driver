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
/*                                                                        */
/*                                                                        */
/*  NetX Component                                                        */
/*                                                                        */
/*    Cellular driver for the STM32 family of microprocessors             */
/*                                                                        */
/*                                                                        */
/**************************************************************************/

#ifndef NX_DRIVER_CELLULAR_H
#define NX_DRIVER_CELLULAR_H

#ifdef   __cplusplus
extern   "C" {
#endif
  
/* Public API */
void nx_driver_stm32_cellular_configure(NX_IP *ip_ptr, ULONG *dns_address_ptr);
void nx_driver_stm32_cellular(NX_IP_DRIVER *driver_req_ptr);

#ifdef   __cplusplus
}
#endif

#endif /* NX_DRIVER_CELLULAR_H */
