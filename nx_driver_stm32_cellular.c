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
/**   Cellular driver for STM32 family of microprocessors                 */
/**                                                                       */
/**************************************************************************/
/**************************************************************************/

/* Indicate that driver source is being compiled.  */
/*cstat -MISRAC2012-* */
#include "nx_api.h"
/*cstat +MISRAC2012-* */
#include "nx_driver_stm32_cellular.h"
#include "plf_config.h"
#include "com_sockets.h"
#include "cellular_control_api.h"
#include "cellular_runtime_custom.h"

#ifndef NX_ENABLE_TCPIP_OFFLOAD
#error "NX_ENABLE_TCPIP_OFFLOAD must be defined to use this driver"
#endif /* NX_ENABLE_TCPIP_OFFLOAD */

#ifndef NX_DRIVER_IP_MTU
#define NX_DRIVER_IP_MTU                        1500
#endif /* NX_DRIVER_IP_MTU */

#ifndef NX_DRIVER_STACK_SIZE
#define NX_DRIVER_STACK_SIZE                    2048
#endif /* NX_DRIVER_STACK_SIZE  */

/* Interval to receive packets when there is no packet. The default value is 10 ticks which is 100ms.  */
#ifndef NX_DRIVER_THREAD_INTERVAL
#define NX_DRIVER_THREAD_INTERVAL               10
#endif /* NX_DRIVER_THREAD_INTERVAL */

/* Define the maximum sockets at the same time.  */
#ifndef NX_DRIVER_SOCKETS_MAXIMUM
#define NX_DRIVER_SOCKETS_MAXIMUM               16
#endif

#define NX_DRIVER_CAPABILITY                    (NX_INTERFACE_CAPABILITY_TCPIP_OFFLOAD)

/* Define basic netword driver information typedef.  */

typedef struct NX_DRIVER_INFORMATION_STRUCT
{

  /* NetX IP instance that this driver is attached to.  */
  NX_IP               *nx_driver_information_ip_ptr;

  /* Packet pool used for receiving packets. */
  NX_PACKET_POOL      *nx_driver_information_packet_pool_ptr;

  /* Define the driver interface association.  */
  NX_INTERFACE        *nx_driver_information_interface;

  /* A flag to indicate datacache registered or not.  */
  UCHAR                nx_driver_datacache_registered;
} NX_DRIVER_INFORMATION;


/* Define socket structure for hardware TCP/IP.  */

typedef struct NX_DRIVER_SOCKET_STRUCT
{
  VOID                *socket_ptr;
  int                  socket_fd;
  UCHAR                protocol;
  UCHAR                tcp_connected;
  UCHAR                is_client;
  USHORT               local_port;
  USHORT               remote_port;
} NX_DRIVER_SOCKET;

static NX_DRIVER_INFORMATION nx_driver_information;
static NX_DRIVER_SOCKET nx_driver_sockets[NX_DRIVER_SOCKETS_MAXIMUM];
static TX_THREAD nx_driver_thread;
static UCHAR nx_driver_thread_stack[NX_DRIVER_STACK_SIZE];

/* Define the routines for processing each driver entry request.  The contents of these routines will change with
   each driver. However, the main driver entry function will not change, except for the entry function name.  */

static VOID         _nx_driver_interface_attach(NX_IP_DRIVER *driver_req_ptr);
static VOID         _nx_driver_initialize(NX_IP_DRIVER *driver_req_ptr);
static VOID         _nx_driver_enable(NX_IP_DRIVER *driver_req_ptr);
static VOID         _nx_driver_disable(NX_IP_DRIVER *driver_req_ptr);
static VOID         _nx_driver_multicast_join(NX_IP_DRIVER *driver_req_ptr);
static VOID         _nx_driver_multicast_leave(NX_IP_DRIVER *driver_req_ptr);
static VOID         _nx_driver_get_status(NX_IP_DRIVER *driver_req_ptr);
#ifdef NX_ENABLE_INTERFACE_CAPABILITY
static VOID         _nx_driver_capability_get(NX_IP_DRIVER *driver_req_ptr);
static VOID         _nx_driver_capability_set(NX_IP_DRIVER *driver_req_ptr);
#endif /* NX_ENABLE_INTERFACE_CAPABILITY */
static VOID         _nx_driver_deferred_processing(NX_IP_DRIVER *driver_req_ptr);
static VOID         _nx_driver_thread_entry(ULONG thread_input);
static UINT         _nx_driver_tcpip_handler(struct NX_IP_STRUCT *ip_ptr,
                                             struct NX_INTERFACE_STRUCT *interface_ptr,
                                             VOID *socket_ptr, UINT operation, NX_PACKET *packet_ptr,
                                             NXD_ADDRESS *local_ip, NXD_ADDRESS *remote_ip,
                                             UINT local_port, UINT *remote_port, UINT wait_option);
static VOID         _nx_driver_cellular_ip_info_cbk(ca_event_type_t event_type,
                                                    const cellular_ip_info_t *const p_ip_info,
                                                    void *const p_callback_ctx);


/* Define the prototypes for the hardware implementation of this driver. The contents of these routines are
   driver-specific.  */

static UINT         _nx_driver_hardware_initialize(NX_IP_DRIVER *driver_req_ptr);
static UINT         _nx_driver_hardware_enable(NX_IP_DRIVER *driver_req_ptr);
static UINT         _nx_driver_hardware_disable(NX_IP_DRIVER *driver_req_ptr);
static UINT         _nx_driver_hardware_get_status(NX_IP_DRIVER *driver_req_ptr);
#ifdef NX_ENABLE_INTERFACE_CAPABILITY
static UINT         _nx_driver_hardware_capability_set(NX_IP_DRIVER *driver_req_ptr);
#endif /* NX_ENABLE_INTERFACE_CAPABILITY */


