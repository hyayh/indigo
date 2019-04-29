// Copyright (c) 2019 CloudMakers, s. r. o.
// All rights reserved.
//
// You can use this software under the terms of 'INDIGO Astronomy
// open-source license' (see LICENSE.md).
//
// THIS SOFTWARE IS PROVIDED BY THE AUTHORS 'AS IS' AND ANY EXPRESS
// OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
// WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY
// DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
// DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE
// GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
// WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
// NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
// SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

// version history
// 2.0 by Peter Polakovic <peter.polakovic@cloudmakers.eu>

/** INDIGO Guider agent
 \file indigo_agent_guider.c
 */

#define DRIVER_VERSION 0x0001
#define DRIVER_NAME	"indigo_agent_guider"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <math.h>
#include <assert.h>
#include <pthread.h>

#include "indigo_driver_xml.h"
#include "indigo_filter.h"
#include "indigo_ccd_driver.h"
#include "indigo_guider_utils.h"
#include "indigo_agent_guider.h"

#define PI (3.14159265358979)
#define PI2 (PI/2)

#define DEVICE_PRIVATE_DATA										((agent_private_data *)device->private_data)
#define CLIENT_PRIVATE_DATA										((agent_private_data *)FILTER_CLIENT_CONTEXT->device->private_data)

#define AGENT_GUIDER_DETECTION_MODE_PROPERTY	(DEVICE_PRIVATE_DATA->agent_guider_detection_mode_property)
#define AGENT_GUIDER_DETECTION_DONUTS_ITEM  	(AGENT_GUIDER_DETECTION_MODE_PROPERTY->items+0)
#define AGENT_GUIDER_DETECTION_CENTROID_ITEM  (AGENT_GUIDER_DETECTION_MODE_PROPERTY->items+1)

#define AGENT_GUIDER_DEC_MODE_PROPERTY				(DEVICE_PRIVATE_DATA->agent_guider_dec_mode_property)
#define AGENT_GUIDER_DEC_MODE_BOTH_ITEM    		(AGENT_GUIDER_DEC_MODE_PROPERTY->items+0)
#define AGENT_GUIDER_DEC_MODE_NORTH_ITEM    	(AGENT_GUIDER_DEC_MODE_PROPERTY->items+1)
#define AGENT_GUIDER_DEC_MODE_SOUTH_ITEM    	(AGENT_GUIDER_DEC_MODE_PROPERTY->items+2)
#define AGENT_GUIDER_DEC_MODE_NONE_ITEM    		(AGENT_GUIDER_DEC_MODE_PROPERTY->items+3)

#define AGENT_START_PROCESS_PROPERTY					(DEVICE_PRIVATE_DATA->agent_start_process_property)
#define AGENT_GUIDER_START_PREVIEW_ITEM  			(AGENT_START_PROCESS_PROPERTY->items+0)
#define AGENT_GUIDER_START_CALIBRATION_ITEM 	(AGENT_START_PROCESS_PROPERTY->items+1)
#define AGENT_GUIDER_START_GUIDING_ITEM 			(AGENT_START_PROCESS_PROPERTY->items+2)

#define AGENT_ABORT_PROCESS_PROPERTY					(DEVICE_PRIVATE_DATA->agent_abort_process_property)
#define AGENT_ABORT_PROCESS_ITEM      				(AGENT_ABORT_PROCESS_PROPERTY->items+0)

#define AGENT_GUIDER_SETTINGS_PROPERTY				(DEVICE_PRIVATE_DATA->agent_settings_property)
#define AGENT_GUIDER_SETTINGS_EXPOSURE_ITEM   (AGENT_GUIDER_SETTINGS_PROPERTY->items+0)
#define AGENT_GUIDER_SETTINGS_STEP_ITEM       (AGENT_GUIDER_SETTINGS_PROPERTY->items+1)
#define AGENT_GUIDER_SETTINGS_BL_STEPS_ITEM  	(AGENT_GUIDER_SETTINGS_PROPERTY->items+2)
#define AGENT_GUIDER_SETTINGS_BL_DRIFT_ITEM  	(AGENT_GUIDER_SETTINGS_PROPERTY->items+3)
#define AGENT_GUIDER_SETTINGS_CAL_STEPS_ITEM  (AGENT_GUIDER_SETTINGS_PROPERTY->items+4)
#define AGENT_GUIDER_SETTINGS_CAL_DRIFT_ITEM  (AGENT_GUIDER_SETTINGS_PROPERTY->items+5)
#define AGENT_GUIDER_SETTINGS_AGG_RA_ITEM  		(AGENT_GUIDER_SETTINGS_PROPERTY->items+6)
#define AGENT_GUIDER_SETTINGS_AGG_DEC_ITEM  	(AGENT_GUIDER_SETTINGS_PROPERTY->items+7)
#define AGENT_GUIDER_SETTINGS_MIN_ERR_ITEM  	(AGENT_GUIDER_SETTINGS_PROPERTY->items+8)
#define AGENT_GUIDER_SETTINGS_MIN_PULSE_ITEM  (AGENT_GUIDER_SETTINGS_PROPERTY->items+9)
#define AGENT_GUIDER_SETTINGS_MAX_PULSE_ITEM  (AGENT_GUIDER_SETTINGS_PROPERTY->items+10)
#define AGENT_GUIDER_SETTINGS_ANGLE_ITEM      (AGENT_GUIDER_SETTINGS_PROPERTY->items+11)
#define AGENT_GUIDER_SETTINGS_BACKLASH_ITEM   (AGENT_GUIDER_SETTINGS_PROPERTY->items+12)
#define AGENT_GUIDER_SETTINGS_SPEED_RA_ITEM   (AGENT_GUIDER_SETTINGS_PROPERTY->items+13)
#define AGENT_GUIDER_SETTINGS_SPEED_DEC_ITEM  (AGENT_GUIDER_SETTINGS_PROPERTY->items+14)


#define AGENT_GUIDER_STATS_PROPERTY						(DEVICE_PRIVATE_DATA->agent_stats_property)
#define AGENT_GUIDER_STATS_PHASE_ITEM      		(AGENT_GUIDER_STATS_PROPERTY->items+0)
#define AGENT_GUIDER_STATS_FRAME_ITEM      		(AGENT_GUIDER_STATS_PROPERTY->items+1)
#define AGENT_GUIDER_STATS_DRIFT_X_ITEM      	(AGENT_GUIDER_STATS_PROPERTY->items+2)
#define AGENT_GUIDER_STATS_DRIFT_Y_ITEM      	(AGENT_GUIDER_STATS_PROPERTY->items+3)
#define AGENT_GUIDER_STATS_DRIFT_RA_ITEM      (AGENT_GUIDER_STATS_PROPERTY->items+4)
#define AGENT_GUIDER_STATS_DRIFT_DEC_ITEM     (AGENT_GUIDER_STATS_PROPERTY->items+5)
#define AGENT_GUIDER_STATS_CORR_RA_ITEM      	(AGENT_GUIDER_STATS_PROPERTY->items+6)
#define AGENT_GUIDER_STATS_CORR_DEC_ITEM      (AGENT_GUIDER_STATS_PROPERTY->items+7)
#define AGENT_GUIDER_STATS_RMSE_RA_ITEM      	(AGENT_GUIDER_STATS_PROPERTY->items+8)
#define AGENT_GUIDER_STATS_RMSE_DEC_ITEM      (AGENT_GUIDER_STATS_PROPERTY->items+9)

typedef struct {
	indigo_property *agent_guider_detection_mode_property;
	indigo_property *agent_guider_dec_mode_property;
	indigo_property *agent_start_process_property;
	indigo_property *agent_abort_process_property;
	indigo_property *agent_settings_property;
	indigo_property *agent_stats_property;
	bool properties_defined;
	indigo_frame_digest reference;
	double drift_x, drift_y, drift;
	double rmse_ra_sum, rmse_dec_sum;
	enum { INIT, CLEAR_DEC, CLEAR_RA, MOVE_NORTH, MOVE_SOUTH, MOVE_WEST, MOVE_EAST, FAILED, DONE } phase;
} agent_private_data;

// -------------------------------------------------------------------------------- INDIGO agent common code


static indigo_property *cached_remote_ccd_property(indigo_device *device, char *name) {
	indigo_property **cache = FILTER_DEVICE_CONTEXT->device_property_cache;
	for (int j = 0; j < INDIGO_FILTER_MAX_CACHED_PROPERTIES; j++) {
		if (cache[j] && !strcmp(cache[j]->device, FILTER_DEVICE_CONTEXT->device_name[INDIGO_FILTER_CCD_INDEX]) && !strcmp(cache[j]->name, name)) {
			return cache[j];
		}
	}
	return NULL;
}

