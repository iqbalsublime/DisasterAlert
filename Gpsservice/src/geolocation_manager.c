#include <tizen.h>
#include <stdio.h>
#include <locations.h>
#include <bundle.h>
#include <message_port.h>
#include <efl_extension.h>
#include "gpsservice.h"
#include "geolocation_manager.h"

#define POSITION_UPDATE_INTERVAL 1
#define SATELLITE_UPDATE_INTERVAL 5
#define CHAR_BUFF_SIZE 20
#define MAX_TIME_DIFF 15
#define SEND_DATA_INTERVAL 5.0

#define MESSAGE_TYPE_POSITION_UPDATE "POSITION_UPDATE"
#define MESSAGE_TYPE_SATELLITES_UPDATE "SATELLITES_UPDATE"
#define MESSAGE_TYPE_CIRCLE_INIT "CIRCLE_INIT"

static struct
{
	location_manager_h manager;

	bool init_data_sent;
} s_geolocation_data = {
	.manager = NULL,

	.init_data_sent = false
};
static bool __send_message(bundle *b);
static bool __send_position_coords(location_coords_s coords);
static bool __send_satellites_count(int s_count);
static void __position_updated_cb(double latitude, double longitude, double altitude, time_t timestamp, void *data);
static void __satellite_updated_cb(int num_of_active, int num_of_inview, time_t timestamp, void *data);
static Eina_Bool __init_data_send_cb(void *data);
static Eina_Bool __init_data_send(void);
static bool __init_data(location_coords_s *init_coords, int *satellites_count);


bool
geolocation_manager_init(void)
{
	bool exists;

	/* Create location manager handle */
	if (location_manager_create(LOCATIONS_METHOD_GPS, &s_geolocation_data.manager) != LOCATIONS_ERROR_NONE) {
		dlog_print(DLOG_ERROR, LOG_TAG, "Failed to create location manager");
		return false;
	}

	/* Register callbacks for position and satellites data update */
	location_error_e pos_cb = location_manager_set_position_updated_cb(s_geolocation_data.manager, __position_updated_cb, POSITION_UPDATE_INTERVAL, NULL);
	location_error_e sat_cb = gps_status_set_satellite_updated_cb(s_geolocation_data.manager, __satellite_updated_cb, SATELLITE_UPDATE_INTERVAL, NULL);

	if (pos_cb != LOCATIONS_ERROR_NONE) {
		dlog_print(DLOG_ERROR, LOG_TAG, "Failed to register callbacks for location manager");
		geolocation_manager_destroy_service();
		return false;
	}

	if (sat_cb != LOCATIONS_ERROR_NONE)
		dlog_print(DLOG_WARN, LOG_TAG, "Failed to get satellites number. Probably you run this sample on the emulator.");

	/* Check state of remote port from gps-consumer */
	if (message_port_check_remote_port(REMOTE_APP_ID, REMOTE_PORT, &exists) != MESSAGE_PORT_ERROR_NONE) {
		dlog_print(DLOG_ERROR, LOG_TAG, "Failed to check remote port");
		geolocation_manager_destroy_service();
		return false;
	}

	if (!exists)
		dlog_print(DLOG_ERROR, LOG_TAG, "Remote port is not registered");

	/* Start location service */
	location_manager_start(s_geolocation_data.manager);

	/* Send initial data to gps-consumer port */
	 if (!__init_data_send()) {
		 dlog_print(DLOG_ERROR, LOG_TAG, "Failed to send init data - create timer to periodically try to send data");
		 ecore_timer_add(SEND_DATA_INTERVAL, __init_data_send_cb, NULL);
	 }

	return true;
}


void
geolocation_manager_stop_service(void)
{
	location_manager_stop(s_geolocation_data.manager);
}


void
geolocation_manager_destroy_service(void)
{
	location_manager_unset_position_updated_cb(s_geolocation_data.manager);
	gps_status_unset_satellite_updated_cb(s_geolocation_data.manager);
	location_manager_stop(s_geolocation_data.manager);
	location_manager_destroy(s_geolocation_data.manager);


	s_geolocation_data.manager = NULL;
}

static bool
__send_message(bundle *b)
{
	if (!b) {
		dlog_print(DLOG_ERROR, LOG_TAG, "Can not send message, the bundle is NULL");
		return false;
	}

	/* Send message to specified remote port */
	int ret = message_port_send_message(REMOTE_APP_ID, REMOTE_PORT, b);

	if (ret != MESSAGE_PORT_ERROR_NONE) {
		dlog_print(DLOG_ERROR, LOG_TAG, "Failed to send message: error %d", ret);
		return false;
	}

	return true;
}