/**************************************************************************/
/*                                                                        */
/*  FUNCTION                                               RELEASE        */
/*                                                                        */
/*    nx_driver_stm32_cellular                          STM32/Generic     */
/*                                                           6.x          */
/*  AUTHOR                                                                */
/*                                                                        */
/*    Yuxin Zhou, Microsoft Corporation                                   */
/*                                                                        */
/*  DESCRIPTION                                                           */
/*                                                                        */
/*    This is the entry point of the NetX Driver. This driver             */
/*    function is responsible for initializing the network controller,    */
/*    enabling or disabling the controller as need, preparing             */
/*    a packet for transmission, and getting status information.          */
/*                                                                        */
/*  INPUT                                                                 */
/*                                                                        */
/*    driver_req_ptr                        The driver request from the   */
/*                                            IP layer.                   */
/*                                                                        */
/*  OUTPUT                                                                */
/*                                                                        */
/*    None                                                                */
/*                                                                        */
/*  CALLS                                                                 */
/*                                                                        */
/*    _nx_driver_interface_attach           Process attach request        */
/*    _nx_driver_initialize                 Process initialize request    */
/*    _nx_driver_enable                     Process link enable request   */
/*    _nx_driver_disable                    Process link disable request  */
/*    _nx_driver_multicast_join             Process multicast join request*/
/*    _nx_driver_multicast_leave            Process multicast leave req   */
/*    _nx_driver_get_status                 Process get status request    */
/*    _nx_driver_deferred_processing        Drive deferred processing     */
/*    _nx_driver_capability_get             Get interface capability      */
/*    _nx_driver_capability_set             Set interface capability      */
/*                                                                        */
/*  CALLED BY                                                             */
/*                                                                        */
/*    IP layer                                                            */
/*                                                                        */
/*  RELEASE HISTORY                                                       */
/*                                                                        */
/*    DATE              NAME                      DESCRIPTION             */
/*                                                                        */
/*  xx-xx-xxxx     Yuxin Zhou               Initial Version 6.x           */
/*                                                                        */
/**************************************************************************/
VOID  nx_driver_stm32_cellular(NX_IP_DRIVER *driver_req_ptr)
{

  /* Default to successful return.  */
  driver_req_ptr -> nx_ip_driver_status =  NX_SUCCESS;

  /* Process according to the driver request type in the IP control
     block.  */
  switch (driver_req_ptr -> nx_ip_driver_command)
  {

    case NX_LINK_INTERFACE_ATTACH:
    {
      /* Process link interface attach requests.  */
      _nx_driver_interface_attach(driver_req_ptr);
      break;
    }

    case NX_LINK_INITIALIZE:
    {

      /* Process link initialize requests.  */
      _nx_driver_initialize(driver_req_ptr);
      break;
    }

    case NX_LINK_ENABLE:
    {

      /* Process link enable requests.  */
      _nx_driver_enable(driver_req_ptr);
      break;
    }

    case NX_LINK_DISABLE:
    {

      /* Process link disable requests.  */
      _nx_driver_disable(driver_req_ptr);
      break;
    }


    case NX_LINK_ARP_SEND:
    case NX_LINK_ARP_RESPONSE_SEND:
    case NX_LINK_PACKET_BROADCAST:
    case NX_LINK_RARP_SEND:
    case NX_LINK_PACKET_SEND:
    {

      /* Default to successful return.  */
      driver_req_ptr -> nx_ip_driver_status = NX_NOT_SUCCESSFUL;
      nx_packet_transmit_release(driver_req_ptr -> nx_ip_driver_packet);
      break;
    }


    case NX_LINK_MULTICAST_JOIN:
    {

      /* Process multicast join requests.  */
      _nx_driver_multicast_join(driver_req_ptr);
      break;
    }


    case NX_LINK_MULTICAST_LEAVE:
    {

      /* Process multicast leave requests.  */
      _nx_driver_multicast_leave(driver_req_ptr);
      break;
    }

    case NX_LINK_GET_STATUS:
    {

      /* Process get status requests.  */
      _nx_driver_get_status(driver_req_ptr);
      break;
    }

    case NX_LINK_DEFERRED_PROCESSING:
    {

      /* Process driver deferred requests.  */

      /* Process a device driver function on behave of the IP thread. */
      _nx_driver_deferred_processing(driver_req_ptr);

      break;
    }

#ifdef NX_ENABLE_INTERFACE_CAPABILITY
    case NX_INTERFACE_CAPABILITY_GET:
    {

      /* Process get capability requests.  */
      _nx_driver_capability_get(driver_req_ptr);
      break;
    }

    case NX_INTERFACE_CAPABILITY_SET:
    {

      /* Process set capability requests.  */
      _nx_driver_capability_set(driver_req_ptr);
      break;
    }
#endif /* NX_ENABLE_INTERFACE_CAPABILITY */

    default:


      /* Invalid driver request.  */

      /* Return the unhandled command status.  */
      driver_req_ptr -> nx_ip_driver_status =  NX_UNHANDLED_COMMAND;

      /* Default to successful return.  */
      driver_req_ptr -> nx_ip_driver_status =  NX_NOT_SUCCESSFUL;
  }
}


/**************************************************************************/
/*                                                                        */
/*  FUNCTION                                               RELEASE        */
/*                                                                        */
/*    _nx_driver_interface_attach                       x86/Generic       */
/*                                                           6.x          */
/*  AUTHOR                                                                */
/*                                                                        */
/*    Yuxin Zhou, Microsoft Corporation                                   */
/*                                                                        */
/*  DESCRIPTION                                                           */
/*                                                                        */
/*    This function processing the interface attach request.              */
/*                                                                        */
/*  INPUT                                                                 */
/*                                                                        */
/*    driver_req_ptr                        Driver command from the IP    */
/*                                            thread                      */
/*                                                                        */
/*  OUTPUT                                                                */
/*                                                                        */
/*    None                                                                */
/*                                                                        */
/*  CALLS                                                                 */
/*                                                                        */
/*    None                                                                */
/*                                                                        */
/*  CALLED BY                                                             */
/*                                                                        */
/*    Driver entry function                                               */
/*                                                                        */
/*  RELEASE HISTORY                                                       */
/*                                                                        */
/*    DATE              NAME                      DESCRIPTION             */
/*                                                                        */
/*  xx-xx-xxxx     Yuxin Zhou               Initial Version 6.x           */
/*                                                                        */
/**************************************************************************/
static VOID  _nx_driver_interface_attach(NX_IP_DRIVER *driver_req_ptr)
{
#ifdef NX_ENABLE_INTERFACE_CAPABILITY
  driver_req_ptr -> nx_ip_driver_interface -> nx_interface_capability_flag = NX_DRIVER_CAPABILITY;
#endif /* NX_ENABLE_INTERFACE_CAPABILITY */

  /* Return successful status.  */
  driver_req_ptr -> nx_ip_driver_status =  NX_SUCCESS;
}


/**************************************************************************/
/*                                                                        */
/*  FUNCTION                                               RELEASE        */
/*                                                                        */
/*    _nx_driver_initialize                             x86/Generic       */
/*                                                           6.x          */
/*  AUTHOR                                                                */
/*                                                                        */
/*    Yuxin Zhou, Microsoft Corporation                                   */
/*                                                                        */
/*  DESCRIPTION                                                           */
/*                                                                        */
/*    This function processing the initialize request.  The processing    */
/*    in this function is generic. All ethernet controller logic is to    */
/*    be placed in _nx_driver_hardware_initialize.                        */
/*                                                                        */
/*  INPUT                                                                 */
/*                                                                        */
/*    driver_req_ptr                        Driver command from the IP    */
/*                                            thread                      */
/*                                                                        */
/*  OUTPUT                                                                */
/*                                                                        */
/*    None                                                                */
/*                                                                        */
/*  CALLS                                                                 */
/*                                                                        */
/*    _nx_driver_hardware_initialize        Process initialize request    */
/*                                                                        */
/*  CALLED BY                                                             */
/*                                                                        */
/*    Driver entry function                                               */
/*                                                                        */
/*  RELEASE HISTORY                                                       */
/*                                                                        */
/*    DATE              NAME                      DESCRIPTION             */
/*                                                                        */
/*  xx-xx-xxxx     Yuxin Zhou               Initial Version 6.x           */
/*                                                                        */
/**************************************************************************/
static VOID  _nx_driver_initialize(NX_IP_DRIVER *driver_req_ptr)
{

  NX_IP        *ip_ptr;
  NX_INTERFACE *interface_ptr;
  UINT          status;

  /* Setup the IP pointer from the driver request.  */
  ip_ptr =  driver_req_ptr -> nx_ip_driver_ptr;

  /* Setup interface pointer.  */
  interface_ptr = driver_req_ptr -> nx_ip_driver_interface;

  /* Initialize the driver's information structure.  */

  /* Default IP pointer to NULL.  */
  nx_driver_information.nx_driver_information_ip_ptr = NX_NULL;

  /* Setup the default packet pool for the driver's received packets.  */
  nx_driver_information.nx_driver_information_packet_pool_ptr = ip_ptr -> nx_ip_default_packet_pool;

  /* Call the hardware-specific ethernet controller initialization.  */
  status =  _nx_driver_hardware_initialize(driver_req_ptr);

  /* Determine if the request was successful.  */
  if (status == NX_SUCCESS)
  {

    /* Successful hardware initialization.  */

    /* Setup driver information to point to IP pointer.  */
    nx_driver_information.nx_driver_information_ip_ptr = ip_ptr;
    nx_driver_information.nx_driver_information_interface = interface_ptr;

    /* Setup the link maximum transfer unit. */
    interface_ptr -> nx_interface_ip_mtu_size = NX_DRIVER_IP_MTU;

    /* Setup the physical address of this IP instance.  */
    /* TODO: Use WIFI_GetMAC_Address() to set the real value.  */
    interface_ptr -> nx_interface_physical_address_msw = 0;
    interface_ptr -> nx_interface_physical_address_lsw = 0;

    /* Indicate to the IP software that IP to physical mapping
       is required.  */
    interface_ptr -> nx_interface_address_mapping_needed =  NX_FALSE;

    /* Indicate successful initialize.  */
    driver_req_ptr -> nx_ip_driver_status =  NX_SUCCESS;
  }
  else
  {

    /* Initialization failed.  Indicate that the request failed.  */
    driver_req_ptr -> nx_ip_driver_status =   NX_NOT_SUCCESSFUL;
  }
}