static indigo_property_state capture_frame(indigo_device *device) {
	indigo_property *remote_exposure_property = cached_remote_ccd_property(device, CCD_EXPOSURE_PROPERTY_NAME);
	indigo_property *remote_image_property = cached_remote_ccd_property(device, CCD_IMAGE_PROPERTY_NAME);
	if (remote_exposure_property == NULL || remote_image_property == NULL) {
		indigo_send_message(device, "%s: CCD_EXPOSURE_PROPERTY or CCD_IMAGE_PROPERTY not found", GUIDER_AGENT_NAME);
		return INDIGO_ALERT_STATE;
	} else {
		indigo_property *remote_format_property = cached_remote_ccd_property(device, CCD_IMAGE_FORMAT_PROPERTY_NAME);
		for (int i = 0; i < remote_format_property->count; i++) {
			indigo_item *item = remote_format_property->items + i;
			if (item->sw.value && strcmp(item->name, CCD_IMAGE_FORMAT_RAW_ITEM_NAME)) {
				indigo_property *local_format_property = indigo_init_switch_property(NULL, remote_format_property->device, remote_format_property->name, NULL, NULL, INDIGO_OK_STATE, INDIGO_RW_PERM, INDIGO_ONE_OF_MANY_RULE, 1);
				indigo_init_switch_item(local_format_property->items, CCD_IMAGE_FORMAT_RAW_ITEM_NAME, NULL, true);
				indigo_change_property(FILTER_DEVICE_CONTEXT->client, local_format_property);
				indigo_release_property(local_format_property);
			}
		}
		indigo_property *local_exposure_property = indigo_init_number_property(NULL, remote_exposure_property->device, remote_exposure_property->name, NULL, NULL, INDIGO_OK_STATE, INDIGO_RW_PERM, remote_exposure_property->count);
		if (local_exposure_property == NULL) {
			return INDIGO_ALERT_STATE;
		} else {
			memcpy(local_exposure_property, remote_exposure_property, sizeof(indigo_property) + remote_exposure_property->count * sizeof(indigo_item));
			double time = AGENT_GUIDER_SETTINGS_EXPOSURE_ITEM->number.value;
			local_exposure_property->items[0].number.value = time;
			indigo_change_property(FILTER_DEVICE_CONTEXT->client, local_exposure_property);
			for (int i = 0; remote_exposure_property->state != INDIGO_BUSY_STATE && i < 1000; i++)
				usleep(1000);
			if (remote_exposure_property->state != INDIGO_BUSY_STATE) {
				indigo_send_message(device, "%s: CCD_EXPOSURE_PROPERTY didn't become busy in 1s", GUIDER_AGENT_NAME);
				indigo_release_property(local_exposure_property);
				return INDIGO_ALERT_STATE;
			}
			while (remote_exposure_property->state == INDIGO_BUSY_STATE) {
				if (time > 1) {
					usleep(1000000);
					time -= 1;
				} else {
					usleep(10000);
					time -= 0.01;
				}
			}
			if (remote_exposure_property->state == INDIGO_OK_STATE) {
				indigo_raw_header *header = (indigo_raw_header *)(remote_image_property->items->blob.value);
				bool donuts = AGENT_GUIDER_DETECTION_DONUTS_ITEM->sw.value;
				if (header->signature == INDIGO_RAW_MONO8 || header->signature == INDIGO_RAW_MONO16 || header->signature == INDIGO_RAW_RGB24 || header->signature == INDIGO_RAW_RGB48) {
					if (AGENT_GUIDER_STATS_FRAME_ITEM->number.value == 0) {
						indigo_result result;
						indigo_delete_frame_digest(&DEVICE_PRIVATE_DATA->reference);
						if (donuts)
							result = indigo_donuts_frame_digest(header->signature, (void*)header + sizeof(indigo_raw_header), header->width, header->height, &DEVICE_PRIVATE_DATA->reference);
						else
							result = indigo_centroid_frame_digest(header->signature, (void*)header + sizeof(indigo_raw_header), header->width, header->height, &DEVICE_PRIVATE_DATA->reference);
						if (result == INDIGO_OK) {
							AGENT_GUIDER_STATS_FRAME_ITEM->number.value++;
						}
					} else {
						indigo_frame_digest digest;
						indigo_result result;
						if (donuts)
							result = indigo_donuts_frame_digest(header->signature, (void*)header + sizeof(indigo_raw_header), header->width, header->height, &digest);
						else
							result = indigo_centroid_frame_digest(header->signature, (void*)header + sizeof(indigo_raw_header), header->width, header->height, &digest);
						if (result == INDIGO_OK) {
							result = indigo_calculate_drift(&DEVICE_PRIVATE_DATA->reference, &digest, &DEVICE_PRIVATE_DATA->drift_x, &DEVICE_PRIVATE_DATA->drift_y);
							if (result == INDIGO_OK) {
								AGENT_GUIDER_STATS_FRAME_ITEM->number.value++;
								AGENT_GUIDER_STATS_DRIFT_X_ITEM->number.value = round(1000 * DEVICE_PRIVATE_DATA->drift_x) / 1000;
								AGENT_GUIDER_STATS_DRIFT_Y_ITEM->number.value = round(1000 * DEVICE_PRIVATE_DATA->drift_y) / 1000;
								DEVICE_PRIVATE_DATA->drift = sqrt(DEVICE_PRIVATE_DATA->drift_x * DEVICE_PRIVATE_DATA->drift_x + DEVICE_PRIVATE_DATA->drift_y * DEVICE_PRIVATE_DATA->drift_y);
								INDIGO_DRIVER_DEBUG(DRIVER_NAME, "Drift %.4gpx (%.4g, %.4g)", DEVICE_PRIVATE_DATA->drift, DEVICE_PRIVATE_DATA->drift_x, DEVICE_PRIVATE_DATA->drift_y);
							}
						}
						indigo_delete_frame_digest(&digest);
					}
				} else {
					indigo_send_message(device, "%s: Invalid image format, only RAW is supported", GUIDER_AGENT_NAME);
					indigo_release_property(local_exposure_property);
					return INDIGO_ALERT_STATE;
				}
			}
			indigo_release_property(local_exposure_property);
		}
		return remote_exposure_property->state;
	}
}

static indigo_property *cached_remote_guider_property(indigo_device *device, char *name) {
	indigo_property **cache = FILTER_DEVICE_CONTEXT->device_property_cache;
	for (int j = 0; j < INDIGO_FILTER_MAX_CACHED_PROPERTIES; j++) {
		if (cache[j] && !strcmp(cache[j]->device, FILTER_DEVICE_CONTEXT->device_name[INDIGO_FILTER_GUIDER_INDEX]) && !strcmp(cache[j]->name, name)) {
			return cache[j];
		}
	}
	return NULL;
}

