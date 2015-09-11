#include <stdio.h>

#include <libbacnet/address.h>
#include <libbacnet/device.h>
#include <libbacnet/handlers.h>
#include <libbacnet/datalink.h>
#include <libbacnet/bvlc.h>
#include <libbacnet/client.h>
#include <libbacnet/txbuf.h>
#include <libbacnet/tsm.h>
#include <libbacnet/bactext.h>
#include "bacnet_namespace.h"

#define TARGET_DEVICE		    120
#define GET_OBJECT_LIST		    0
#define GET_AI			    1

#if GET_OBJECT_LIST
#define TARGET_OBJECT_TYPE	    bacnet_OBJECT_DEVICE
#define TARGET_OBJECT_INSTANCE	    TARGET_DEVICE
#define TARGET_OBJECT_PROPERTY	    bacnet_PROP_OBJECT_LIST
#define TARGET_OBJECT_INDEX	    BACNET_ARRAY_ALL
#endif

#if GET_AI
#define TARGET_OBJECT_TYPE	    bacnet_OBJECT_ANALOG_INPUT
#define TARGET_OBJECT_INSTANCE	    0
#define TARGET_OBJECT_PROPERTY	    bacnet_PROP_PRESENT_VALUE
#define TARGET_OBJECT_INDEX	    BACNET_ARRAY_ALL
#endif

#define BACNET_PORT		    0xBAC0
#define BACNET_INTERFACE	    "lo"
#define BACNET_DATALINK_TYPE	    "bvlc"
#define BACNET_SELECT_TIMEOUT_MS    1	    /* ms */

#define RUN_AS_BBMD_CLIENT	    0

#if RUN_AS_BBMD_CLIENT
#define BACNET_BBMD_PORT	    0xBAC0
#define BACNET_BBMD_ADDRESS	    "127.0.0.1"
#define BACNET_BBMD_TTL		    90
#endif

static bool found_server;
static BACNET_ADDRESS target_address;
static int request_invoke_id;
static pthread_mutex_t timer_lock = PTHREAD_MUTEX_INITIALIZER;

static bacnet_object_functions_t client_objects[] = {
    {bacnet_OBJECT_DEVICE,
	    NULL,
	    bacnet_Device_Count,
	    bacnet_Device_Index_To_Instance,
	    bacnet_Device_Valid_Object_Instance_Number,
	    bacnet_Device_Object_Name,
	    bacnet_Device_Read_Property_Local,
	    bacnet_Device_Write_Property_Local,
	    bacnet_Device_Property_Lists,
	    bacnet_DeviceGetRRInfo,
	    NULL, /* Iterator */
	    NULL, /* Value_Lists */
	    NULL, /* COV */
	    NULL, /* COV Clear */
	    NULL  /* Intrinsic Reporting */
    },
    {MAX_BACNET_OBJECT_TYPE}
};

static void register_with_bbmd(void) {
#if RUN_AS_BBMD_CLIENT
    bacnet_bvlc_register_with_bbmd(
	    bacnet_bip_getaddrbyname(BACNET_BBMD_ADDRESS), 
	    htons(BACNET_BBMD_PORT),
	    BACNET_BBMD_TTL);
#endif
}

static void *minute_tick(void *arg) {
    while (1) {
	pthread_mutex_lock(&timer_lock);

	/* Expire addresses once the TTL has expired */
	bacnet_address_cache_timer(60);

	/* Re-register with BBMD once BBMD TTL has expired */
	register_with_bbmd();

	/* Update addresses for notification class recipient list 
	 * Requred for INTRINSIC_REPORTING
	 * bacnet_Notification_Class_find_recipient(); */

	/* Sleep for 1 minute */
	pthread_mutex_unlock(&timer_lock);
	sleep(60);
    }
    return arg;
}

static void *second_tick(void *arg) {
    while (1) {
	pthread_mutex_lock(&timer_lock);

	/* Keep searching for server */
	if (!found_server) bacnet_Send_WhoIs(TARGET_DEVICE, TARGET_DEVICE);

	/* Invalidates stale BBMD foreign device table entries */
	bacnet_bvlc_maintenance_timer(1);

	/* Transaction state machine: Responsible for retransmissions and ack
	 * checking for confirmed services */
	bacnet_tsm_timer_milliseconds(1000);

	/* Re-enables communications after DCC_Time_Duration_Seconds
	 * Required for SERVICE_CONFIRMED_DEVICE_COMMUNICATION_CONTROL
	 * bacnet_dcc_timer_seconds(1); */

	/* State machine for load control object
	 * Required for OBJECT_LOAD_CONTROL
	 * bacnet_Load_Control_State_Machine_Handler(); */

	/* Expires any COV subscribers that have finite lifetimes
	 * Required for SERVICE_CONFIRMED_SUBSCRIBE_COV
	 * bacnet_handler_cov_timer_seconds(1); */

	/* Monitor Trend Log uLogIntervals and fetch properties
	 * Required for OBJECT_TRENDLOG
	 * bacnet_trend_log_timer(1); */
	
	/* Run [Object_Type]_Intrinsic_Reporting() for all objects in device
	 * Required for INTRINSIC_REPORTING
	 * bacnet_Device_local_reporting(); */

	/* Sleep for 1 second */
	pthread_mutex_unlock(&timer_lock);
	sleep(1);
    }
    return arg;
}

static void ms_tick(void) {
    /* Updates change of value COV subscribers.
     * Required for SERVICE_CONFIRMED_SUBSCRIBE_COV
     * bacnet_handler_cov_task(); */
}