/**************************************************************************/
/*                                                                        */
/*  FUNCTION                                               RELEASE        */
/*                                                                        */
/*    _nx_driver_enable                                 x86/Generic       */
/*                                                           6.x          */
/*  AUTHOR                                                                */
/*                                                                        */
/*    Yuxin Zhou, Microsoft Corporation                                   */
/*                                                                        */
/*  DESCRIPTION                                                           */
/*                                                                        */
/*    This function processing the initialize request. The processing     */
/*    in this function is generic. All ethernet controller logic is to    */
/*    be placed in _nx_driver_hardware_enable.                            */
/*                                                                        */
/*  INPUT                                                                 */
/*                                                                        */
/*    driver_req_ptr                        Driver command from the IP    */
/*                                            thread                      */
/*                                                                        */
/*  OUTPUT                                                                */
/*                                                                        */
/*    None                                                                */
/*                                                                        */
/*  CALLS                                                                 */
/*                                                                        */
/*    _nx_driver_hardware_enable            Process enable request        */
/*                                                                        */
/*  CALLED BY                                                             */
/*                                                                        */
/*    Driver entry function                                               */
/*                                                                        */
/*  RELEASE HISTORY                                                       */
/*                                                                        */
/*    DATE              NAME                      DESCRIPTION             */
/*                                                                        */
/*  xx-xx-xxxx     Yuxin Zhou               Initial Version 6.x           */
/*  13-01-2022     STMicrolectronics.       Change datacache service use  */
/*                                          to cellular control service   */
/*                                                                        */
/**************************************************************************/
static VOID  _nx_driver_enable(NX_IP_DRIVER *driver_req_ptr)
{

  UINT status;
  cellular_ip_info_t ip_info;

  /* Call hardware specific enable.  */
  status =  _nx_driver_hardware_enable(driver_req_ptr);

  /* Was the hardware enable successful?  */
  if (status == NX_SUCCESS)
  {

    /* Mark request as successful.  */
    driver_req_ptr -> nx_ip_driver_status =  NX_SUCCESS;

    /* Is Data ready or not ? */
    cellular_get_ip_info(&ip_info);

    /* Mark the IP interface as link up when cellular is connected.  */
    if (ip_info.ip_addr.addr != 0U)
    {
      driver_req_ptr -> nx_ip_driver_interface -> nx_interface_link_up =  NX_TRUE;
    }
    else
    {
      driver_req_ptr -> nx_ip_driver_interface -> nx_interface_link_up =  NX_FALSE;
    }

    /* Set TCP/IP callback function.  */
    driver_req_ptr -> nx_ip_driver_interface -> nx_interface_tcpip_offload_handler = _nx_driver_tcpip_handler;
  }
  else
  {

    /* Enable failed.  Indicate that the request failed.  */
    driver_req_ptr -> nx_ip_driver_status =   NX_NOT_SUCCESSFUL;
  }
}


/**************************************************************************/
/*                                                                        */
/*  FUNCTION                                               RELEASE        */
/*                                                                        */
/*    _nx_driver_disable                                x86/Generic       */
/*                                                           6.x          */
/*  AUTHOR                                                                */
/*                                                                        */
/*    Yuxin Zhou, Microsoft Corporation                                   */
/*                                                                        */
/*  DESCRIPTION                                                           */
/*                                                                        */
/*    This function processing the disable request. The processing        */
/*    in this function is generic. All ethernet controller logic is to    */
/*    be placed in _nx_driver_hardware_disable.                           */
/*                                                                        */
/*  INPUT                                                                 */
/*                                                                        */
/*    driver_req_ptr                        Driver command from the IP    */
/*                                            thread                      */
/*                                                                        */
/*  OUTPUT                                                                */
/*                                                                        */
/*    None                                                                */
/*                                                                        */
/*  CALLS                                                                 */
/*                                                                        */
/*    _nx_driver_hardware_disable           Process disable request       */
/*                                                                        */
/*  CALLED BY                                                             */
/*                                                                        */
/*    Driver entry function                                               */
/*                                                                        */
/*  RELEASE HISTORY                                                       */
/*                                                                        */
/*    DATE              NAME                      DESCRIPTION             */
/*                                                                        */
/*  xx-xx-xxxx     Yuxin Zhou               Initial Version 6.x           */
/*                                                                        */
/**************************************************************************/
static VOID  _nx_driver_disable(NX_IP_DRIVER *driver_req_ptr)
{

  UINT status;

  /* Call hardware specific disable.  */
  status =  _nx_driver_hardware_disable(driver_req_ptr);

  /* Was the hardware disable successful?  */
  if (status == NX_SUCCESS)
  {

    /* Mark the IP interface as link down.  */
    driver_req_ptr -> nx_ip_driver_interface -> nx_interface_link_up =  NX_FALSE;

    /* Mark request as successful.  */
    driver_req_ptr -> nx_ip_driver_status =  NX_SUCCESS;

    /* Clear the TCP/IP callback function.  */
    driver_req_ptr -> nx_ip_driver_interface -> nx_interface_tcpip_offload_handler = NX_NULL;
  }
  else
  {

    /* Disable failed, return an error.  */
    driver_req_ptr -> nx_ip_driver_status =  NX_NOT_SUCCESSFUL;
  }
}


/**************************************************************************/
/*                                                                        */
/*  FUNCTION                                               RELEASE        */
/*                                                                        */
/*    _nx_driver_multicast_join                         x86/Generic       */
/*                                                           6.x          */
/*  AUTHOR                                                                */
/*                                                                        */
/*    Yuxin Zhou, Microsoft Corporation                                   */
/*                                                                        */
/*  DESCRIPTION                                                           */
/*                                                                        */
/*    This function processing the multicast join request. The processing */
/*    in this function is generic. All ethernet controller multicast join */
/*    logic is to be placed in _nx_driver_hardware_multicast_join.        */
/*                                                                        */
/*  INPUT                                                                 */
/*                                                                        */
/*    driver_req_ptr                        Driver command from the IP    */
/*                                            thread                      */
/*                                                                        */
/*  OUTPUT                                                                */
/*                                                                        */
/*    None                                                                */
/*                                                                        */
/*  CALLS                                                                 */
/*                                                                        */
/*    None                                                                */
/*                                                                        */
/*  CALLED BY                                                             */
/*                                                                        */
/*    Driver entry function                                               */
/*                                                                        */
/*  RELEASE HISTORY                                                       */
/*                                                                        */
/*    DATE              NAME                      DESCRIPTION             */
/*                                                                        */
/*  xx-xx-xxxx     Yuxin Zhou               Initial Version 6.x           */
/*                                                                        */
/**************************************************************************/
static VOID  _nx_driver_multicast_join(NX_IP_DRIVER *driver_req_ptr)
{

  /* Not supported.  */
  driver_req_ptr -> nx_ip_driver_status =  NX_NOT_SUCCESSFUL;
}


/**************************************************************************/
/*                                                                        */
/*  FUNCTION                                               RELEASE        */
/*                                                                        */
/*    _nx_driver_multicast_leave                        x86/Generic       */
/*                                                           6.x          */
/*  AUTHOR                                                                */
/*                                                                        */
/*    Yuxin Zhou, Microsoft Corporation                                   */
/*                                                                        */
/*  DESCRIPTION                                                           */
/*                                                                        */
/*    This function processing the multicast leave request. The           */
/*    processing in this function is generic. All ethernet controller     */
/*    multicast leave logic is to be placed in                            */
/*    _nx_driver_hardware_multicast_leave.                                */
/*                                                                        */
/*  INPUT                                                                 */
/*                                                                        */
/*    driver_req_ptr                        Driver command from the IP    */
/*                                            thread                      */
/*                                                                        */
/*  OUTPUT                                                                */
/*                                                                        */
/*    None                                                                */
/*                                                                        */
/*  CALLS                                                                 */
/*                                                                        */
/*    None                                                                */
/*                                                                        */
/*  CALLED BY                                                             */
/*                                                                        */
/*    Driver entry function                                               */
/*                                                                        */
/*  RELEASE HISTORY                                                       */
/*                                                                        */
/*    DATE              NAME                      DESCRIPTION             */
/*                                                                        */
/*  xx-xx-xxxx     Yuxin Zhou               Initial Version 6.x           */
/*                                                                        */
/**************************************************************************/
static VOID  _nx_driver_multicast_leave(NX_IP_DRIVER *driver_req_ptr)
{

  /* Not supported.  */
  driver_req_ptr -> nx_ip_driver_status =  NX_NOT_SUCCESSFUL;
}