static indigo_property_state pulse_guide(indigo_device *device, double ra, double dec) {
	if (ra) {
		indigo_property *remote_guide_property = cached_remote_guider_property(device, GUIDER_GUIDE_RA_PROPERTY_NAME);
		if (remote_guide_property == NULL) {
			indigo_send_message(device, "%s: GUIDER_GUIDE_RA_PROPERTY not found", GUIDER_AGENT_NAME);
			return INDIGO_ALERT_STATE;
		} else {
			indigo_property *local_guide_property = indigo_init_number_property(NULL, remote_guide_property->device, remote_guide_property->name, NULL, NULL, INDIGO_OK_STATE, INDIGO_RW_PERM, remote_guide_property->count);
			if (remote_guide_property == NULL) {
				return INDIGO_ALERT_STATE;
			} else {
				memcpy(local_guide_property, remote_guide_property, sizeof(indigo_property) + remote_guide_property->count * sizeof(indigo_item));
				for (int i = 0; i < local_guide_property->count; i++) {
					indigo_item *item = local_guide_property->items + i;
					if (!strcmp(item->name, GUIDER_GUIDE_WEST_ITEM_NAME)) {
						item->number.value = ra > 0 ? ra * 1000 : 0;
					} else if (!strcmp(item->name, GUIDER_GUIDE_EAST_ITEM_NAME)) {
						item->number.value = ra < 0 ? -ra * 1000 : 0;
					}
				}
				indigo_change_property(FILTER_DEVICE_CONTEXT->client, local_guide_property);
				while (remote_guide_property->state == INDIGO_BUSY_STATE) {
					usleep(50000);
				}
				indigo_release_property(local_guide_property);
			}
		}
	}
	if (dec) {
		indigo_property *remote_guide_property = cached_remote_guider_property(device, GUIDER_GUIDE_DEC_PROPERTY_NAME);
		if (remote_guide_property == NULL) {
			indigo_send_message(device, "%s: GUIDER_GUIDE_DEC_PROPERTY not found", GUIDER_AGENT_NAME);
			return INDIGO_ALERT_STATE;
		} else {
			indigo_property *local_guide_property = indigo_init_number_property(NULL, remote_guide_property->device, remote_guide_property->name, NULL, NULL, INDIGO_OK_STATE, INDIGO_RW_PERM, remote_guide_property->count);
			if (remote_guide_property == NULL) {
				return INDIGO_ALERT_STATE;
			} else {
				memcpy(local_guide_property, remote_guide_property, sizeof(indigo_property) + remote_guide_property->count * sizeof(indigo_item));
				for (int i = 0; i < local_guide_property->count; i++) {
					indigo_item *item = local_guide_property->items + i;
					if (!strcmp(item->name, GUIDER_GUIDE_NORTH_ITEM_NAME)) {
						item->number.value = dec > 0 ? dec * 1000 : 0;
					} else if (!strcmp(item->name, GUIDER_GUIDE_SOUTH_ITEM_NAME)) {
						item->number.value = dec < 0 ? -dec * 1000 : 0;
					}
				}
				indigo_change_property(FILTER_DEVICE_CONTEXT->client, local_guide_property);
				while (remote_guide_property->state == INDIGO_BUSY_STATE) {
					usleep(50000);
				}
				indigo_release_property(local_guide_property);
			}
		}
	}
	return INDIGO_OK_STATE;
}

static void preview_process(indigo_device *device) {
	AGENT_GUIDER_STATS_PHASE_ITEM->number.value = -1;
	AGENT_GUIDER_STATS_FRAME_ITEM->number.value = AGENT_GUIDER_STATS_FRAME_ITEM->number.value = AGENT_GUIDER_STATS_DRIFT_X_ITEM->number.value = AGENT_GUIDER_STATS_DRIFT_Y_ITEM->number.value = AGENT_GUIDER_STATS_DRIFT_RA_ITEM->number.value = AGENT_GUIDER_STATS_DRIFT_DEC_ITEM->number.value = AGENT_GUIDER_STATS_RMSE_RA_ITEM->number.value = AGENT_GUIDER_STATS_RMSE_DEC_ITEM->number.value = 0;
	while (AGENT_START_PROCESS_PROPERTY->state == INDIGO_BUSY_STATE) {
		if (capture_frame(device) != INDIGO_OK_STATE) {
			AGENT_START_PROCESS_PROPERTY->state = INDIGO_ALERT_STATE;
			break;
		}
		indigo_update_property(device, AGENT_GUIDER_STATS_PROPERTY, NULL);
	}
	indigo_update_property(device, AGENT_START_PROCESS_PROPERTY, NULL);
}

static void change_step(indigo_device *device, double q) {
	if (q > 1) {
		indigo_send_message(device, "%s: Drift is too slow", GUIDER_AGENT_NAME);
		if (AGENT_GUIDER_SETTINGS_STEP_ITEM->number.value < AGENT_GUIDER_SETTINGS_STEP_ITEM->number.max) {
			AGENT_GUIDER_SETTINGS_STEP_ITEM->number.target = (AGENT_GUIDER_SETTINGS_STEP_ITEM->number.value *= q);
			indigo_update_property(device, AGENT_GUIDER_SETTINGS_PROPERTY, "%s: Increasing calibration step to %.3g", GUIDER_AGENT_NAME, AGENT_GUIDER_SETTINGS_STEP_ITEM->number.target);
			DEVICE_PRIVATE_DATA->phase = INIT;
		} else {
			DEVICE_PRIVATE_DATA->phase = FAILED;
		}
	} else {
		indigo_send_message(device, "%s: Drift is too fast", GUIDER_AGENT_NAME);
		if (AGENT_GUIDER_SETTINGS_STEP_ITEM->number.value < AGENT_GUIDER_SETTINGS_STEP_ITEM->number.max) {
			AGENT_GUIDER_SETTINGS_STEP_ITEM->number.target = (AGENT_GUIDER_SETTINGS_STEP_ITEM->number.value *= q);
			indigo_update_property(device, AGENT_GUIDER_SETTINGS_PROPERTY, "%s: Decreasing calibration step to %.3g", GUIDER_AGENT_NAME, AGENT_GUIDER_SETTINGS_STEP_ITEM->number.target);
			DEVICE_PRIVATE_DATA->phase = INIT;
		} else {
			DEVICE_PRIVATE_DATA->phase = FAILED;
		}
	}
	indigo_update_property(device, AGENT_GUIDER_SETTINGS_PROPERTY, NULL);
}

static bool guide_and_capture_frame(indigo_device *device, double ra, double dec) {
	if (pulse_guide(device, ra, dec) != INDIGO_OK_STATE) {
		AGENT_START_PROCESS_PROPERTY->state = INDIGO_ALERT_STATE;
		return false;
	}
	if (capture_frame(device) != INDIGO_OK_STATE) {
		AGENT_START_PROCESS_PROPERTY->state = INDIGO_ALERT_STATE;
		return false;
	}
	return true;
}