#define BN_UNC(service, handler) \
    bacnet_apdu_set_unconfirmed_handler(		\
		    SERVICE_UNCONFIRMED_##service,	\
		    bacnet_handler_##handler)
#define BN_CON(service, handler) \
    bacnet_apdu_set_confirmed_handler(			\
		    SERVICE_CONFIRMED_##service,	\
		    bacnet_handler_##handler)
#define BN_CON_ACK(service, handler) \
    bacnet_apdu_set_confirmed_ack_handler(		\
		    SERVICE_CONFIRMED_##service,	\
		    handler)
#define BN_ERR(service, handler) \
    bacnet_apdu_set_error_handler(			\
		    SERVICE_CONFIRMED_##service,	\
		    handler)

static void abort_handler(
		BACNET_ADDRESS *src,
		uint8_t invoke_id,
		uint8_t abort_reason,
		bool server) {
    if (bacnet_address_match(&target_address, src) && 
		    (invoke_id == request_invoke_id)) {
	fprintf(stderr, "BACnet Abort: %s\n",
	    bactext_abort_reason_name(abort_reason));
	found_server = 0;
    }
}

static void reject_handler(
		BACNET_ADDRESS *src,
		uint8_t invoke_id,
		uint8_t reject_reason) {
    if (bacnet_address_match(&target_address, src) && 
		    (invoke_id == request_invoke_id)) {
	fprintf(stderr, "BACnet Reject: %s\n",
	    bactext_reject_reason_name(reject_reason));
	found_server = 0;
    }
}

static void read_property_err(
		BACNET_ADDRESS *src,
		uint8_t invoke_id,
		BACNET_ERROR_CLASS error_class,
		BACNET_ERROR_CODE error_code) {
    if (bacnet_address_match(&target_address, src) && 
		    (invoke_id == request_invoke_id)) {
	fprintf(stderr, "BACnet Error: %s: %s\n",
	    bactext_error_class_name(error_class),
	    bactext_error_code_name(error_code));
	found_server = 0;
    }
}

static void read_property_ack(
		uint8_t *service_request,
		uint16_t service_len,
		BACNET_ADDRESS *src,
		BACNET_CONFIRMED_SERVICE_ACK_DATA *service_data) {
    int len;
    BACNET_READ_PROPERTY_DATA data;

    if (bacnet_address_match(&target_address, src) &&
		    (service_data->invoke_id == request_invoke_id)) {
	len = bacnet_rp_ack_decode_service_request(
			service_request, service_len, &data);
	if (len < 0) {
	    fprintf(stderr, 
			"Read Property ACK service request decode failed\n");
	} else {
	    bacnet_rp_ack_print_data(&data);
	}
    }
}

void *read_prop_thread(void *arg) {
    while (1) {

	usleep(100000);

	if (!found_server) continue;
	    
	if (!request_invoke_id)
	    request_invoke_id = bacnet_Send_Read_Property_Request(
				    TARGET_DEVICE,
				    TARGET_OBJECT_TYPE,
				    TARGET_OBJECT_INSTANCE,
				    TARGET_OBJECT_PROPERTY,
				    TARGET_OBJECT_INDEX);

	else if (bacnet_tsm_invoke_id_free(request_invoke_id)) {
	    /* Transaction is finished */
	    request_invoke_id = 0;
	} else if (bacnet_tsm_invoke_id_failed(request_invoke_id)) {
	    fprintf(stderr, "Error: TSM Timeout\n");
	    bacnet_tsm_free_invoke_id(request_invoke_id);
	    request_invoke_id = 0;
	    found_server = 0;
	}
    }

    return arg;
}

int main(int argc, char **argv) {
    uint8_t rx_buf[bacnet_MAX_MPDU];
    uint16_t pdu_len;
    BACNET_ADDRESS src;
    unsigned max_apdu;
    pthread_t read_prop_thread_id, minute_tick_id, second_tick_id;

    bacnet_Device_Set_Object_Instance_Number(BACNET_MAX_INSTANCE);
    bacnet_address_init();

    /* Setup device objects */
    bacnet_Device_Init(client_objects);
    BN_UNC(I_AM, i_am_bind);
    BN_CON_ACK(READ_PROPERTY, read_property_ack);
    BN_ERR(READ_PROPERTY, read_property_err);
    bacnet_apdu_set_abort_handler(abort_handler);
    bacnet_apdu_set_reject_handler(reject_handler);

    bacnet_BIP_Debug = true;
    bacnet_bip_set_port(htons(BACNET_PORT));
    bacnet_datalink_set(BACNET_DATALINK_TYPE);
    bacnet_datalink_init(BACNET_INTERFACE);
    atexit(bacnet_datalink_cleanup);
    memset(&src, 0, sizeof(src));

    register_with_bbmd();

    pthread_create(&read_prop_thread_id, 0, read_prop_thread, NULL);
    pthread_create(&minute_tick_id, 0, minute_tick, NULL);
    pthread_create(&second_tick_id, 0, second_tick, NULL);
    
    while (1) {
	if (!found_server) found_server = bacnet_address_bind_request(
			TARGET_DEVICE, &max_apdu, &target_address);

	pdu_len = bacnet_datalink_receive(
		    &src, rx_buf, bacnet_MAX_MPDU, BACNET_SELECT_TIMEOUT_MS);

	if (pdu_len) {
	    /* May call any registered handler.
	     * Thread safety: May block, however we still need to guarantee
	     * atomicity with the timers, so hold the lock anyway */
	    pthread_mutex_lock(&timer_lock);
	    bacnet_npdu_handler(&src, rx_buf, pdu_len);
	    pthread_mutex_unlock(&timer_lock);
	}

	ms_tick();
    }

    return 0;
}