/**************************************************************************/
/*                                                                        */
/*  FUNCTION                                               RELEASE        */
/*                                                                        */
/*    _nx_driver_get_status                             x86/Generic       */
/*                                                           6.x          */
/*  AUTHOR                                                                */
/*                                                                        */
/*    Yuxin Zhou, Microsoft Corporation                                   */
/*                                                                        */
/*  DESCRIPTION                                                           */
/*                                                                        */
/*    This function processing the get status request. The processing     */
/*    in this function is generic. All ethernet controller get status     */
/*    logic is to be placed in _nx_driver_hardware_get_status.            */
/*                                                                        */
/*  INPUT                                                                 */
/*                                                                        */
/*    driver_req_ptr                        Driver command from the IP    */
/*                                            thread                      */
/*                                                                        */
/*  OUTPUT                                                                */
/*                                                                        */
/*    None                                                                */
/*                                                                        */
/*  CALLS                                                                 */
/*                                                                        */
/*    _nx_driver_hardware_get_status        Process get status request    */
/*                                                                        */
/*  CALLED BY                                                             */
/*                                                                        */
/*    Driver entry function                                               */
/*                                                                        */
/*  RELEASE HISTORY                                                       */
/*                                                                        */
/*    DATE              NAME                      DESCRIPTION             */
/*                                                                        */
/*  xx-xx-xxxx     Yuxin Zhou               Initial Version 6.x           */
/*                                                                        */
/**************************************************************************/
static VOID  _nx_driver_get_status(NX_IP_DRIVER *driver_req_ptr)
{

  UINT        status;


  /* Call hardware specific get status function. */
  status =  _nx_driver_hardware_get_status(driver_req_ptr);

  /* Determine if there was an error.  */
  if (status != NX_SUCCESS)
  {

    /* Indicate an unsuccessful request.  */
    driver_req_ptr -> nx_ip_driver_status =  NX_NOT_SUCCESSFUL;
  }
  else
  {

    /* Indicate the request was successful.   */
    driver_req_ptr -> nx_ip_driver_status =  NX_SUCCESS;
  }
}


#ifdef NX_ENABLE_INTERFACE_CAPABILITY
/**************************************************************************/
/*                                                                        */
/*  FUNCTION                                               RELEASE        */
/*                                                                        */
/*    _nx_driver_capability_get                         x86/Generic       */
/*                                                           6.x          */
/*  AUTHOR                                                                */
/*                                                                        */
/*    Yuxin Zhou, Microsoft Corporation                                   */
/*                                                                        */
/*  DESCRIPTION                                                           */
/*                                                                        */
/*    This function processing the get capability request.                */
/*                                                                        */
/*  INPUT                                                                 */
/*                                                                        */
/*    driver_req_ptr                        Driver command from the IP    */
/*                                            thread                      */
/*                                                                        */
/*  OUTPUT                                                                */
/*                                                                        */
/*    None                                                                */
/*                                                                        */
/*  CALLS                                                                 */
/*                                                                        */
/*    None                                                                */
/*                                                                        */
/*  CALLED BY                                                             */
/*                                                                        */
/*    Driver entry function                                               */
/*                                                                        */
/*  RELEASE HISTORY                                                       */
/*                                                                        */
/*    DATE              NAME                      DESCRIPTION             */
/*                                                                        */
/*  xx-xx-xxxx     Yuxin Zhou               Initial Version 6.x           */
/*                                                                        */
/**************************************************************************/
static VOID  _nx_driver_capability_get(NX_IP_DRIVER *driver_req_ptr)
{

  /* Return the capability of the Ethernet controller.  */
  *(driver_req_ptr -> nx_ip_driver_return_ptr) = NX_DRIVER_CAPABILITY;

  /* Return the success status.  */
  driver_req_ptr -> nx_ip_driver_status =  NX_SUCCESS;
}


/**************************************************************************/
/*                                                                        */
/*  FUNCTION                                               RELEASE        */
/*                                                                        */
/*    _nx_driver_capability_set                         x86/Generic       */
/*                                                           6.x          */
/*  AUTHOR                                                                */
/*                                                                        */
/*    Yuxin Zhou, Microsoft Corporation                                   */
/*                                                                        */
/*  DESCRIPTION                                                           */
/*                                                                        */
/*    This function processing the set capability request.                */
/*                                                                        */
/*  INPUT                                                                 */
/*                                                                        */
/*    driver_req_ptr                        Driver command from the IP    */
/*                                            thread                      */
/*                                                                        */
/*  OUTPUT                                                                */
/*                                                                        */
/*    None                                                                */
/*                                                                        */
/*  CALLS                                                                 */
/*                                                                        */
/*    None                                                                */
/*                                                                        */
/*  CALLED BY                                                             */
/*                                                                        */
/*    Driver entry function                                               */
/*                                                                        */
/*  RELEASE HISTORY                                                       */
/*                                                                        */
/*    DATE              NAME                      DESCRIPTION             */
/*                                                                        */
/*  xx-xx-xxxx     Yuxin Zhou               Initial Version 6.x           */
/*                                                                        */
/**************************************************************************/
static VOID  _nx_driver_capability_set(NX_IP_DRIVER *driver_req_ptr)
{

  UINT        status;


  /* Call hardware specific get status function. */
  status =  _nx_driver_hardware_capability_set(driver_req_ptr);

  /* Determine if there was an error.  */
  if (status != NX_SUCCESS)
  {

    /* Indicate an unsuccessful request.  */
    driver_req_ptr -> nx_ip_driver_status =  NX_NOT_SUCCESSFUL;
  }
  else
  {

    /* Indicate the request was successful.   */
    driver_req_ptr -> nx_ip_driver_status =  NX_SUCCESS;
  }
}
#endif /* NX_ENABLE_INTERFACE_CAPABILITY */


/**************************************************************************/
/*                                                                        */
/*  FUNCTION                                               RELEASE        */
/*                                                                        */
/*    _nx_driver_deferred_processing                    x86/Generic       */
/*                                                           6.x          */
/*  AUTHOR                                                                */
/*                                                                        */
/*    Yuxin Zhou, Microsoft Corporation                                   */
/*                                                                        */
/*  DESCRIPTION                                                           */
/*                                                                        */
/*    This function processing the deferred ISR action within the context */
/*    of the IP thread.                                                   */
/*                                                                        */
/*  INPUT                                                                 */
/*                                                                        */
/*    driver_req_ptr                        Driver command from the IP    */
/*                                            thread                      */
/*                                                                        */
/*  OUTPUT                                                                */
/*                                                                        */
/*    None                                                                */
/*                                                                        */
/*  CALLS                                                                 */
/*                                                                        */
/*    None                                                                */
/*                                                                        */
/*  CALLED BY                                                             */
/*                                                                        */
/*    Driver entry function                                               */
/*                                                                        */
/*  RELEASE HISTORY                                                       */
/*                                                                        */
/*    DATE              NAME                      DESCRIPTION             */
/*                                                                        */
/*  xx-xx-xxxx     Yuxin Zhou               Initial Version 6.x           */
/*                                                                        */
/**************************************************************************/
static VOID  _nx_driver_deferred_processing(NX_IP_DRIVER *driver_req_ptr)
{
}