static void calibrate_process(indigo_device *device) {
	double last_drift = 0, dec_angle = 0;
	int last_count = 0;
	AGENT_GUIDER_STATS_PHASE_ITEM->number.value = 0;
	AGENT_GUIDER_STATS_FRAME_ITEM->number.value = AGENT_GUIDER_STATS_FRAME_ITEM->number.value = AGENT_GUIDER_STATS_DRIFT_X_ITEM->number.value = AGENT_GUIDER_STATS_DRIFT_Y_ITEM->number.value = AGENT_GUIDER_STATS_DRIFT_RA_ITEM->number.value = AGENT_GUIDER_STATS_DRIFT_DEC_ITEM->number.value = AGENT_GUIDER_STATS_RMSE_RA_ITEM->number.value = AGENT_GUIDER_STATS_RMSE_DEC_ITEM->number.value = 0;
	indigo_update_property(device, AGENT_GUIDER_STATS_PROPERTY, NULL);
	DEVICE_PRIVATE_DATA->phase = INIT;
	while (AGENT_START_PROCESS_PROPERTY->state == INDIGO_BUSY_STATE) {
		AGENT_GUIDER_STATS_PHASE_ITEM->number.value = DEVICE_PRIVATE_DATA->phase;
		switch (DEVICE_PRIVATE_DATA->phase) {
			case INIT: {
				indigo_send_message(device, "%s: Calibration started", GUIDER_AGENT_NAME);
				AGENT_GUIDER_SETTINGS_ANGLE_ITEM->number.value = AGENT_GUIDER_SETTINGS_ANGLE_ITEM->number.target = 0;
				AGENT_GUIDER_SETTINGS_BACKLASH_ITEM->number.value = AGENT_GUIDER_SETTINGS_BACKLASH_ITEM->number.target = 0;
				AGENT_GUIDER_SETTINGS_SPEED_RA_ITEM->number.value = AGENT_GUIDER_SETTINGS_SPEED_RA_ITEM->number.target = 0;
				AGENT_GUIDER_SETTINGS_SPEED_DEC_ITEM->number.value = AGENT_GUIDER_SETTINGS_SPEED_DEC_ITEM->number.target = 0;
				indigo_update_property(device, AGENT_GUIDER_SETTINGS_PROPERTY, NULL);
				DEVICE_PRIVATE_DATA->phase = CLEAR_DEC;
				break;
			}
			case CLEAR_DEC: {
				if (AGENT_GUIDER_DEC_MODE_NONE_ITEM->sw.value) {
					DEVICE_PRIVATE_DATA->phase = CLEAR_RA;
					break;
				}
				if (AGENT_GUIDER_SETTINGS_CAL_STEPS_ITEM->number.value == 0) {
					DEVICE_PRIVATE_DATA->phase = MOVE_NORTH;
					break;
				}
				AGENT_GUIDER_STATS_FRAME_ITEM->number.value = AGENT_GUIDER_STATS_DRIFT_X_ITEM->number.value = AGENT_GUIDER_STATS_DRIFT_Y_ITEM->number.value = 0;
				if (capture_frame(device) != INDIGO_OK_STATE) {
					DEVICE_PRIVATE_DATA->phase = FAILED;
					break;
				}
				indigo_update_property(device, AGENT_GUIDER_STATS_PROPERTY, "%s: Clearing DEC backlash", GUIDER_AGENT_NAME);
				for (int i = 0; i < AGENT_GUIDER_SETTINGS_BL_STEPS_ITEM->number.value; i++) {
					if (!guide_and_capture_frame(device, 0, AGENT_GUIDER_SETTINGS_STEP_ITEM->number.value))
						break;
					indigo_update_property(device, AGENT_GUIDER_STATS_PROPERTY, NULL);
					if (DEVICE_PRIVATE_DATA->drift > AGENT_GUIDER_SETTINGS_BL_DRIFT_ITEM->number.value) {
						indigo_send_message(device, "%s: DEC backlash cleared", GUIDER_AGENT_NAME);
						DEVICE_PRIVATE_DATA->phase = CLEAR_RA;
						break;
					}
				}
				if (DEVICE_PRIVATE_DATA->phase == CLEAR_DEC) {
					change_step(device, 2);
				}
				break;
			}
			case CLEAR_RA: {
				if (AGENT_GUIDER_SETTINGS_CAL_STEPS_ITEM->number.value == 0) {
					DEVICE_PRIVATE_DATA->phase = MOVE_NORTH;
					break;
				}
				AGENT_GUIDER_STATS_FRAME_ITEM->number.value = AGENT_GUIDER_STATS_DRIFT_X_ITEM->number.value = AGENT_GUIDER_STATS_DRIFT_Y_ITEM->number.value = 0;
				if (capture_frame(device) != INDIGO_OK_STATE) {
					DEVICE_PRIVATE_DATA->phase = FAILED;
					break;
				}
				indigo_update_property(device, AGENT_GUIDER_STATS_PROPERTY, "%s: Clearing RA backlash", GUIDER_AGENT_NAME);
				for (int i = 0; i < AGENT_GUIDER_SETTINGS_BL_STEPS_ITEM->number.value; i++) {
					if (!guide_and_capture_frame(device, AGENT_GUIDER_SETTINGS_STEP_ITEM->number.value, 0))
						break;
					indigo_update_property(device, AGENT_GUIDER_STATS_PROPERTY, NULL);
					if (DEVICE_PRIVATE_DATA->drift > AGENT_GUIDER_SETTINGS_BL_DRIFT_ITEM->number.value) {
						indigo_send_message(device, "%s: RA backlash cleared", GUIDER_AGENT_NAME);
						DEVICE_PRIVATE_DATA->phase = MOVE_NORTH;
						break;
					}
				}
				if (DEVICE_PRIVATE_DATA->phase == CLEAR_RA) {
					change_step(device, 2);
				}
				break;
			}
			case MOVE_NORTH: {
				if (AGENT_GUIDER_DEC_MODE_NONE_ITEM->sw.value) {
					DEVICE_PRIVATE_DATA->phase = MOVE_WEST;
					break;
				}
				AGENT_GUIDER_STATS_FRAME_ITEM->number.value = AGENT_GUIDER_STATS_DRIFT_X_ITEM->number.value = AGENT_GUIDER_STATS_DRIFT_Y_ITEM->number.value = 0;
				if (capture_frame(device) != INDIGO_OK_STATE) {
					DEVICE_PRIVATE_DATA->phase = FAILED;
					break;
				}
				indigo_update_property(device, AGENT_GUIDER_STATS_PROPERTY, "%s: Moving north", GUIDER_AGENT_NAME);
				for (int i = 0; i < AGENT_GUIDER_SETTINGS_CAL_STEPS_ITEM->number.value; i++) {
					if (!guide_and_capture_frame(device, 0, AGENT_GUIDER_SETTINGS_STEP_ITEM->number.value))
						break;
					indigo_update_property(device, AGENT_GUIDER_STATS_PROPERTY, NULL);
					if (DEVICE_PRIVATE_DATA->drift > AGENT_GUIDER_SETTINGS_CAL_DRIFT_ITEM->number.value) {
						if (i < AGENT_GUIDER_SETTINGS_CAL_STEPS_ITEM->number.value / 5) {
							change_step(device, 0.5);
							break;
						} else {
							last_drift = DEVICE_PRIVATE_DATA->drift;
							dec_angle = atan2(AGENT_GUIDER_STATS_DRIFT_Y_ITEM->number.value, AGENT_GUIDER_STATS_DRIFT_X_ITEM->number.value);
							AGENT_GUIDER_SETTINGS_ANGLE_ITEM->number.value = AGENT_GUIDER_SETTINGS_ANGLE_ITEM->number.target = round(180 * dec_angle / PI);
							AGENT_GUIDER_SETTINGS_SPEED_DEC_ITEM->number.value = AGENT_GUIDER_SETTINGS_SPEED_DEC_ITEM->number.target = round(1000 * last_drift / (i * AGENT_GUIDER_SETTINGS_STEP_ITEM->number.value)) / 1000;
							indigo_update_property(device, AGENT_GUIDER_SETTINGS_PROPERTY, NULL);
							last_count = i;
							DEVICE_PRIVATE_DATA->phase = MOVE_SOUTH;
							break;
						}
					}
				}
				if (DEVICE_PRIVATE_DATA->phase == MOVE_NORTH) {
					change_step(device, 2);
				}
				break;
			}
			case MOVE_SOUTH: {
				AGENT_GUIDER_STATS_FRAME_ITEM->number.value = AGENT_GUIDER_STATS_DRIFT_X_ITEM->number.value = AGENT_GUIDER_STATS_DRIFT_Y_ITEM->number.value = 0;
				if (capture_frame(device) != INDIGO_OK_STATE) {
					DEVICE_PRIVATE_DATA->phase = FAILED;
					break;
				}
				indigo_update_property(device, AGENT_GUIDER_STATS_PROPERTY, "%s: Moving south", GUIDER_AGENT_NAME);
				for (int i = 0; i < last_count; i++) {
					if (!guide_and_capture_frame(device, 0, -AGENT_GUIDER_SETTINGS_STEP_ITEM->number.value))
						break;
					indigo_update_property(device, AGENT_GUIDER_STATS_PROPERTY, NULL);
				}
				if (DEVICE_PRIVATE_DATA->phase == MOVE_SOUTH) {
					if (DEVICE_PRIVATE_DATA->drift < last_drift) {
						AGENT_GUIDER_SETTINGS_BACKLASH_ITEM->number.value = AGENT_GUIDER_SETTINGS_BACKLASH_ITEM->number.target = round(1000 * (last_drift - DEVICE_PRIVATE_DATA->drift)) / 1000;
					} else {
						AGENT_GUIDER_SETTINGS_BACKLASH_ITEM->number.value = AGENT_GUIDER_SETTINGS_BACKLASH_ITEM->number.target = 0;
						indigo_send_message(device, "%s: Inconsitent DEC backlash", GUIDER_AGENT_NAME);
					}
					indigo_update_property(device, AGENT_GUIDER_SETTINGS_PROPERTY, NULL);
					DEVICE_PRIVATE_DATA->phase = MOVE_WEST;
				}
				break;
			}
			case MOVE_WEST: {
				AGENT_GUIDER_STATS_FRAME_ITEM->number.value = AGENT_GUIDER_STATS_DRIFT_X_ITEM->number.value = AGENT_GUIDER_STATS_DRIFT_Y_ITEM->number.value = 0;
				if (capture_frame(device) != INDIGO_OK_STATE) {
					DEVICE_PRIVATE_DATA->phase = FAILED;
					break;
				}
				indigo_update_property(device, AGENT_GUIDER_STATS_PROPERTY, "%s: Moving west", GUIDER_AGENT_NAME);
				for (int i = 0; i < AGENT_GUIDER_SETTINGS_CAL_STEPS_ITEM->number.value; i++) {
					if (!guide_and_capture_frame(device, AGENT_GUIDER_SETTINGS_STEP_ITEM->number.value, 0))
						break;
					indigo_update_property(device, AGENT_GUIDER_STATS_PROPERTY, NULL);
					if (DEVICE_PRIVATE_DATA->drift > AGENT_GUIDER_SETTINGS_CAL_DRIFT_ITEM->number.value) {
						if (i < AGENT_GUIDER_SETTINGS_CAL_STEPS_ITEM->number.value / 5) {
							change_step(device, 0.5);
							break;
						} else {
							last_drift = DEVICE_PRIVATE_DATA->drift;
							if (!AGENT_GUIDER_DEC_MODE_NONE_ITEM->sw.value) {
								double ra_angle = atan2(AGENT_GUIDER_STATS_DRIFT_Y_ITEM->number.value, AGENT_GUIDER_STATS_DRIFT_X_ITEM->number.value);
								double dif_p = PI - fabs(fabs(ra_angle - dec_angle + PI2) - PI);
								double dif_m = PI - fabs(fabs(ra_angle - dec_angle - PI2) - PI);
								if (dif_p < dif_m)
									dec_angle -= PI2;
								else
									dec_angle += PI2;
								AGENT_GUIDER_SETTINGS_ANGLE_ITEM->number.value = AGENT_GUIDER_SETTINGS_ANGLE_ITEM->number.target = round(180 * atan2((sin(dec_angle)+sin(ra_angle)) / 2, (cos(dec_angle)+cos(ra_angle)) / 2) / PI);
							} else {
								AGENT_GUIDER_SETTINGS_ANGLE_ITEM->number.value = AGENT_GUIDER_SETTINGS_ANGLE_ITEM->number.target = round(180 * atan2(AGENT_GUIDER_STATS_DRIFT_Y_ITEM->number.value, AGENT_GUIDER_STATS_DRIFT_X_ITEM->number.value) / PI);
							}
							AGENT_GUIDER_SETTINGS_SPEED_RA_ITEM->number.value = AGENT_GUIDER_SETTINGS_SPEED_RA_ITEM->number.target = round(1000 * last_drift / (i * AGENT_GUIDER_SETTINGS_STEP_ITEM->number.value)) / 1000;
							indigo_update_property(device, AGENT_GUIDER_SETTINGS_PROPERTY, NULL);
							last_count = i;
							DEVICE_PRIVATE_DATA->phase = MOVE_EAST;
							break;
						}
					}
				}
				if (DEVICE_PRIVATE_DATA->phase == MOVE_WEST) {
					change_step(device, 2);
				}
				break;
			}
			case MOVE_EAST: {
				AGENT_GUIDER_STATS_FRAME_ITEM->number.value = AGENT_GUIDER_STATS_DRIFT_X_ITEM->number.value = AGENT_GUIDER_STATS_DRIFT_Y_ITEM->number.value = 0;
				if (capture_frame(device) != INDIGO_OK_STATE) {
					DEVICE_PRIVATE_DATA->phase = FAILED;
					break;
				}
				indigo_update_property(device, AGENT_GUIDER_STATS_PROPERTY, "%s: Moving east", GUIDER_AGENT_NAME);
				for (int i = 0; i < last_count; i++) {
					if (!guide_and_capture_frame(device, -AGENT_GUIDER_SETTINGS_STEP_ITEM->number.value, 0))
						break;
					indigo_update_property(device, AGENT_GUIDER_STATS_PROPERTY, NULL);
				}
				if (DEVICE_PRIVATE_DATA->phase == MOVE_EAST) {
					DEVICE_PRIVATE_DATA->phase = DONE;
				}
				break;
			}
			case FAILED: {
				indigo_send_message(device, "%s: Calibration failed", GUIDER_AGENT_NAME);
				AGENT_START_PROCESS_PROPERTY->state = INDIGO_ALERT_STATE;
				break;
			}
			case DONE: {
				indigo_send_message(device, "%s: Calibration done", GUIDER_AGENT_NAME);
				AGENT_START_PROCESS_PROPERTY->state = INDIGO_OK_STATE;
				break;
			}
			default:
				AGENT_START_PROCESS_PROPERTY->state = INDIGO_ALERT_STATE;
				break;
		}
	}
	indigo_update_property(device, AGENT_START_PROCESS_PROPERTY, NULL);
}