static bool
__send_position_coords(location_coords_s coords)
{
	bundle *b = bundle_create();
	char latitude_str[CHAR_BUFF_SIZE], longitude_str[CHAR_BUFF_SIZE];

	if (!b) {
		dlog_print(DLOG_ERROR, LOG_TAG, "Failed to create bundle, the coords will not be sent");
		return false;
	}

	snprintf(latitude_str, CHAR_BUFF_SIZE, "%f", coords.latitude);
	snprintf(longitude_str, CHAR_BUFF_SIZE, "%f", coords.longitude);

	bundle_add_str(b, "msg_type", MESSAGE_TYPE_POSITION_UPDATE);
	bundle_add_str(b, "latitude", latitude_str);
	bundle_add_str(b, "longitude", longitude_str);

	bool ret = __send_message(b);

	bundle_free(b);

	return ret;
}

static bool
__send_satellites_count(int s_count)
{
	bundle *b = bundle_create();
	char count_str[CHAR_BUFF_SIZE];

	if (!b) {
		dlog_print(DLOG_ERROR, LOG_TAG, "Failed to create bundle, the satellites will not be sent");
		return false;
	}

	snprintf(count_str, CHAR_BUFF_SIZE, "%d", s_count);

	bundle_add_str(b, "msg_type", MESSAGE_TYPE_SATELLITES_UPDATE);
	bundle_add_str(b, "satellites_count", count_str);

	bool ret = __send_message(b);

	bundle_free(b);

	return ret;
}

static void
__position_updated_cb(double latitude, double longitude, double altitude, time_t timestamp, void *data)
{
	location_coords_s coords;

	coords.latitude = latitude;
	coords.longitude = longitude;

	time_t curr_timestamp;

	/* Get current time to compare to the last position timestamp */
	time(&curr_timestamp);


	/* Send updated position only if init data has been sent and position update
	 * was registered less than MAX_TIME_DIFF seconds ago */
	if (s_geolocation_data.init_data_sent && curr_timestamp-timestamp < MAX_TIME_DIFF) {

		/* Send position update via message port */
		if (__send_position_coords(coords)) {
			dlog_print(DLOG_INFO, LOG_TAG, "Position updated to %f, %f", latitude, longitude);
		} else {
			dlog_print(DLOG_ERROR, LOG_TAG, "Failed to send position update");
		}
	}
}


static void
__satellite_updated_cb(int num_of_active, int num_of_inview, time_t timestamp, void *data)
{
	/* Send update satellite count only if init data has been sent*/
	if (s_geolocation_data.init_data_sent) {
		/* Send satellite count update via message port */
		if (__send_satellites_count(num_of_inview)) {
			dlog_print(DLOG_INFO, LOG_TAG, "Satellite count updated: inview %d", num_of_inview);
		} else {
			dlog_print(DLOG_ERROR, LOG_TAG, "Failed to send satellite count update");
		}
	}
}

static Eina_Bool
__init_data_send_cb(void *data)
{
	if (__init_data_send())
		return ECORE_CALLBACK_CANCEL;

	return ECORE_CALLBACK_RENEW;
}

static Eina_Bool
__init_data_send(void)
{
	location_coords_s init_coords;
	int satellites_count = 0;

	/* Get initial position and satellites count */
	if (!__init_data(&init_coords, &satellites_count)) {
		dlog_print(DLOG_ERROR, LOG_TAG, "Failed to initialize geolocation data");
		return EINA_FALSE;
	}

	/* Send initial data to consumer application */
	if (!__send_satellites_count(satellites_count) || !__send_position_coords(init_coords))
		return EINA_FALSE;

	s_geolocation_data.init_data_sent = true;

	return EINA_TRUE;
}

static bool
__init_data(location_coords_s *init_coords, int *satellites_count)
{
	/* Additional variables have to be declared to ensure proper local_manager and gps_status functions execution */
	double altitude;
	time_t timestamp;
	time_t curr_timestamp;
	int num_of_active;

	/* Get last location information*/
	if (location_manager_get_last_position(s_geolocation_data.manager, &altitude, &init_coords->latitude, &init_coords->longitude, &timestamp) != LOCATIONS_ERROR_NONE) {
		dlog_print(DLOG_ERROR, LOG_TAG, "Failed to get last location");
		return false;
	}
	dlog_print(DLOG_INFO, LOG_TAG, "Initial position: latitude %f, longitude %f", init_coords->latitude, init_coords->longitude);

	/* Get current time and compare it to the last position timestamp */
	time(&curr_timestamp);

	if (curr_timestamp-timestamp > MAX_TIME_DIFF) {
		dlog_print(DLOG_ERROR, LOG_TAG, "Last position updated over 15 seconds ago - no longer valid");
		return false;
	}

	/* Get initial count of satellites in view */
	location_error_e ret = gps_status_get_satellite(s_geolocation_data.manager, &num_of_active, satellites_count, &timestamp);
	if (ret != LOCATIONS_ERROR_NONE) {
		dlog_print(DLOG_ERROR, LOG_TAG, "Failed to get satellite data [%d]", ret);
		/* Satellite data is not supported on Tizen Emulator - satellites count remains at 0 */
	}
	dlog_print(DLOG_INFO, LOG_TAG, "Initial satelite data: number of satellites inview: %d", *satellites_count);

	return true;
}