/**************************************************************************/
/*                                                                        */
/*  FUNCTION                                               RELEASE        */
/*                                                                        */
/*    _nx_driver_thread_entry                           x86/Generic       */
/*                                                           6.x          */
/*  AUTHOR                                                                */
/*                                                                        */
/*    Yuxin Zhou, Microsoft Corporation                                   */
/*                                                                        */
/*  DESCRIPTION                                                           */
/*                                                                        */
/*    This function is the driver thread entry. In this thread, it        */
/*    performs checking for incoming TCP and UDP packets. On new packet,  */
/*    it will be passed to NetX.                                          */
/*                                                                        */
/*  INPUT                                                                 */
/*                                                                        */
/*    thread_input                          Thread input                  */
/*                                                                        */
/*  OUTPUT                                                                */
/*                                                                        */
/*    None                                                                */
/*                                                                        */
/*  CALLS                                                                 */
/*                                                                        */
/*    tx_mutex_get                          Obtain protection mutex       */
/*    tx_mutex_put                          Release protection mutex      */
/*    tx_thread_sleep                       Sleep driver thread           */
/*    nx_packet_allocate                    Allocate a packet for incoming*/
/*                                            TCP and UDP data            */
/*    _nx_tcp_socket_driver_packet_receive  Receive TCP packet            */
/*    _nx_udp_socket_driver_packet_receive  Receive UDP packet            */
/*                                                                        */
/*  CALLED BY                                                             */
/*                                                                        */
/*    Driver entry function                                               */
/*                                                                        */
/*  RELEASE HISTORY                                                       */
/*                                                                        */
/*    DATE              NAME                      DESCRIPTION             */
/*                                                                        */
/*  xx-xx-xxxx     Yuxin Zhou               Initial Version 6.x           */
/*                                                                        */
/**************************************************************************/
static VOID _nx_driver_thread_entry(ULONG thread_input)
{
  UINT i;
  NX_PACKET *packet_ptr;
  UINT packet_type;
  NXD_ADDRESS local_ip;
  NXD_ADDRESS remote_ip;
  int32_t addrlen;
  int ret;
  com_sockaddr_in_t remote_addr;
  int data_length;
  NX_IP *ip_ptr = nx_driver_information.nx_driver_information_ip_ptr;
  NX_INTERFACE *interface_ptr = nx_driver_information.nx_driver_information_interface;
  NX_PACKET_POOL *pool_ptr = nx_driver_information.nx_driver_information_packet_pool_ptr;

  NX_PARAMETER_NOT_USED(thread_input);

  for (;;)
  {

    /* Obtain the IP internal mutex before processing the IP event.  */
    tx_mutex_get(&(ip_ptr -> nx_ip_protection), TX_WAIT_FOREVER);

    /* Loop through TCP socket.  */
    for (i = 0; i < NX_DRIVER_SOCKETS_MAXIMUM; i++)
    {
      if (nx_driver_sockets[i].socket_ptr == NX_NULL)
      {

        /* Skip sockets not used.  */
        continue;
      }

      if ((nx_driver_sockets[i].local_port == 0) &&
          (nx_driver_sockets[i].remote_port == 0))
      {

        /* Skip sockets not listening.  */
        continue;
      }

      /* Set packet type.  */
      if (nx_driver_sockets[i].protocol == NX_PROTOCOL_TCP)
      {
        packet_type = NX_TCP_PACKET;
        if ((nx_driver_sockets[i].tcp_connected == NX_FALSE) &&
            (nx_driver_sockets[i].is_client == NX_FALSE))
        {

          /* TCP server. Try accept. */
          if (_nx_tcp_socket_driver_establish(nx_driver_sockets[i].socket_ptr, interface_ptr, 0))
          {

            /* NetX TCP socket is not ready to accept. Just sleep to avoid starving.  */
            tx_thread_sleep(NX_DRIVER_THREAD_INTERVAL);
            continue;
          }
        }
      }
      else
      {
        packet_type = NX_UDP_PACKET;
      }

      /* Loop to receive all data on current socket.  */
      for (;;)
      {
        if ((nx_driver_sockets[i].protocol == NX_PROTOCOL_TCP) &&
            (nx_driver_sockets[i].tcp_connected == NX_FALSE))
        {

          /* TCP socket disconnected.  */
          break;
        }

        if (nx_packet_allocate(pool_ptr, &packet_ptr, packet_type, NX_NO_WAIT))
        {

          /* Packet not available.  */
          break;
        }

        /* Get available size of packet.  */
        data_length = (int)(packet_ptr -> nx_packet_data_end - packet_ptr -> nx_packet_prepend_ptr);

        /* Receive data. */
        addrlen = sizeof(remote_addr);
        ret = com_recvfrom(nx_driver_sockets[i].socket_fd,
                           (com_char_t *)packet_ptr -> nx_packet_prepend_ptr, data_length, COM_MSG_DONTWAIT,
                           (com_sockaddr_t *)&remote_addr, &addrlen);

        if (ret < 0)
        {

          /* Connection error. Notify upper layer with Null packet.  */
          if (nx_driver_sockets[i].protocol == NX_PROTOCOL_TCP)
          {
            _nx_tcp_socket_driver_packet_receive(nx_driver_sockets[i].socket_ptr, NX_NULL);
            nx_driver_sockets[i].tcp_connected = NX_FALSE;
          }
          else
          {
            _nx_udp_socket_driver_packet_receive(nx_driver_sockets[i].socket_ptr, NX_NULL,
                                                 NX_NULL, NX_NULL, 0);
          }
          nx_packet_release(packet_ptr);
          break;
        }

        if (ret == 0)
        {

          /* No incoming data.  */
          nx_packet_release(packet_ptr);
          break;
        }

        /* Set packet length.  */
        packet_ptr -> nx_packet_length = (ULONG)ret;
        packet_ptr -> nx_packet_append_ptr = packet_ptr -> nx_packet_prepend_ptr + ret;
        packet_ptr -> nx_packet_ip_interface = interface_ptr;

        /* Pass it to NetXDuo.  */
        if (nx_driver_sockets[i].protocol == NX_PROTOCOL_TCP)
        {
          _nx_tcp_socket_driver_packet_receive(nx_driver_sockets[i].socket_ptr, packet_ptr);
        }
        else
        {

          /* Convert IP version.  */
          remote_ip.nxd_ip_version = NX_IP_VERSION_V4;
          remote_ip.nxd_ip_address.v4 = COM_NTOHL(remote_addr.sin_addr.s_addr);
          local_ip.nxd_ip_version = NX_IP_VERSION_V4;
          local_ip.nxd_ip_address.v4 = interface_ptr -> nx_interface_ip_address;

          _nx_udp_socket_driver_packet_receive(nx_driver_sockets[i].socket_ptr,
                                               packet_ptr, &local_ip, &remote_ip,
                                               COM_NTOHS(remote_addr.sin_port));
        }
      }
    }

    /* Release the IP internal mutex before processing the IP event.  */
    tx_mutex_put(&(ip_ptr -> nx_ip_protection));

    /* Sleep some ticks to next loop.  */
    tx_thread_sleep(NX_DRIVER_THREAD_INTERVAL);
  }
}