static void guide_process(indigo_device *device) {
	AGENT_GUIDER_STATS_PHASE_ITEM->number.value = 0;
	AGENT_GUIDER_STATS_FRAME_ITEM->number.value = AGENT_GUIDER_STATS_FRAME_ITEM->number.value = AGENT_GUIDER_STATS_DRIFT_X_ITEM->number.value = AGENT_GUIDER_STATS_DRIFT_Y_ITEM->number.value = AGENT_GUIDER_STATS_DRIFT_RA_ITEM->number.value = AGENT_GUIDER_STATS_DRIFT_DEC_ITEM->number.value = AGENT_GUIDER_STATS_RMSE_RA_ITEM->number.value = AGENT_GUIDER_STATS_RMSE_DEC_ITEM->number.value = 0;
	DEVICE_PRIVATE_DATA->rmse_ra_sum = DEVICE_PRIVATE_DATA->rmse_dec_sum = 0;
	indigo_update_property(device, AGENT_GUIDER_STATS_PROPERTY, "%s: Guiding", GUIDER_AGENT_NAME);
	while (AGENT_START_PROCESS_PROPERTY->state == INDIGO_BUSY_STATE) {
		if (capture_frame(device) != INDIGO_OK_STATE) {
			AGENT_START_PROCESS_PROPERTY->state = INDIGO_ALERT_STATE;
			break;
		}
		double angle = -PI * AGENT_GUIDER_SETTINGS_ANGLE_ITEM->number.value / 180;
		double sin_angle = sin(angle);
		double cos_angle = cos(angle);
		double min_error = AGENT_GUIDER_SETTINGS_MIN_ERR_ITEM->number.value;
		double min_pulse = AGENT_GUIDER_SETTINGS_MIN_PULSE_ITEM->number.value;
		double max_pulse = AGENT_GUIDER_SETTINGS_MAX_PULSE_ITEM->number.value;
		double drift_ra = DEVICE_PRIVATE_DATA->drift_x * cos_angle - DEVICE_PRIVATE_DATA->drift_y * sin_angle;
		double drift_dec = DEVICE_PRIVATE_DATA->drift_x * sin_angle + DEVICE_PRIVATE_DATA->drift_y * cos_angle;
		AGENT_GUIDER_STATS_DRIFT_RA_ITEM->number.value = round(1000 * drift_ra) / 1000;
		AGENT_GUIDER_STATS_DRIFT_DEC_ITEM->number.value = round(1000 * drift_dec) / 1000;
		double correction_ra = 0, correction_dec = 0;
		if (fabs(drift_ra) > min_error) {
			correction_ra = -drift_ra * AGENT_GUIDER_SETTINGS_AGG_RA_ITEM->number.value / AGENT_GUIDER_SETTINGS_SPEED_RA_ITEM->number.value / 100;
			if (correction_ra > max_pulse)
				correction_ra = max_pulse;
			else if (correction_ra < -max_pulse)
				correction_ra = -max_pulse;
			else if (fabs(correction_ra) < min_pulse)
				correction_ra = 0;
		}
		if (fabs(drift_dec) > min_error) {
			correction_dec = drift_dec * AGENT_GUIDER_SETTINGS_AGG_DEC_ITEM->number.value / AGENT_GUIDER_SETTINGS_SPEED_DEC_ITEM->number.value / 100;
			if (correction_dec > max_pulse)
				correction_dec = max_pulse;
			else if (correction_dec < -max_pulse)
				correction_dec = -max_pulse;
			else if (fabs(correction_dec) < min_pulse)
				correction_dec = 0;
		}
		if (AGENT_GUIDER_DEC_MODE_NONE_ITEM->sw.value)
			correction_dec = 0;
		else if (AGENT_GUIDER_DEC_MODE_NORTH_ITEM->sw.value && correction_dec < 0)
			correction_dec = 0;
		else if (AGENT_GUIDER_DEC_MODE_SOUTH_ITEM->sw.value && correction_dec > 0)
			correction_dec = 0;
		AGENT_GUIDER_STATS_CORR_RA_ITEM->number.value = round(1000 * correction_ra) / 1000;
		AGENT_GUIDER_STATS_CORR_DEC_ITEM->number.value = round(1000 * correction_dec) / 1000;
		if (pulse_guide(device, correction_ra, correction_dec) != INDIGO_OK_STATE) {
			AGENT_START_PROCESS_PROPERTY->state = INDIGO_ALERT_STATE;
			break;
		}
		DEVICE_PRIVATE_DATA->rmse_ra_sum += drift_ra * drift_ra;
		DEVICE_PRIVATE_DATA->rmse_dec_sum += drift_dec * drift_dec;
		AGENT_GUIDER_STATS_RMSE_RA_ITEM->number.value = round(1000 * sqrt(DEVICE_PRIVATE_DATA->rmse_ra_sum / AGENT_GUIDER_STATS_FRAME_ITEM->number.value)) / 1000;
		AGENT_GUIDER_STATS_RMSE_DEC_ITEM->number.value = round(1000 * sqrt(DEVICE_PRIVATE_DATA->rmse_dec_sum / AGENT_GUIDER_STATS_FRAME_ITEM->number.value)) / 1000;
		indigo_update_property(device, AGENT_GUIDER_STATS_PROPERTY, NULL);
	}
	indigo_update_property(device, AGENT_START_PROCESS_PROPERTY, NULL);
}

static void abort_process(indigo_device *device) {
	if (AGENT_START_PROCESS_PROPERTY->state == INDIGO_BUSY_STATE) {
		indigo_property *abort_property = indigo_init_switch_property(NULL, FILTER_DEVICE_CONTEXT->device_name[INDIGO_FILTER_CCD_INDEX], CCD_ABORT_EXPOSURE_PROPERTY_NAME, NULL, NULL, INDIGO_OK_STATE, INDIGO_RW_PERM, INDIGO_ONE_OF_MANY_RULE, 1);
		if (abort_property) {
			indigo_init_switch_item(abort_property->items, CCD_ABORT_EXPOSURE_ITEM_NAME, "", true);
			indigo_change_property(FILTER_DEVICE_CONTEXT->client, abort_property);
			indigo_release_property(abort_property);
		}
		AGENT_START_PROCESS_PROPERTY->state = INDIGO_ALERT_STATE;
		indigo_update_property(device, AGENT_START_PROCESS_PROPERTY, NULL);
	}
}

// -------------------------------------------------------------------------------- INDIGO agent device implementation

static indigo_result agent_enumerate_properties(indigo_device *device, indigo_client *client, indigo_property *property);

static indigo_result agent_device_attach(indigo_device *device) {
	assert(device != NULL);
	assert(DEVICE_PRIVATE_DATA != NULL);
	if (indigo_filter_device_attach(device, DRIVER_VERSION, INDIGO_INTERFACE_CCD) == INDIGO_OK) {
		// -------------------------------------------------------------------------------- Device properties
		FILTER_CCD_LIST_PROPERTY->hidden = false;
		FILTER_GUIDER_LIST_PROPERTY->hidden = false;
		// -------------------------------------------------------------------------------- Process properties
		AGENT_GUIDER_DETECTION_MODE_PROPERTY = indigo_init_switch_property(NULL, device->name, AGENT_GUIDER_DETECTION_MODE_PROPERTY_NAME, "Process", "Detection mode", INDIGO_OK_STATE, INDIGO_RW_PERM, INDIGO_ONE_OF_MANY_RULE, 2);
		if (AGENT_GUIDER_DETECTION_MODE_PROPERTY == NULL)
			return INDIGO_FAILED;
		indigo_init_switch_item(AGENT_GUIDER_DETECTION_DONUTS_ITEM, AGENT_GUIDER_DETECTION_DONUTS_ITEM_NAME, "Donuts mode", true);
		indigo_init_switch_item(AGENT_GUIDER_DETECTION_CENTROID_ITEM, AGENT_GUIDER_DETECTION_CENTROID_ITEM_NAME, "Centroid mode", false);
		AGENT_GUIDER_DEC_MODE_PROPERTY = indigo_init_switch_property(NULL, device->name, AGENT_GUIDER_DEC_MODE_PROPERTY_NAME, "Process", "Dec guiding mode", INDIGO_OK_STATE, INDIGO_RW_PERM, INDIGO_ONE_OF_MANY_RULE, 4);
		if (AGENT_GUIDER_DEC_MODE_PROPERTY == NULL)
			return INDIGO_FAILED;
		indigo_init_switch_item(AGENT_GUIDER_DEC_MODE_BOTH_ITEM, AGENT_GUIDER_DEC_MODE_BOTH_ITEM_NAME, "North and south", true);
		indigo_init_switch_item(AGENT_GUIDER_DEC_MODE_NORTH_ITEM, AGENT_GUIDER_DEC_MODE_NORTH_ITEM_NAME, "North only", false);
		indigo_init_switch_item(AGENT_GUIDER_DEC_MODE_SOUTH_ITEM, AGENT_GUIDER_DEC_MODE_SOUTH_ITEM_NAME, "South only", false);
		indigo_init_switch_item(AGENT_GUIDER_DEC_MODE_NONE_ITEM, AGENT_GUIDER_DEC_MODE_NONE_ITEM_NAME, "None", false);
		AGENT_START_PROCESS_PROPERTY = indigo_init_switch_property(NULL, device->name, AGENT_START_PROCESS_PROPERTY_NAME, "Process", "Start", INDIGO_OK_STATE, INDIGO_RW_PERM, INDIGO_ANY_OF_MANY_RULE, 3);
		if (AGENT_START_PROCESS_PROPERTY == NULL)
			return INDIGO_FAILED;
		indigo_init_switch_item(AGENT_GUIDER_START_PREVIEW_ITEM, AGENT_GUIDER_START_PREVIEW_ITEM_NAME, "Start preview", false);
		indigo_init_switch_item(AGENT_GUIDER_START_CALIBRATION_ITEM, AGENT_GUIDER_START_CALIBRATION_ITEM_NAME, "Start calibration", false);
		indigo_init_switch_item(AGENT_GUIDER_START_GUIDING_ITEM, AGENT_GUIDER_START_GUIDING_ITEM_NAME, "Start guiding", false);
		AGENT_ABORT_PROCESS_PROPERTY = indigo_init_switch_property(NULL, device->name, AGENT_ABORT_PROCESS_PROPERTY_NAME, "Process", "Abort", INDIGO_OK_STATE, INDIGO_RW_PERM, INDIGO_ANY_OF_MANY_RULE, 1);
		if (AGENT_ABORT_PROCESS_PROPERTY == NULL)
			return INDIGO_FAILED;
		indigo_init_switch_item(AGENT_ABORT_PROCESS_ITEM, AGENT_ABORT_PROCESS_ITEM_NAME, "Abort", false);
		// -------------------------------------------------------------------------------- Guiding properties
		AGENT_GUIDER_SETTINGS_PROPERTY = indigo_init_number_property(NULL, device->name, AGENT_GUIDER_SETTINGS_PROPERTY_NAME, "Process", "Settings", INDIGO_OK_STATE, INDIGO_RW_PERM, 15);
		if (AGENT_GUIDER_SETTINGS_PROPERTY == NULL)
			return INDIGO_FAILED;
		indigo_init_number_item(AGENT_GUIDER_SETTINGS_EXPOSURE_ITEM, AGENT_GUIDER_SETTINGS_EXPOSURE_ITEM_NAME, "Exposure time (s)", 0, 60, 0, 1);
		indigo_init_number_item(AGENT_GUIDER_SETTINGS_STEP_ITEM, AGENT_GUIDER_SETTINGS_STEP_ITEM_NAME, "Calibration step (s)", 0.05, 1, 0, 0.200);
		indigo_init_number_item(AGENT_GUIDER_SETTINGS_BL_STEPS_ITEM, AGENT_GUIDER_SETTINGS_BL_STEPS_ITEM_NAME, "Max clear backlash steps", 0, 50, 0, 10);
		indigo_init_number_item(AGENT_GUIDER_SETTINGS_BL_DRIFT_ITEM, AGENT_GUIDER_SETTINGS_BL_DRIFT_ITEM_NAME, "Min clear backlash drift (px)", 0, 15, 0, 3);
		indigo_init_number_item(AGENT_GUIDER_SETTINGS_CAL_STEPS_ITEM, AGENT_GUIDER_SETTINGS_CAL_STEPS_ITEM_NAME, "Max calibration steps", 0, 50, 0, 20);
		indigo_init_number_item(AGENT_GUIDER_SETTINGS_CAL_DRIFT_ITEM, AGENT_GUIDER_SETTINGS_CAL_DRIFT_ITEM_NAME, "Min calibration drift (px)", 0, 15, 0, 5);
		indigo_init_number_item(AGENT_GUIDER_SETTINGS_AGG_RA_ITEM, AGENT_GUIDER_SETTINGS_AGG_RA_ITEM_NAME, "RA aggressivity (%)", 0, 200, 5, 90);
		indigo_init_number_item(AGENT_GUIDER_SETTINGS_AGG_DEC_ITEM, AGENT_GUIDER_SETTINGS_AGG_DEC_ITEM_NAME, "DEC aggressivity (%)", 0, 200, 5, 90);
		indigo_init_number_item(AGENT_GUIDER_SETTINGS_MIN_ERR_ITEM, AGENT_GUIDER_SETTINGS_MIN_ERR_ITEM_NAME, "Min error (px)", 0, 5, 0, 0);
		indigo_init_number_item(AGENT_GUIDER_SETTINGS_MIN_PULSE_ITEM, AGENT_GUIDER_SETTINGS_MIN_PULSE_ITEM_NAME, "Min pulse (s)", 0, 1, 0, 0);
		indigo_init_number_item(AGENT_GUIDER_SETTINGS_MAX_PULSE_ITEM, AGENT_GUIDER_SETTINGS_MAX_PULSE_ITEM_NAME, "Max pulse (s)", 0, 5, 0, 1);
		indigo_init_number_item(AGENT_GUIDER_SETTINGS_ANGLE_ITEM, AGENT_GUIDER_SETTINGS_ANGLE_ITEM_NAME, "Angle (deg)", -360, 360, 0, 0);
		indigo_init_number_item(AGENT_GUIDER_SETTINGS_BACKLASH_ITEM, AGENT_GUIDER_SETTINGS_BACKLASH_ITEM_NAME, "DEC backlash (px)", 0, 100, 0, 0);
		indigo_init_number_item(AGENT_GUIDER_SETTINGS_SPEED_RA_ITEM, AGENT_GUIDER_SETTINGS_SPEED_RA_ITEM_NAME, "RA speed (px/s)", 0, 100, 0, 0);
		indigo_init_number_item(AGENT_GUIDER_SETTINGS_SPEED_DEC_ITEM, AGENT_GUIDER_SETTINGS_SPEED_DEC_ITEM_NAME, "DEC speed (px/s)", 0, 100, 0, 0);

		AGENT_GUIDER_STATS_PROPERTY = indigo_init_number_property(NULL, device->name, AGENT_GUIDER_STATS_PROPERTY_NAME, "Process", "Stats", INDIGO_OK_STATE, INDIGO_RO_PERM, 10);
		if (AGENT_GUIDER_STATS_PROPERTY == NULL)
			return INDIGO_FAILED;
		indigo_init_number_item(AGENT_GUIDER_STATS_PHASE_ITEM, AGENT_GUIDER_STATS_PHASE_ITEM_NAME, "Phase #", -1, 100, 0, 0);
		indigo_init_number_item(AGENT_GUIDER_STATS_FRAME_ITEM, AGENT_GUIDER_STATS_FRAME_ITEM_NAME, "Frame #", 0, 0xFFFFFFFF, 0, 0);
		indigo_init_number_item(AGENT_GUIDER_STATS_DRIFT_X_ITEM, AGENT_GUIDER_STATS_DRIFT_X_ITEM_NAME, "Drift X", -1000, 1000, 0, 0);
		indigo_init_number_item(AGENT_GUIDER_STATS_DRIFT_Y_ITEM, AGENT_GUIDER_STATS_DRIFT_Y_ITEM_NAME, "Drift Y", -1000, 1000, 0, 0);
		indigo_init_number_item(AGENT_GUIDER_STATS_DRIFT_RA_ITEM, AGENT_GUIDER_STATS_DRIFT_RA_ITEM_NAME, "Drift RA", -1000, 1000, 0, 0);
		indigo_init_number_item(AGENT_GUIDER_STATS_DRIFT_DEC_ITEM, AGENT_GUIDER_STATS_DRIFT_DEC_ITEM_NAME, "Drift Dec", -1000, 1000, 0, 0);
		indigo_init_number_item(AGENT_GUIDER_STATS_CORR_RA_ITEM, AGENT_GUIDER_STATS_CORR_RA_ITEM_NAME, "Correction RA", -1000, 1000, 0, 0);
		indigo_init_number_item(AGENT_GUIDER_STATS_CORR_DEC_ITEM, AGENT_GUIDER_STATS_CORR_DEC_ITEM_NAME, "Correction Dec", -1000, 1000, 0, 0);
		indigo_init_number_item(AGENT_GUIDER_STATS_RMSE_RA_ITEM, AGENT_GUIDER_STATS_RMSE_RA_ITEM_NAME, "RMSE RA", -1000, 1000, 0, 0);
		indigo_init_number_item(AGENT_GUIDER_STATS_RMSE_DEC_ITEM, AGENT_GUIDER_STATS_RMSE_DEC_ITEM_NAME, "RMSE Dec", -1000, 1000, 0, 0);
		// --------------------------------------------------------------------------------
		CONNECTION_PROPERTY->hidden = true;
		PROFILE_PROPERTY->hidden = true;
		INDIGO_DEVICE_ATTACH_LOG(DRIVER_NAME, device->name);
		return agent_enumerate_properties(device, NULL, NULL);
	}
	return INDIGO_FAILED;
}