/**************************************************************************/
/*                                                                        */
/*  FUNCTION                                               RELEASE        */
/*                                                                        */
/*    _nx_driver_tcpip_handler                          x86/Generic       */
/*                                                           6.x          */
/*  AUTHOR                                                                */
/*                                                                        */
/*    Yuxin Zhou, Microsoft Corporation                                   */
/*                                                                        */
/*  DESCRIPTION                                                           */
/*                                                                        */
/*    This function processing the TCP/IP request.                        */
/*                                                                        */
/*  INPUT                                                                 */
/*                                                                        */
/*    ip_ptr                                Pointer to IP                 */
/*    interface_ptr                         Pointer to interface          */
/*    socket_ptr                            Pointer to TCP or UDP socket  */
/*    operation                             Operation of TCP/IP request   */
/*    packet_ptr                            Pointer to packet             */
/*    local_ip                              Pointer to local IP address   */
/*    remote_ip                             Pointer to remote IP address  */
/*    local_port                            Local socket port             */
/*    remote_port                           Remote socket port            */
/*    wait_option                           Wait option in ticks          */
/*                                                                        */
/*  OUTPUT                                                                */
/*                                                                        */
/*    None                                                                */
/*                                                                        */
/*  CALLS                                                                 */
/*                                                                        */
/*    nx_packet_transmit_release            Release transmittion packet   */
/*                                                                        */
/*  CALLED BY                                                             */
/*                                                                        */
/*    Driver entry function                                               */
/*                                                                        */
/*  RELEASE HISTORY                                                       */
/*                                                                        */
/*    DATE              NAME                      DESCRIPTION             */
/*                                                                        */
/*  xx-xx-xxxx     Yuxin Zhou               Initial Version 6.x           */
/*                                                                        */
/**************************************************************************/
static UINT _nx_driver_tcpip_handler(struct NX_IP_STRUCT *ip_ptr,
                                     struct NX_INTERFACE_STRUCT *interface_ptr,
                                     VOID *socket_ptr, UINT operation, NX_PACKET *packet_ptr,
                                     NXD_ADDRESS *local_ip, NXD_ADDRESS *remote_ip,
                                     UINT local_port, UINT *remote_port, UINT wait_option)
{
  UINT status = NX_NOT_SUCCESSFUL;
  NX_PACKET *current_packet;
  ULONG packet_size;
  UINT i;
  int ret;
  int socket_fd;
  com_sockaddr_in_t remote_addr, local_addr;

  NX_PARAMETER_NOT_USED(local_ip);
  NX_PARAMETER_NOT_USED(remote_ip);

  if ((operation == NX_TCPIP_OFFLOAD_TCP_CLIENT_SOCKET_CONNECT) ||
      (operation == NX_TCPIP_OFFLOAD_UDP_SOCKET_BIND))
  {

    /* Find a socket that is not used.  */
    for (i = 0; i < NX_DRIVER_SOCKETS_MAXIMUM; i++)
    {
      if (nx_driver_sockets[i].socket_ptr == NX_NULL)
      {

        /* Find an empty entry.  */
        if (operation == NX_TCPIP_OFFLOAD_UDP_SOCKET_BIND)
        {
          socket_fd = com_socket(COM_AF_INET, COM_SOCK_DGRAM, 0);
        }
        else
        {
          socket_fd = com_socket(COM_AF_INET, COM_SOCK_STREAM, 0);
        }
        if (socket_fd < 0)
        {

          /* Failed to create socket.  */
          return (NX_NOT_SUCCESSFUL);
        }
        nx_driver_sockets[i].socket_fd = socket_fd;
        nx_driver_sockets[i].socket_ptr = socket_ptr;
        break;
      }
    }

    if (i == NX_DRIVER_SOCKETS_MAXIMUM)
    {

      /* No more entries.  */
      return (NX_NO_MORE_ENTRIES);
    }
  }

  switch (operation)
  {
    case NX_TCPIP_OFFLOAD_TCP_CLIENT_SOCKET_CONNECT:

      /* Store the index of driver socket.  */
      ((NX_TCP_SOCKET *)socket_ptr) -> nx_tcp_socket_tcpip_offload_context = (VOID *)i;

      /* Store address and port.  */
      nx_driver_sockets[i].local_port = 0;
      nx_driver_sockets[i].remote_port = 0;
      nx_driver_sockets[i].protocol = NX_PROTOCOL_TCP;
      nx_driver_sockets[i].tcp_connected = NX_FALSE;
      nx_driver_sockets[i].is_client = NX_TRUE;

      /* Bind to local IP address. */
      local_addr.sin_family = COM_AF_INET;
      local_addr.sin_port = COM_HTONS(local_port);
      local_addr.sin_addr.s_addr = COM_INADDR_ANY;
      ret = com_bind(socket_fd, (com_sockaddr_t *)&local_addr, sizeof(local_addr));
      if (ret < 0)
      {
        return (NX_NOT_SUCCESSFUL);
      }

      /* Set remote address.  */
      remote_addr.sin_family = COM_AF_INET;
      remote_addr.sin_port = COM_HTONS(*remote_port);
      remote_addr.sin_addr.s_addr = COM_HTONL(remote_ip -> nxd_ip_address.v4);

      /* Connect.  */
      ret = com_connect(socket_fd, (com_sockaddr_t *)&remote_addr, sizeof(remote_addr));
      if (ret < 0)
      {
        return (NX_NOT_SUCCESSFUL);
      }

      nx_driver_sockets[i].tcp_connected = NX_TRUE;
      nx_driver_sockets[i].local_port = local_port;
      nx_driver_sockets[i].remote_port = *remote_port;

#ifdef NX_DEBUG
      printf("TCP client socket %u connect to: %u.%u.%u.%u:%u\r\n", i,
             (remote_ip -> nxd_ip_address.v4 >> 24) & 0xFF,
             (remote_ip -> nxd_ip_address.v4 >> 16) & 0xFF,
             (remote_ip -> nxd_ip_address.v4 >> 8) & 0xFF,
             remote_ip -> nxd_ip_address.v4 & 0xFF, *remote_port);
#endif
      status = NX_SUCCESS;
      break;

    case NX_TCPIP_OFFLOAD_TCP_SERVER_SOCKET_LISTEN:
      /* Limitation from COM API. Not supported. */
      break;

    case NX_TCPIP_OFFLOAD_TCP_SERVER_SOCKET_ACCEPT:
      /* Limitation from COM API. Not supported. */
      break;

    case NX_TCPIP_OFFLOAD_TCP_SERVER_SOCKET_UNLISTEN:
      /* Limitation from COM API. Not supported. */
      break;

    case NX_TCPIP_OFFLOAD_TCP_SOCKET_DISCONNECT:
      i = (UINT)(((NX_TCP_SOCKET *)socket_ptr) -> nx_tcp_socket_tcpip_offload_context);

      /* Close socket.  */
      socket_fd = nx_driver_sockets[i].socket_fd;
      com_closesocket(socket_fd);

      /* Reset socket to free this entry.  */
      nx_driver_sockets[i].socket_ptr = NX_NULL;
      status = NX_SUCCESS;
      break;

    case NX_TCPIP_OFFLOAD_UDP_SOCKET_BIND:
      /* Note, send data from one port to multiple remotes are not supported.  */
      /* Store the index of driver socket.  */
      ((NX_UDP_SOCKET *)socket_ptr) -> nx_udp_socket_tcpip_offload_context = (VOID *)i;

      /* Reset the remote port to indicate the socket is not connected yet.  */
      nx_driver_sockets[i].local_port = local_port;
      nx_driver_sockets[i].remote_port = 0;
      nx_driver_sockets[i].protocol = NX_PROTOCOL_UDP;

      /* Bind to local IP address. */
      local_addr.sin_family = COM_AF_INET;
      local_addr.sin_port = COM_HTONS(local_port);
      local_addr.sin_addr.s_addr = COM_INADDR_ANY;
      ret = com_bind(socket_fd, (com_sockaddr_t *)&local_addr, sizeof(local_addr));
      if (ret < 0)
      {
        return (NX_NOT_SUCCESSFUL);
      }

#ifdef NX_DEBUG
      printf("UDP socket %u bind to port: %u\r\n", i, local_port);
#endif

      status = NX_SUCCESS;
      break;

    case NX_TCPIP_OFFLOAD_UDP_SOCKET_UNBIND:
      i = (UINT)(((NX_UDP_SOCKET *)socket_ptr) -> nx_udp_socket_tcpip_offload_context);

      /* Disconnect.  */
      socket_fd = nx_driver_sockets[i].socket_fd;
      com_closesocket(socket_fd);

#ifdef NX_DEBUG
      printf("UDP socket %u unbind port: %u\r\n", i, local_port);
#endif

      /* Reset socket to free this entry.  */
      nx_driver_sockets[i].socket_ptr = NX_NULL;
      status = NX_SUCCESS;
      break;

    case NX_TCPIP_OFFLOAD_UDP_SOCKET_SEND:
      i = (UINT)(((NX_UDP_SOCKET *)socket_ptr) -> nx_udp_socket_tcpip_offload_context);

#ifndef NX_DISABLE_PACKET_CHAIN
      if (packet_ptr -> nx_packet_next)
      {

        /* Limitation in this driver. UDP packet must be in one packet.  */
        return (NX_NOT_SUCCESSFUL);
      }
#endif /* NX_DISABLE_PACKET_CHAIN */

      /* Set remote address.  */
      remote_addr.sin_family = COM_AF_INET;
      remote_addr.sin_port = COM_HTONS(*remote_port);
      remote_addr.sin_addr.s_addr = COM_HTONL(remote_ip -> nxd_ip_address.v4);

      /* Send data.  */
      socket_fd = nx_driver_sockets[i].socket_fd;
      ret = com_sendto(socket_fd, packet_ptr -> nx_packet_prepend_ptr,
                       packet_ptr -> nx_packet_length, 0,
                       (com_sockaddr_t *)&remote_addr, sizeof(remote_addr));
      if (ret != (INT)packet_ptr -> nx_packet_length)
      {
        return (NX_NOT_SUCCESSFUL);
      }

#ifdef NX_DEBUG
      // printf("UDP socket %u send %u bytes\r\n", i, sent_size);
#endif

      /* Release the packet.  */
      nx_packet_transmit_release(packet_ptr);
      status = NX_SUCCESS;
      break;

    case NX_TCPIP_OFFLOAD_TCP_SOCKET_SEND:
      i = (UINT)(((NX_TCP_SOCKET *)socket_ptr) -> nx_tcp_socket_tcpip_offload_context);

      socket_fd = nx_driver_sockets[i].socket_fd;

      /* Initialize the current packet to the input packet pointer.  */
      current_packet =  packet_ptr;

      /* Loop to send the packet.  */
      while (current_packet)
      {

        /* Calculate current packet size. */
        packet_size = (ULONG)(current_packet -> nx_packet_append_ptr - current_packet -> nx_packet_prepend_ptr);

        /* Send data.  */
        ret = com_send(socket_fd, packet_ptr -> nx_packet_prepend_ptr,
                       packet_size, 0);
        if (ret != (INT)packet_size)
        {
          /* Close TCP connection. */
          _nx_tcp_socket_driver_packet_receive(socket_ptr, NX_NULL);
          nx_driver_sockets[i].tcp_connected = NX_FALSE;
          return (NX_NOT_SUCCESSFUL);
        }

#ifndef NX_DISABLE_PACKET_CHAIN
        /* We have crossed the packet boundary.  Move to the next packet structure.  */
        current_packet =  current_packet -> nx_packet_next;
#else
        /* End of the loop.  */
        current_packet = NX_NULL;
#endif /* NX_DISABLE_PACKET_CHAIN */
      }

#ifdef NX_DEBUG
      // printf("TCP socket %u send %u bytes\r\n", i, packet_ptr -> nx_packet_length);
#endif

      /* Release the packet.  */
      nx_packet_transmit_release(packet_ptr);
      status = NX_SUCCESS;
      break;

    default:
      break;
  }

  return (status);
}