static indigo_result agent_enumerate_properties(indigo_device *device, indigo_client *client, indigo_property *property) {
	if (client != NULL && client == FILTER_DEVICE_CONTEXT->client)
		return INDIGO_OK;
	if (!FILTER_CCD_LIST_PROPERTY->items->sw.value && !FILTER_GUIDER_LIST_PROPERTY->items->sw.value) {
		if (indigo_property_match(AGENT_GUIDER_DETECTION_MODE_PROPERTY, property))
			indigo_define_property(device, AGENT_GUIDER_DETECTION_MODE_PROPERTY, NULL);
		if (indigo_property_match(AGENT_START_PROCESS_PROPERTY, property))
			indigo_define_property(device, AGENT_START_PROCESS_PROPERTY, NULL);
		if (indigo_property_match(AGENT_ABORT_PROCESS_PROPERTY, property))
			indigo_define_property(device, AGENT_ABORT_PROCESS_PROPERTY, NULL);
		if (indigo_property_match(AGENT_GUIDER_SETTINGS_PROPERTY, property))
			indigo_define_property(device, AGENT_GUIDER_SETTINGS_PROPERTY, NULL);
		if (indigo_property_match(AGENT_GUIDER_STATS_PROPERTY, property))
			indigo_define_property(device, AGENT_GUIDER_STATS_PROPERTY, NULL);
		if (indigo_property_match(AGENT_GUIDER_DEC_MODE_PROPERTY, property))
			indigo_define_property(device, AGENT_GUIDER_DEC_MODE_PROPERTY, NULL);
	}
	return indigo_filter_enumerate_properties(device, client, property);
}

static indigo_result agent_change_property(indigo_device *device, indigo_client *client, indigo_property *property) {
	assert(device != NULL);
	assert(DEVICE_CONTEXT != NULL);
	assert(property != NULL);
	if (client == FILTER_DEVICE_CONTEXT->client)
		return INDIGO_OK;
	if (indigo_property_match(AGENT_GUIDER_DETECTION_MODE_PROPERTY, property)) {
		indigo_property_copy_values(AGENT_GUIDER_DETECTION_MODE_PROPERTY, property, false);
		AGENT_GUIDER_DETECTION_MODE_PROPERTY->state = INDIGO_OK_STATE;
		indigo_update_property(device, AGENT_GUIDER_DETECTION_MODE_PROPERTY, NULL);
	} else if (indigo_property_match(AGENT_GUIDER_DEC_MODE_PROPERTY, property)) {
		indigo_property_copy_values(AGENT_GUIDER_DEC_MODE_PROPERTY, property, false);
		AGENT_GUIDER_DEC_MODE_PROPERTY->state = INDIGO_OK_STATE;
		indigo_update_property(device, AGENT_GUIDER_DEC_MODE_PROPERTY, NULL);
	} else if (indigo_property_match(AGENT_GUIDER_SETTINGS_PROPERTY, property)) {
		indigo_property_copy_values(AGENT_GUIDER_SETTINGS_PROPERTY, property, false);
		AGENT_GUIDER_SETTINGS_PROPERTY->state = INDIGO_OK_STATE;
		indigo_update_property(device, AGENT_GUIDER_SETTINGS_PROPERTY, NULL);
	} else if (indigo_property_match(AGENT_START_PROCESS_PROPERTY, property)) {
		if (*FILTER_DEVICE_CONTEXT->device_name[INDIGO_FILTER_CCD_INDEX]) {
			indigo_property_copy_values(AGENT_START_PROCESS_PROPERTY, property, false);
			if (AGENT_START_PROCESS_PROPERTY->state != INDIGO_BUSY_STATE) {
				if (AGENT_GUIDER_START_PREVIEW_ITEM->sw.value) {
					AGENT_START_PROCESS_PROPERTY->state = INDIGO_BUSY_STATE;
					indigo_set_timer(device, 0, preview_process);
				} else if (AGENT_GUIDER_START_CALIBRATION_ITEM->sw.value) {
					AGENT_START_PROCESS_PROPERTY->state = INDIGO_BUSY_STATE;
					indigo_set_timer(device, 0, calibrate_process);
				} else if (AGENT_GUIDER_START_GUIDING_ITEM->sw.value) {
					AGENT_START_PROCESS_PROPERTY->state = INDIGO_BUSY_STATE;
					indigo_set_timer(device, 0, guide_process);
				} else {
					AGENT_START_PROCESS_PROPERTY->state = INDIGO_ALERT_STATE;
				}
			}
			AGENT_GUIDER_START_PREVIEW_ITEM->sw.value = AGENT_GUIDER_START_CALIBRATION_ITEM->sw.value = AGENT_GUIDER_START_GUIDING_ITEM->sw.value = false;
			indigo_update_property(device, AGENT_START_PROCESS_PROPERTY, NULL);
		} else {
			AGENT_START_PROCESS_PROPERTY->state = INDIGO_ALERT_STATE;
			indigo_update_property(device, AGENT_START_PROCESS_PROPERTY, "%s: No CCD is selected", GUIDER_AGENT_NAME);
		}
	} else 	if (indigo_property_match(AGENT_ABORT_PROCESS_PROPERTY, property)) {
		if (*FILTER_DEVICE_CONTEXT->device_name[INDIGO_FILTER_CCD_INDEX]) {
			indigo_property_copy_values(AGENT_ABORT_PROCESS_PROPERTY, property, false);
			abort_process(device);
			AGENT_ABORT_PROCESS_ITEM->sw.value = false;
			AGENT_ABORT_PROCESS_PROPERTY->state = INDIGO_OK_STATE;
			indigo_update_property(device, AGENT_ABORT_PROCESS_PROPERTY, NULL);
		} else {
			AGENT_ABORT_PROCESS_PROPERTY->state = INDIGO_ALERT_STATE;
			indigo_update_property(device, AGENT_ABORT_PROCESS_PROPERTY, "%s: No CCD is selected", GUIDER_AGENT_NAME);
		}
	} else if (indigo_property_match(CONFIG_PROPERTY, property)) {
			// -------------------------------------------------------------------------------- CONFIG
		if (indigo_switch_match(CONFIG_SAVE_ITEM, property)) {
			indigo_save_property(device, NULL, AGENT_GUIDER_SETTINGS_PROPERTY);
			indigo_save_property(device, NULL, AGENT_GUIDER_DETECTION_MODE_PROPERTY);
			indigo_save_property(device, NULL, AGENT_GUIDER_DEC_MODE_PROPERTY);
		}
	}
	return indigo_filter_change_property(device, client, property);
}

static indigo_result agent_device_detach(indigo_device *device) {
	assert(device != NULL);
	indigo_release_property(AGENT_GUIDER_DETECTION_MODE_PROPERTY);
	indigo_release_property(AGENT_START_PROCESS_PROPERTY);
	indigo_release_property(AGENT_ABORT_PROCESS_PROPERTY);
	indigo_release_property(AGENT_GUIDER_SETTINGS_PROPERTY);
	indigo_release_property(AGENT_GUIDER_STATS_PROPERTY);
	indigo_release_property(AGENT_GUIDER_DEC_MODE_PROPERTY);
	indigo_delete_frame_digest(&DEVICE_PRIVATE_DATA->reference);
	return indigo_filter_device_detach(device);
}

// -------------------------------------------------------------------------------- INDIGO agent client implementation

static indigo_result agent_define_property(indigo_client *client, indigo_device *device, indigo_property *property, const char *message) {
	return indigo_filter_define_property(client, device, property, message);
}

static indigo_result agent_update_property(indigo_client *client, indigo_device *device, indigo_property *property, const char *message) {
	if (!strcmp(property->device, GUIDER_AGENT_NAME) && (!strcmp(property->name, FILTER_CCD_LIST_PROPERTY_NAME) || !strcmp(property->name, FILTER_GUIDER_LIST_PROPERTY_NAME))) {
		if (FILTER_CCD_LIST_PROPERTY->items->sw.value || FILTER_GUIDER_LIST_PROPERTY->items->sw.value) {
			abort_process(device);
			if (CLIENT_PRIVATE_DATA->properties_defined) {
				indigo_delete_property(device, AGENT_GUIDER_DETECTION_MODE_PROPERTY, NULL);
				indigo_delete_property(device, AGENT_START_PROCESS_PROPERTY, NULL);
				indigo_delete_property(device, AGENT_ABORT_PROCESS_PROPERTY, NULL);
				indigo_delete_property(device, AGENT_GUIDER_SETTINGS_PROPERTY, NULL);
				indigo_delete_property(device, AGENT_GUIDER_STATS_PROPERTY, NULL);
				indigo_delete_property(device, AGENT_GUIDER_DEC_MODE_PROPERTY, NULL);
				CLIENT_PRIVATE_DATA->properties_defined = false;
			}
		} else {
			if (!CLIENT_PRIVATE_DATA->properties_defined) {
				indigo_define_property(device, AGENT_GUIDER_DETECTION_MODE_PROPERTY, NULL);
				indigo_define_property(device, AGENT_START_PROCESS_PROPERTY, NULL);
				indigo_define_property(device, AGENT_ABORT_PROCESS_PROPERTY, NULL);
				indigo_define_property(device, AGENT_GUIDER_SETTINGS_PROPERTY, NULL);
				indigo_define_property(device, AGENT_GUIDER_STATS_PROPERTY, NULL);
				indigo_define_property(device, AGENT_GUIDER_DEC_MODE_PROPERTY, NULL);
				CLIENT_PRIVATE_DATA->properties_defined = true;
			}
		}
	}
	return indigo_filter_update_property(client, device, property, message);
}

static indigo_result agent_delete_property(indigo_client *client, indigo_device *device, indigo_property *property, const char *message) {
	return indigo_filter_delete_property(client, device, property, message);
}
// -------------------------------------------------------------------------------- Initialization

static agent_private_data *private_data = NULL;

static indigo_device *agent_device = NULL;
static indigo_client *agent_client = NULL;

indigo_result indigo_agent_guider(indigo_driver_action action, indigo_driver_info *info) {
	static indigo_device agent_device_template = INDIGO_DEVICE_INITIALIZER(
		GUIDER_AGENT_NAME,
		agent_device_attach,
		agent_enumerate_properties,
		agent_change_property,
		NULL,
		agent_device_detach
	);

	static indigo_client agent_client_template = {
		GUIDER_AGENT_NAME, false, NULL, INDIGO_OK, INDIGO_VERSION_CURRENT, NULL,
		indigo_filter_client_attach,
		agent_define_property,
		agent_update_property,
		agent_delete_property,
		NULL,
		indigo_filter_client_detach
	};

	static indigo_driver_action last_action = INDIGO_DRIVER_SHUTDOWN;

	SET_DRIVER_INFO(info, GUIDER_AGENT_NAME, __FUNCTION__, DRIVER_VERSION, false, last_action);

	if (action == last_action)
		return INDIGO_OK;

	switch(action) {
		case INDIGO_DRIVER_INIT:
			last_action = action;
			private_data = malloc(sizeof(agent_private_data));
			assert(private_data != NULL);
			memset(private_data, 0, sizeof(agent_private_data));
			agent_device = malloc(sizeof(indigo_device));
			assert(agent_device != NULL);
			memcpy(agent_device, &agent_device_template, sizeof(indigo_device));
			agent_device->private_data = private_data;
			indigo_attach_device(agent_device);

			agent_client = malloc(sizeof(indigo_client));
			assert(agent_client != NULL);
			memcpy(agent_client, &agent_client_template, sizeof(indigo_client));
			agent_client->client_context = agent_device->device_context;
			indigo_attach_client(agent_client);
			break;

		case INDIGO_DRIVER_SHUTDOWN:
			last_action = action;
			if (agent_client != NULL) {
				indigo_detach_client(agent_client);
				free(agent_client);
				agent_client = NULL;
			}
			if (agent_device != NULL) {
				indigo_detach_device(agent_device);
				free(agent_device);
				agent_device = NULL;
			}
			if (private_data != NULL) {
				free(private_data);
				private_data = NULL;
			}
			break;

		case INDIGO_DRIVER_INFO:
			break;
	}
	return INDIGO_OK;
}