/**
  * @brief  Callback called by driver to update nx_interface_link_up
  * @author MCD Application Team - STMicroelectonics.
  * @param  input: event_type     - event type (must be CA_IP_INFO_EVENT)
  * @param  input: p_ip_info      - ip information pointer
  * @param  input: p_callback_ctx - callback context pointer (unused)
  * @retval none
  */
static VOID _nx_driver_cellular_ip_info_cbk(ca_event_type_t event_type,
                                            const cellular_ip_info_t *const p_ip_info,
                                            void *const p_callback_ctx)
{
  ULONG ip_address;
  ULONG network_mask;

  NX_PARAMETER_NOT_USED(p_callback_ctx);

  if ((event_type == CA_IP_INFO_EVENT)  && (p_ip_info != NULL))
  {
    /* If IP address is not null then it means Data is ready */
    if (p_ip_info->ip_addr.addr != 0U)
    {
      /* Data is ready */
      ip_address = p_ip_info->ip_addr.addr;
      NX_CHANGE_ULONG_ENDIAN(ip_address);
      network_mask = 0xFFFFFFFF;
      nx_ip_interface_address_set(nx_driver_information.nx_driver_information_ip_ptr,
                                  nx_driver_information.nx_driver_information_interface->nx_interface_index,
                                  ip_address, network_mask);
      nx_ip_gateway_address_set(nx_driver_information.nx_driver_information_ip_ptr, ip_address);

      /* Output cellular interface. */
      printf("Cellular interface:\r\n");
      printf("> IP Address: %d.%d.%d.%d\r\n",
             COM_IP4_ADDR1(&(p_ip_info->ip_addr)), COM_IP4_ADDR2(&(p_ip_info->ip_addr)),
             COM_IP4_ADDR3(&(p_ip_info->ip_addr)), COM_IP4_ADDR4(&(p_ip_info->ip_addr)));
#ifdef NX_DEBUG
      printf("Data is ready\n\r");
#endif
      nx_driver_information.nx_driver_information_interface->nx_interface_link_up = NX_TRUE;
    }
    else
    {
#ifdef NX_DEBUG
      printf("Data is NOT ready\n\r");
#endif
      nx_driver_information.nx_driver_information_interface->nx_interface_link_up = NX_FALSE;
      ip_address = 0x00000000U;
      network_mask = 0xFFFFFFFFU;
      nx_ip_interface_address_set(nx_driver_information.nx_driver_information_ip_ptr,
                                  nx_driver_information.nx_driver_information_interface->nx_interface_index,
                                  ip_address, network_mask);
      nx_ip_gateway_address_set(nx_driver_information.nx_driver_information_ip_ptr, ip_address);
    }

    /* Notify NetXDuo IP. */
    _nx_ip_driver_link_status_event(nx_driver_information.nx_driver_information_ip_ptr,
                                    nx_driver_information.nx_driver_information_interface->nx_interface_index);
  }
}

/****** DRIVER SPECIFIC ****** Start of part/vendor specific internal driver functions.  */


/**************************************************************************/
/*                                                                        */
/*  FUNCTION                                               RELEASE        */
/*                                                                        */
/*    _nx_driver_hardware_initialize                    x86/Generic       */
/*                                                           6.x          */
/*  AUTHOR                                                                */
/*                                                                        */
/*    Yuxin Zhou, Microsoft Corporation                                   */
/*                                                                        */
/*  DESCRIPTION                                                           */
/*                                                                        */
/*    This function processes hardware-specific initialization.           */
/*                                                                        */
/*  INPUT                                                                 */
/*                                                                        */
/*    driver_req_ptr                        Driver request pointer        */
/*                                                                        */
/*  OUTPUT                                                                */
/*                                                                        */
/*    status                                [NX_SUCCESS|NX_NOT_SUCCESSFUL]*/
/*                                                                        */
/*  CALLS                                                                 */
/*                                                                        */
/*    tx_thread_info_get                    Get thread information        */
/*    tx_thread_create                      Create driver thread          */
/*                                                                        */
/*  CALLED BY                                                             */
/*                                                                        */
/*    _nx_driver_initialize                 Driver initialize processing  */
/*                                                                        */
/*  RELEASE HISTORY                                                       */
/*                                                                        */
/*    DATE              NAME                      DESCRIPTION             */
/*                                                                        */
/*  xx-xx-xxxx     Yuxin Zhou               Initial Version 6.x           */
/*  13-01-2022     STMicrolectronics.       Change datacache registration */
/*                                          to cellular control service   */
/*                                                                        */
/**************************************************************************/
static UINT  _nx_driver_hardware_initialize(NX_IP_DRIVER *driver_req_ptr)
{
  UINT status;
  UINT priority = 0;

  if (nx_driver_information.nx_driver_datacache_registered)
  {
    return (NX_SUCCESS);
  }

  /* Get priority of IP thread.  */
  tx_thread_info_get(tx_thread_identify(), NX_NULL, NX_NULL, NX_NULL, &priority,
                     NX_NULL, NX_NULL, NX_NULL, NX_NULL);

  /* Create the driver thread.  */
  /* The priority of network thread is lower than IP thread.  */
  status = tx_thread_create(&nx_driver_thread, "Driver Thread", _nx_driver_thread_entry, 0,
                            nx_driver_thread_stack, NX_DRIVER_STACK_SIZE,
                            priority + 1, priority + 1,
                            TX_NO_TIME_SLICE, TX_DONT_START);

  if (status)
  {
    return (status);
  }

  /* Cellular components statical init */
  cellular_init();

  /* Cellular initialization done - Registration to services is OK */
  /* Registration to Cellular: needs to know when IP is obtained, means Data ready/not ready */
  if (cellular_ip_info_cb_registration(_nx_driver_cellular_ip_info_cbk, (void *) NULL) == CELLULAR_SUCCESS)
  {
    nx_driver_information.nx_driver_datacache_registered = NX_TRUE;
  }
  else
  {
    status = NX_NOT_SUCCESSFUL;
  }

  /* Return success!  */
  return (status);
}


/**************************************************************************/
/*                                                                        */
/*  FUNCTION                                               RELEASE        */
/*                                                                        */
/*    _nx_driver_hardware_enable                        x86/Generic       */
/*                                                           6.x          */
/*  AUTHOR                                                                */
/*                                                                        */
/*    Yuxin Zhou, Microsoft Corporation                                   */
/*                                                                        */
/*  DESCRIPTION                                                           */
/*                                                                        */
/*    This function processes hardware-specific link enable requests.     */
/*                                                                        */
/*  INPUT                                                                 */
/*                                                                        */
/*    driver_req_ptr                        Driver request pointer        */
/*                                                                        */
/*  OUTPUT                                                                */
/*                                                                        */
/*    status                                [NX_SUCCESS|NX_NOT_SUCCESSFUL]*/
/*                                                                        */
/*  CALLS                                                                 */
/*                                                                        */
/*    tx_thread_reset                       Reset driver thread           */
/*    tx_thread_resume                      Resume driver thread          */
/*                                                                        */
/*  CALLED BY                                                             */
/*                                                                        */
/*    _nx_driver_enable                     Driver link enable processing */
/*                                                                        */
/*  RELEASE HISTORY                                                       */
/*                                                                        */
/*    DATE              NAME                      DESCRIPTION             */
/*                                                                        */
/*  xx-xx-xxxx     Yuxin Zhou               Initial Version 6.x           */
/*                                                                        */
/**************************************************************************/
static UINT  _nx_driver_hardware_enable(NX_IP_DRIVER *driver_req_ptr)
{
  tx_thread_reset(&nx_driver_thread);
  tx_thread_resume(&nx_driver_thread);

  /* Return success!  */
  return (NX_SUCCESS);
}


/**************************************************************************/
/*                                                                        */
/*  FUNCTION                                               RELEASE        */
/*                                                                        */
/*    _nx_driver_hardware_disable                       x86/Generic       */
/*                                                           6.x          */
/*  AUTHOR                                                                */
/*                                                                        */
/*    Yuxin Zhou, Microsoft Corporation                                   */
/*                                                                        */
/*  DESCRIPTION                                                           */
/*                                                                        */
/*    This function processes hardware-specific link disable requests.    */
/*                                                                        */
/*  INPUT                                                                 */
/*                                                                        */
/*    driver_req_ptr                        Driver request pointer        */
/*                                                                        */
/*  OUTPUT                                                                */
/*                                                                        */
/*    status                                [NX_SUCCESS|NX_NOT_SUCCESSFUL]*/
/*                                                                        */
/*  CALLS                                                                 */
/*                                                                        */
/*    tx_thread_suspend                     Suspend driver thread         */
/*    tx_thread_terminate                   Terminate driver thread       */
/*                                                                        */
/*  CALLED BY                                                             */
/*                                                                        */
/*    _nx_driver_disable                    Driver link disable processing*/
/*                                                                        */
/*  RELEASE HISTORY                                                       */
/*                                                                        */
/*    DATE              NAME                      DESCRIPTION             */
/*                                                                        */
/*  xx-xx-xxxx     Yuxin Zhou               Initial Version 6.x           */
/*                                                                        */
/**************************************************************************/
static UINT  _nx_driver_hardware_disable(NX_IP_DRIVER *driver_req_ptr)
{
  UINT i;

  /* Reset all sockets.  */
  for (i = 0; i < NX_DRIVER_SOCKETS_MAXIMUM; i++)
  {
    if (nx_driver_sockets[i].socket_ptr)
    {

      /* Disconnect.  */
      com_closesocket(nx_driver_sockets[i].socket_fd);
      nx_driver_sockets[i].socket_ptr = NX_NULL;
    }
  }

  tx_thread_suspend(&nx_driver_thread);
  tx_thread_terminate(&nx_driver_thread);

  /* Return success!  */
  return (NX_SUCCESS);
}


/**************************************************************************/
/*                                                                        */
/*  FUNCTION                                               RELEASE        */
/*                                                                        */
/*    _nx_driver_hardware_get_status                    x86/Generic       */
/*                                                           6.x          */
/*  AUTHOR                                                                */
/*                                                                        */
/*    Yuxin Zhou, Microsoft Corporation                                   */
/*                                                                        */
/*  DESCRIPTION                                                           */
/*                                                                        */
/*    This function processes hardware-specific get status requests.      */
/*                                                                        */
/*  INPUT                                                                 */
/*                                                                        */
/*    driver_req_ptr                        Driver request pointer        */
/*                                                                        */
/*  OUTPUT                                                                */
/*                                                                        */
/*    status                                [NX_SUCCESS|NX_NOT_SUCCESSFUL]*/
/*                                                                        */
/*  CALLS                                                                 */
/*                                                                        */
/*    None                                                                */
/*                                                                        */
/*  CALLED BY                                                             */
/*                                                                        */
/*    _nx_driver_get_status                 Driver get status processing  */
/*                                                                        */
/*  RELEASE HISTORY                                                       */
/*                                                                        */
/*    DATE              NAME                      DESCRIPTION             */
/*                                                                        */
/*  xx-xx-xxxx     Yuxin Zhou               Initial Version 6.x           */
/*                                                                        */
/**************************************************************************/
static UINT  _nx_driver_hardware_get_status(NX_IP_DRIVER *driver_req_ptr)
{

  /* Just return the link status from interface.  */
  *(driver_req_ptr -> nx_ip_driver_return_ptr) = driver_req_ptr -> nx_ip_driver_interface -> nx_interface_link_up;

  /* Return success.  */
  return (NX_SUCCESS);
}


#ifdef NX_ENABLE_INTERFACE_CAPABILITY
/**************************************************************************/
/*                                                                        */
/*  FUNCTION                                               RELEASE        */
/*                                                                        */
/*    _nx_driver_hardware_capability_set                x86/Generic       */
/*                                                           6.x          */
/*  AUTHOR                                                                */
/*                                                                        */
/*    Yuxin Zhou, Microsoft Corporation                                   */
/*                                                                        */
/*  DESCRIPTION                                                           */
/*                                                                        */
/*    This function processes hardware-specific capability set requests.  */
/*                                                                        */
/*  INPUT                                                                 */
/*                                                                        */
/*    driver_req_ptr                         Driver request pointer       */
/*                                                                        */
/*  OUTPUT                                                                */
/*                                                                        */
/*    status                                [NX_SUCCESS|NX_NOT_SUCCESSFUL]*/
/*                                                                        */
/*  CALLS                                                                 */
/*                                                                        */
/*    None                                                                */
/*                                                                        */
/*  CALLED BY                                                             */
/*                                                                        */
/*    _nx_driver_capability_set             Capability set processing     */
/*                                                                        */
/*  RELEASE HISTORY                                                       */
/*                                                                        */
/*    DATE              NAME                      DESCRIPTION             */
/*                                                                        */
/*  xx-xx-xxxx     Yuxin Zhou               Initial Version 6.x           */
/*                                                                        */
/**************************************************************************/
static UINT _nx_driver_hardware_capability_set(NX_IP_DRIVER *driver_req_ptr)
{

  return NX_SUCCESS;
}
#endif /* NX_ENABLE_INTERFACE_CAPABILITY */

/**
  * @brief  Configure and start driver
  * @author MCD Application Team - STMicroelectonics.
  * @param  input:  ip_ptr          - ip pointer (unused)
  * @param  output: dns_address_ptr - dns address pointer
  * @retval none
  */
void nx_driver_stm32_cellular_configure(NX_IP *ip_ptr, ULONG *dns_address_ptr)
{
  NX_PARAMETER_NOT_USED(ip_ptr);
  UCHAR ip_addr[4];  /* Used to analyse DNS server address configuration */

  /* 1. Add here special configuration of Cellular Library by calling cellular_control_api.h service. */
  /* such as cellular_set_pdn(), cellular_set_sim_slot_order(), cellular_set_operator() ...           */

  /* 2. DNS value update. */
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
  /* 3. Start Cellular Library. */
  cellular_start();
}

/****** DRIVER SPECIFIC ****** Start of part/vendor specific internal driver functions.  */

