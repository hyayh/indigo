// Copyright (c) 2018 Rumen G. Bogdanovski
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
// 2.0 by Rumen Bogdanovski <rumen@skyarchive.org>


/** INDIGO CCD driver for Apogee
 \file indigo_ccd_apogee.cpp
 */

#define DRIVER_VERSION 0x0001
#define DRIVER_NAME		"indigo_ccd_apogee"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <math.h>
#include <assert.h>

#include <sstream>
#include <stdexcept>

#include <pthread.h>
#include <sys/time.h>

#if defined(INDIGO_MACOS)
#include <libusb-1.0/libusb.h>
#elif defined(INDIGO_FREEBSD)
#include <libusb.h>
#else
#include <libusb-1.0/libusb.h>
#endif

#include <libapogee/ApogeeCam.h>
#include <libapogee/FindDeviceEthernet.h>
#include <libapogee/FindDeviceUsb.h>
#include <libapogee/Alta.h>
#include <libapogee/AltaF.h>
#include <libapogee/Ascent.h>
#include <libapogee/Aspen.h>
#include <libapogee/Quad.h>
#include <libapogee/ApgLogger.h>
#include <libapogee/CamHelpers.h>
#include <libapogee/versionNo.h>

#include "indigo_driver_xml.h"
#include "indigo_ccd_apogee.h"

#define MAXCAMERAS				32

#define PRIVATE_DATA              ((apogee_private_data *)device->private_data)

// gp_bits is used as boolean
#define is_connected               gp_bits

#undef INDIGO_DEBUG_DRIVER
#define INDIGO_DEBUG_DRIVER(c) c

typedef struct {
	ApogeeCam *camera;
	std::string discovery_string;
	bool available;
	int count_open;
	int exp_bin_x, exp_bin_y;
	int exp_frame_width, exp_frame_height;
	int exp_bpp;
	double target_temperature, current_temperature;
	long cooler_power;
	indigo_timer *exposure_timer, *temperature_timer;
	pthread_mutex_t usb_mutex;
	long int buffer_size;
	unsigned char *buffer;
	char serial[255];
	bool can_check_temperature;
} apogee_private_data;


//------------------- Routines to deal with discovery string, mostly taken from libapogee examples.
std::vector<std::string> GetDeviceVector( const std::string & msg ) {
	std::vector<std::string> devices;
	const std::string startDelim("<d>");
	const std::string stopDelim("</d>");

	size_t pos = 0;
	bool find = true;
	while( find ) {
		size_t posStart = msg.find( startDelim, pos );
		if( std::string::npos == posStart ) {
			break;
		}
		size_t posStop = msg.find(stopDelim, posStart + 1);
		if(std::string::npos == posStop) {
			break;
		}
		size_t strLen = (posStop - posStart) - startDelim.size();
		std::string sub = msg.substr(posStart + startDelim.size(), strLen);
		devices.push_back(sub);
		pos = 1+posStop;
	}
	return devices;
}


std::vector<std::string> MakeTokens(const std::string &str, const std::string &separator) {
	std::vector<std::string> returnVector;
	std::string::size_type start = 0;
	std::string::size_type end = 0;

	while((end = str.find(separator, start)) != std::string::npos) {
		returnVector.push_back (str.substr (start, end - start));
		start = end + separator.size();
	}

	returnVector.push_back(str.substr(start));

	return returnVector;
}


std::string GetItemFromFindStr(const std::string & msg, const std::string & item) {
	//search the single device input string for the requested item
    std::vector<std::string> params = MakeTokens( msg, "," );
	std::vector<std::string>::iterator iter;

	for(iter = params.begin(); iter != params.end(); ++iter) {
		if( std::string::npos != (*iter).find(item)) {
			std::string result = MakeTokens((*iter), "=").at(1);
			return result;
		}
	} //for

	std::string noOp;
	return noOp;
}


std::string GetInterface(const std::string & msg) {
	return GetItemFromFindStr(msg, "interface=");
}


std::string GetUsbAddress(const std::string & msg) {
	return GetItemFromFindStr(msg, "address=");
}


std::string GetEthernetAddress(const std::string & msg) {
	std::string addr = GetItemFromFindStr(msg, "address=");
	addr.append(":");
	addr.append(GetItemFromFindStr(msg, "port="));
	return addr;
}


uint16_t GetID(const std::string & msg) {
	std::string str = GetItemFromFindStr(msg, "id=");
	uint16_t id = 0;
	std::stringstream ss;
	ss << std::hex << std::showbase << str.c_str();
	ss >> id;

	return id;
}


uint16_t GetFrmwrRev(const std::string & msg) {
	std::string str = GetItemFromFindStr(msg, "firmwareRev=");

	uint16_t rev = 0;
	std::stringstream ss;
	ss << std::hex << std::showbase << str.c_str();
	ss >> rev;

	return rev;
}


CamModel::PlatformType GetModel(const std::string &msg) {
	return CamModel::GetPlatformType(GetItemFromFindStr(msg, "model="));
}


std::string GetModelName(const std::string &msg) {
	return GetItemFromFindStr(msg, "model=");
}


bool IsDeviceFilterWheel(const std::string & msg) {
	std::string str = GetItemFromFindStr(msg, "deviceType=");
	return ( 0 == str.compare("filterWheel") ? true : false );
}


void checkStatus(const Apg::Status status) {
	switch(status) {
		case Apg::Status_ConnectionError:
		{
			std::string errMsg("Status_ConnectionError");
			std::runtime_error except( errMsg );
			throw except;
		}
		break;

		case Apg::Status_DataError:
		{
			std::string errMsg("Status_DataError");
			std::runtime_error except( errMsg );
			throw except;
		}
		break;

		case Apg::Status_PatternError:
		{
			std::string errMsg("Status_PatternError");
			std::runtime_error except( errMsg );
			throw except;
		}
		break;

		case Apg::Status_Idle:
		{
			std::string errMsg("Status_Idle");
			std::runtime_error except( errMsg );
			throw except;
		}
		break;

		default:
			//no op on purpose
		break;
	}
}


// -------------------------------------------------------------------------------- INDIGO device implementation
static bool apogee_open(indigo_device *device) {
	uint16_t id = GetID(PRIVATE_DATA->discovery_string);
	uint16_t frmwrRev = GetFrmwrRev(PRIVATE_DATA->discovery_string);

	char firmwareStr[16];
	snprintf(firmwareStr, 16, "0x%X", frmwrRev);
	std::string firmwareRev = std::string(firmwareStr);
	CamModel::PlatformType model = GetModel(PRIVATE_DATA->discovery_string);

	std::string ioInterface = GetItemFromFindStr(PRIVATE_DATA->discovery_string, "interface=");
	std::string addr;
	if (!ioInterface.compare("usb")) {
		addr = GetUsbAddress(PRIVATE_DATA->discovery_string);
	} else {
		addr = GetEthernetAddress(PRIVATE_DATA->discovery_string);
	}

	pthread_mutex_lock(&PRIVATE_DATA->usb_mutex);
	switch (model) {
		case CamModel::ALTAU:
		case CamModel::ALTAE:
			PRIVATE_DATA->camera = new Alta();
			break;

		case CamModel::ASPEN:
			PRIVATE_DATA->camera = new Aspen();
			break;

		case CamModel::ALTAF:
			PRIVATE_DATA->camera = new AltaF();
			break;

		case CamModel::ASCENT:
			PRIVATE_DATA->camera = new Ascent();
			break;

		case CamModel::QUAD:
			PRIVATE_DATA->camera = new Quad();
			break;

		default:
			INDIGO_DRIVER_ERROR(DRIVER_NAME, "Model %s is not supported by the INDI Apogee driver.", GetItemFromFindStr(PRIVATE_DATA->discovery_string, "model=").c_str());
			return false;
			break;
	}

	ApogeeCam *camera = PRIVATE_DATA->camera;
	uint16_t image_width = 0;
	uint16_t image_height = 0;
	try {
		camera->OpenConnection(ioInterface, addr, frmwrRev, id);
		camera->Init();
		image_width = camera->GetMaxImgCols();
		image_height = camera->GetMaxImgRows();
	} catch (std::runtime_error &err) {
		INDIGO_DRIVER_ERROR(DRIVER_NAME, "Error opening camera: %s", err.what());
		try {
			camera->CloseConnection();
		} catch (std::runtime_error &err) {
			INDIGO_DRIVER_ERROR(DRIVER_NAME, "Error closing camera: %s", err.what());
		}
		delete(camera);
		PRIVATE_DATA->camera = NULL;
		return false;
	}

	if (PRIVATE_DATA->buffer == NULL) {
		PRIVATE_DATA->buffer_size = image_height * image_width * 2 + FITS_HEADER_SIZE;
		PRIVATE_DATA->buffer = (unsigned char*)indigo_alloc_blob_buffer(PRIVATE_DATA->buffer_size);
	}

	pthread_mutex_unlock(&PRIVATE_DATA->usb_mutex);
	return true;
}


static bool apogee_setup_exposure(indigo_device *device, int frame_left, int frame_top, int frame_width, int frame_height, int horizontal_bin, int vertical_bin) {
	ApogeeCam *camera = PRIVATE_DATA->camera;
	pthread_mutex_lock(&PRIVATE_DATA->usb_mutex);
	try {
		//camera->SetCcdAdcSpeed(Apg::AdcSpeed_Fast);
		camera->SetRoiBinCol(horizontal_bin);
		camera->SetRoiBinRow(vertical_bin);
		camera->SetRoiStartCol((uint16_t)(frame_left / horizontal_bin));
		camera->SetRoiStartRow((uint16_t)(frame_top / vertical_bin));
		camera->SetRoiNumCols((uint16_t)(frame_width / horizontal_bin));
		camera->SetRoiNumRows((uint16_t)(frame_height / vertical_bin));
		camera->SetImageCount(1);
	} catch (std::runtime_error err) {
		std::string text = err.what();
		INDIGO_DRIVER_ERROR(DRIVER_NAME, "Exposure setup: %s", text.c_str());
		pthread_mutex_unlock(&PRIVATE_DATA->usb_mutex);
		return false;
	}
	pthread_mutex_unlock(&PRIVATE_DATA->usb_mutex);

	PRIVATE_DATA->exp_bin_x = horizontal_bin;
	PRIVATE_DATA->exp_bin_y = vertical_bin;
	PRIVATE_DATA->exp_frame_width = frame_width;
	PRIVATE_DATA->exp_frame_height = frame_height;
	PRIVATE_DATA->exp_bpp = 16;

	return true;
}


static bool apogee_start_exposure(indigo_device *device, double exposure, bool dark, int frame_left, int frame_top, int frame_width, int frame_height, int horizontal_bin, int vertical_bin) {
	ApogeeCam *camera = PRIVATE_DATA->camera;
	if (!apogee_setup_exposure(device, frame_left, frame_top, frame_width, frame_height, horizontal_bin, vertical_bin)) {
		return false;
	}
	pthread_mutex_lock(&PRIVATE_DATA->usb_mutex);
	try {
		camera->StartExposure(exposure, !dark);
	} catch (std::runtime_error err) {
		std::string text = err.what();
		INDIGO_DRIVER_ERROR(DRIVER_NAME, "Start Exposure: %s", text.c_str());
		pthread_mutex_unlock(&PRIVATE_DATA->usb_mutex);
		return false;
	}
	pthread_mutex_unlock(&PRIVATE_DATA->usb_mutex);
	return true;
}


static bool apogee_read_pixels(indigo_device *device) {
	ApogeeCam *camera = PRIVATE_DATA->camera;
	int wait_cycles = 9000;    /* 9000*2000us = 18s */

	Apg::Status status = camera->GetImagingStatus();
	while ((status != Apg::Status_ImageReady) && wait_cycles--) {
		try {
			pthread_mutex_lock(&PRIVATE_DATA->usb_mutex);
			status = camera->GetImagingStatus();
			pthread_mutex_unlock(&PRIVATE_DATA->usb_mutex);
		} catch (std::runtime_error err) {
			std::string text = err.what();
			INDIGO_DRIVER_ERROR(DRIVER_NAME, "Start Exposure: %s", text.c_str());
			break;
		}
		usleep(2000);
	}
	std::vector<uint16_t> image_data(PRIVATE_DATA->buffer_size);
	if (status == Apg::Status_ImageReady) {
		try {
			pthread_mutex_lock(&PRIVATE_DATA->usb_mutex);
			camera->GetImage(image_data);
			pthread_mutex_unlock(&PRIVATE_DATA->usb_mutex);
		} catch (std::runtime_error err) {
			std::string text = err.what();
			INDIGO_DRIVER_ERROR(DRIVER_NAME, "Read Exposure: %s", text.c_str());
			return false;
		}
	} else {
		INDIGO_DRIVER_ERROR(DRIVER_NAME, "Exposure failed: %s with status=%d", device->name, status);
		return false;
	}

	std::copy(image_data.begin(), image_data.end(), (uint16_t *)(PRIVATE_DATA->buffer + FITS_HEADER_SIZE));
	//memcpy(PRIVATE_DATA->buffer + FITS_HEADER_SIZE, (char*)&image_data[0], CCD_INFO_WIDTH_ITEM->number.value * CCD_INFO_HEIGHT_ITEM->number.value * 2);
	return true;
}


static bool apogee_abort_exposure(indigo_device *device) {
	pthread_mutex_lock(&PRIVATE_DATA->usb_mutex);
	int res;
	try {
		PRIVATE_DATA->camera->StopExposure(false);
	} catch (std::runtime_error err) {
		std::string text = err.what();
		INDIGO_DRIVER_DEBUG(DRIVER_NAME, "StopExposure(): %s", text.c_str());
	}
	pthread_mutex_unlock(&PRIVATE_DATA->usb_mutex);
	if(res) return false;
	else return true;
}


static bool apogee_set_cooler(indigo_device *device, bool on, double target, double *current, long *cooler_power) {
	bool is_on_now;
	bool cooling_supported = false;
	bool cooling_regulated = false;

	ApogeeCam *camera = PRIVATE_DATA->camera;
	pthread_mutex_lock(&PRIVATE_DATA->usb_mutex);
	try {
		*current = camera->GetTempCcd();
	} catch (std::runtime_error err) {
		INDIGO_DRIVER_ERROR(DRIVER_NAME, "GetTempCcd(): %s", err.what());
	}

	try {
		cooling_supported = camera->IsCoolingSupported();
	} catch (std::runtime_error err) {
		INDIGO_DRIVER_ERROR(DRIVER_NAME, "IsCoolingSupported(): %s", err.what());
	}

	if (!cooling_supported) {
		pthread_mutex_unlock(&PRIVATE_DATA->usb_mutex);
		return false;
	}

	try {
		is_on_now = camera->IsCoolerOn();
	} catch (std::runtime_error err) {
		pthread_mutex_unlock(&PRIVATE_DATA->usb_mutex);
		INDIGO_DRIVER_ERROR(DRIVER_NAME, "IsCoolerOn(): %s", err.what());
		return false;
	}

	if (is_on_now != on) {
		try {
			camera->SetCooler(on);
		} catch (std::runtime_error err) {
			INDIGO_DRIVER_ERROR(DRIVER_NAME, "setCooler(): %s", err.what());
		}
	} else if(on) {
		try {
			cooling_regulated = camera->IsCoolingRegulated();
		} catch (std::runtime_error err) {
			INDIGO_DRIVER_ERROR(DRIVER_NAME, "IsCoolingRegulated(): %s", err.what());
		}
		try {
			if (cooling_regulated) camera->SetCoolerSetPoint(target);
		} catch (std::runtime_error err) {
			INDIGO_DRIVER_ERROR(DRIVER_NAME, "SetCoolerSetPoint(): %s", err.what());
		}
	}

	try {
		if (cooling_regulated && on) *cooler_power = camera->GetCoolerDrive();
		else *cooler_power = 0;
	} catch (std::runtime_error err) {
		INDIGO_DRIVER_ERROR(DRIVER_NAME, "GetCoolerDrive(): %s", err.what());
	}

	INDIGO_DRIVER_DEBUG(DRIVER_NAME, "GetCoolerDrive(): %d", *cooler_power);

	pthread_mutex_unlock(&PRIVATE_DATA->usb_mutex);
	return true;
}//bool status;
					//libatik_check_cooler(PRIVATE_DATA->device_context, &status, &PRIVATE_DATA->cooler_power, &PRIVATE_DATA->current_temperature);


static void apogee_close(indigo_device *device) {
	if (!device->is_connected) return;

	pthread_mutex_lock(&PRIVATE_DATA->usb_mutex);
	if (PRIVATE_DATA->camera != NULL) {
		try {
			PRIVATE_DATA->camera->CloseConnection();
		} catch (std::runtime_error &err) {
			INDIGO_DRIVER_ERROR(DRIVER_NAME, "Error closing camera: %s", err.what());
		}
		delete(PRIVATE_DATA->camera);
		PRIVATE_DATA->camera = NULL;
		indigo_global_unlock(device);
	}
	if (PRIVATE_DATA->buffer != NULL) {
		free(PRIVATE_DATA->buffer);
		PRIVATE_DATA->buffer = NULL;
	}
	pthread_mutex_unlock(&PRIVATE_DATA->usb_mutex);
}


static void exposure_timer_callback(indigo_device *device) {
	PRIVATE_DATA->exposure_timer = NULL;
	if (!CONNECTION_CONNECTED_ITEM->sw.value) return;

	if (CCD_EXPOSURE_PROPERTY->state == INDIGO_BUSY_STATE) {
		CCD_EXPOSURE_ITEM->number.value = 0;
		indigo_update_property(device, CCD_EXPOSURE_PROPERTY, NULL);
		if (apogee_read_pixels(device)) {
			indigo_process_image(device, PRIVATE_DATA->buffer, (int)(PRIVATE_DATA->exp_frame_width / PRIVATE_DATA->exp_bin_x), (int)(PRIVATE_DATA->exp_frame_height / PRIVATE_DATA->exp_bin_y), PRIVATE_DATA->exp_bpp, true, NULL);
			CCD_EXPOSURE_PROPERTY->state = INDIGO_OK_STATE;
			indigo_update_property(device, CCD_EXPOSURE_PROPERTY, NULL);
		} else {
			CCD_EXPOSURE_PROPERTY->state = INDIGO_ALERT_STATE;
			indigo_update_property(device, CCD_EXPOSURE_PROPERTY, "Exposure failed");
		}
	}
	PRIVATE_DATA->can_check_temperature = true;
}


// callback called 4s before image download (e.g. to clear vreg or turn off temperature check)
static void clear_reg_timer_callback(indigo_device *device) {
	if (!CONNECTION_CONNECTED_ITEM->sw.value) return;
	if (CCD_EXPOSURE_PROPERTY->state == INDIGO_BUSY_STATE) {
		PRIVATE_DATA->can_check_temperature = false;
		PRIVATE_DATA->exposure_timer = indigo_set_timer(device, 4, exposure_timer_callback);
	} else {
		PRIVATE_DATA->exposure_timer = NULL;
	}
}


static void ccd_temperature_callback(indigo_device *device) {
	if (!CONNECTION_CONNECTED_ITEM->sw.value) return;
	if (PRIVATE_DATA->can_check_temperature) {
		if (apogee_set_cooler(device, CCD_COOLER_ON_ITEM->sw.value, PRIVATE_DATA->target_temperature, &PRIVATE_DATA->current_temperature, &PRIVATE_DATA->cooler_power)) {
			double diff = PRIVATE_DATA->current_temperature - PRIVATE_DATA->target_temperature;
			if (CCD_COOLER_ON_ITEM->sw.value)
				CCD_TEMPERATURE_PROPERTY->state = fabs(diff) > 0.5 ? INDIGO_BUSY_STATE : INDIGO_OK_STATE;
			else
				CCD_TEMPERATURE_PROPERTY->state = INDIGO_OK_STATE;
			CCD_TEMPERATURE_ITEM->number.value = PRIVATE_DATA->current_temperature;
			CCD_COOLER_PROPERTY->state = INDIGO_OK_STATE;
			CCD_COOLER_POWER_PROPERTY->state = INDIGO_OK_STATE;
			CCD_COOLER_POWER_ITEM->number.value = PRIVATE_DATA->cooler_power;
			CCD_COOLER_PROPERTY->state = INDIGO_OK_STATE;
		} else {
			CCD_COOLER_PROPERTY->state = INDIGO_ALERT_STATE;
			CCD_TEMPERATURE_PROPERTY->state = INDIGO_ALERT_STATE;
			CCD_COOLER_POWER_PROPERTY->state = INDIGO_ALERT_STATE;
		}
		indigo_update_property(device, CCD_COOLER_PROPERTY, NULL);
		indigo_update_property(device, CCD_TEMPERATURE_PROPERTY, NULL);
		indigo_update_property(device, CCD_COOLER_POWER_PROPERTY, NULL);
	}
	indigo_reschedule_timer(device, 5, &PRIVATE_DATA->temperature_timer);
}


static indigo_result ccd_attach(indigo_device *device) {
	assert(device != NULL);
	assert(PRIVATE_DATA != NULL);
	if (indigo_ccd_attach(device, DRIVER_VERSION) == INDIGO_OK) {
		PRIVATE_DATA->can_check_temperature = true;
		INFO_PROPERTY->count = 7;
		strncpy(INFO_DEVICE_MODEL_ITEM->text.value, GetModelName(PRIVATE_DATA->discovery_string).c_str(), INDIGO_VALUE_SIZE);
		INDIGO_DEVICE_ATTACH_LOG(DRIVER_NAME, device->name);
		return indigo_ccd_enumerate_properties(device, NULL, NULL);
	}
	return INDIGO_FAILED;
}


static indigo_result ccd_change_property(indigo_device *device, indigo_client *client, indigo_property *property) {
	assert(device != NULL);
	assert(DEVICE_CONTEXT != NULL);
	assert(property != NULL);

	// -------------------------------------------------------------------------------- CONNECTION -> CCD_INFO, CCD_COOLER, CCD_TEMPERATURE
	if (indigo_property_match(CONNECTION_PROPERTY, property)) {
		indigo_property_copy_values(CONNECTION_PROPERTY, property, false);
		if (CONNECTION_CONNECTED_ITEM->sw.value) {
			if (!device->is_connected) {
				if (apogee_open(device)) {
					PRIVATE_DATA->temperature_timer = indigo_set_timer(device, 0, ccd_temperature_callback);

					ApogeeCam *camera = PRIVATE_DATA->camera;
					uint16_t image_width = 0;
					uint16_t image_height = 0;
					double pixel_width = 0;
					double pixel_height = 0;
					uint16_t max_bin_x = 0;
					uint16_t max_bin_y = 0;
					bool cooling_regulated = false;
					bool cooling_supported = false;
					std::string serial_no;
					try {
						image_width = camera->GetMaxImgCols();
						image_height = camera->GetMaxImgRows();
						pixel_width = camera->GetPixelWidth();
						pixel_height = camera->GetPixelHeight();
						max_bin_x = camera->GetMaxBinCols();
						max_bin_y = camera->GetMaxBinRows();
						cooling_regulated = camera->IsCoolingRegulated();
						cooling_supported = camera->IsCoolingSupported();
						// serial_no = camera->GetSerialNumber();  // Breaks ASPEN!!!!
						CCD_EXPOSURE_ITEM->number.min = camera->GetMinExposureTime();
						CCD_EXPOSURE_ITEM->number.max = camera->GetMaxExposureTime();
					} catch (std::runtime_error err) {
							std::string text = err.what();
							INDIGO_DRIVER_ERROR(DRIVER_NAME, "Can not get camera info: %s", text.c_str());
					}
					CCD_INFO_WIDTH_ITEM->number.value = CCD_FRAME_WIDTH_ITEM->number.value = CCD_FRAME_WIDTH_ITEM->number.max = CCD_FRAME_LEFT_ITEM->number.max = image_width;
					CCD_INFO_HEIGHT_ITEM->number.value = CCD_FRAME_HEIGHT_ITEM->number.value = CCD_FRAME_HEIGHT_ITEM->number.max = CCD_FRAME_TOP_ITEM->number.max = image_height;
					CCD_INFO_PIXEL_SIZE_ITEM->number.value = CCD_INFO_PIXEL_WIDTH_ITEM->number.value = round(pixel_width * 100)/100;
					CCD_INFO_PIXEL_HEIGHT_ITEM->number.value = round(pixel_height * 100) / 100;
					CCD_INFO_BITS_PER_PIXEL_ITEM->number.value = 16;

					strncpy(INFO_DEVICE_SERIAL_NUM_ITEM->text.value, serial_no.c_str(), INDIGO_VALUE_SIZE);
					snprintf(INFO_DEVICE_FW_REVISION_ITEM->text.value, INDIGO_VALUE_SIZE, "0x%x", GetFrmwrRev(PRIVATE_DATA->discovery_string));
					indigo_update_property(device, INFO_PROPERTY, NULL);

					CCD_MODE_PROPERTY->perm = INDIGO_RW_PERM;
					CCD_MODE_PROPERTY->count = 3;
					char name[32];
					sprintf(name, "RAW 16 %dx%d", image_width, image_height);
					indigo_init_switch_item(CCD_MODE_ITEM, "BIN_1x1", name, true);
					sprintf(name, "RAW 16 %dx%d", image_width/2, image_height/2);
					indigo_init_switch_item(CCD_MODE_ITEM+1, "BIN_2x2", name, false);
					sprintf(name, "RAW 16 %dx%d", image_width/4, image_height/4);
					indigo_init_switch_item(CCD_MODE_ITEM+2, "BIN_4x4", name, false);

					CCD_BIN_PROPERTY->perm = INDIGO_RW_PERM;
					CCD_BIN_PROPERTY->hidden = false;
					CCD_BIN_HORIZONTAL_ITEM->number.min = CCD_BIN_HORIZONTAL_ITEM->number.value = 1;
					CCD_BIN_HORIZONTAL_ITEM->number.max = CCD_INFO_MAX_HORIZONAL_BIN_ITEM->number.value = max_bin_x;
					CCD_BIN_VERTICAL_ITEM->number.min = CCD_BIN_VERTICAL_ITEM->number.value = 1;
					CCD_BIN_VERTICAL_ITEM->number.max = CCD_INFO_MAX_VERTICAL_BIN_ITEM->number.value = max_bin_y;

					if (cooling_supported) CCD_COOLER_PROPERTY->hidden = false;
					else CCD_COOLER_PROPERTY->hidden = true;

					CCD_TEMPERATURE_PROPERTY->hidden = false;

					if (cooling_regulated && cooling_supported) {
						CCD_COOLER_POWER_PROPERTY->hidden = false;
						CCD_COOLER_POWER_PROPERTY->perm = INDIGO_RO_PERM;
						CCD_TEMPERATURE_PROPERTY->perm = INDIGO_RW_PERM;
					} else {
						CCD_COOLER_POWER_PROPERTY->hidden = true;
						CCD_TEMPERATURE_PROPERTY->perm = INDIGO_RO_PERM;
					}

					PRIVATE_DATA->target_temperature = 0;
					PRIVATE_DATA->can_check_temperature = true;
					PRIVATE_DATA->temperature_timer = indigo_set_timer(device, 0, ccd_temperature_callback);

					device->is_connected = true;
					CONNECTION_PROPERTY->state = INDIGO_OK_STATE;
				} else {
					CONNECTION_PROPERTY->state = INDIGO_ALERT_STATE;
					indigo_set_switch(CONNECTION_PROPERTY, CONNECTION_DISCONNECTED_ITEM, true);
				}
			}
		} else {
			if(device->is_connected) {
				PRIVATE_DATA->can_check_temperature = false;
				indigo_cancel_timer(device, &PRIVATE_DATA->temperature_timer);
				apogee_close(device);
				device->is_connected = false;
				CONNECTION_PROPERTY->state = INDIGO_OK_STATE;
			}
		}
		// -------------------------------------------------------------------------------- CCD_EXPOSURE
	} else if (indigo_property_match(CCD_EXPOSURE_PROPERTY, property)) {
		if (CCD_EXPOSURE_PROPERTY->state == INDIGO_BUSY_STATE || CCD_STREAMING_PROPERTY->state == INDIGO_BUSY_STATE)
			return INDIGO_OK;
		indigo_property_copy_values(CCD_EXPOSURE_PROPERTY, property, false);
		indigo_use_shortest_exposure_if_bias(device);
		apogee_start_exposure(device, CCD_EXPOSURE_ITEM->number.target, CCD_FRAME_TYPE_DARK_ITEM->sw.value, CCD_FRAME_LEFT_ITEM->number.value, CCD_FRAME_TOP_ITEM->number.value, CCD_FRAME_WIDTH_ITEM->number.value, CCD_FRAME_HEIGHT_ITEM->number.value, CCD_BIN_HORIZONTAL_ITEM->number.value, CCD_BIN_VERTICAL_ITEM->number.value);
		CCD_EXPOSURE_PROPERTY->state = INDIGO_BUSY_STATE;
		indigo_update_property(device, CCD_EXPOSURE_PROPERTY, NULL);
		if (CCD_UPLOAD_MODE_LOCAL_ITEM->sw.value) {
			CCD_IMAGE_FILE_PROPERTY->state = INDIGO_BUSY_STATE;
			indigo_update_property(device, CCD_IMAGE_FILE_PROPERTY, NULL);
		} else {
			CCD_IMAGE_PROPERTY->state = INDIGO_BUSY_STATE;
			indigo_update_property(device, CCD_IMAGE_PROPERTY, NULL);
		}
		if (CCD_EXPOSURE_ITEM->number.target > 4)
			PRIVATE_DATA->exposure_timer = indigo_set_timer(device, CCD_EXPOSURE_ITEM->number.target - 4, clear_reg_timer_callback);
		else {
			PRIVATE_DATA->can_check_temperature = false;
			PRIVATE_DATA->exposure_timer = indigo_set_timer(device, CCD_EXPOSURE_ITEM->number.target, exposure_timer_callback);
		}
		// -------------------------------------------------------------------------------- CCD_ABORT_EXPOSURE
	} else if (indigo_property_match(CCD_ABORT_EXPOSURE_PROPERTY, property)) {
		if (CCD_EXPOSURE_PROPERTY->state == INDIGO_BUSY_STATE) {
			indigo_cancel_timer(device, &PRIVATE_DATA->exposure_timer);
			apogee_abort_exposure(device);
		} else if (CCD_STREAMING_PROPERTY->state == INDIGO_BUSY_STATE && CCD_STREAMING_COUNT_ITEM->number.value != 0) {
			CCD_STREAMING_COUNT_ITEM->number.value = 0;
		}
		PRIVATE_DATA->can_check_temperature = true;
		indigo_property_copy_values(CCD_ABORT_EXPOSURE_PROPERTY, property, false);
		// -------------------------------------------------------------------------------- CCD_COOLER
	} else if (indigo_property_match(CCD_COOLER_PROPERTY, property)) {
		indigo_property_copy_values(CCD_COOLER_PROPERTY, property, false);
		if (CONNECTION_CONNECTED_ITEM->sw.value && !CCD_COOLER_PROPERTY->hidden) {
			CCD_COOLER_PROPERTY->state = INDIGO_BUSY_STATE;
			indigo_update_property(device, CCD_COOLER_PROPERTY, NULL);
		}
		return INDIGO_OK;
		// -------------------------------------------------------------------------------- CCD_TEMPERATURE
	} else if (indigo_property_match(CCD_TEMPERATURE_PROPERTY, property)) {
		indigo_property_copy_values(CCD_TEMPERATURE_PROPERTY, property, false);
		if (CONNECTION_CONNECTED_ITEM->sw.value && !CCD_COOLER_PROPERTY->hidden) {
			PRIVATE_DATA->target_temperature = CCD_TEMPERATURE_ITEM->number.value;
			CCD_TEMPERATURE_ITEM->number.value = PRIVATE_DATA->current_temperature;
			CCD_TEMPERATURE_PROPERTY->state = INDIGO_BUSY_STATE;
			indigo_update_property(device, CCD_TEMPERATURE_PROPERTY, "Target Temperature = %.2f", PRIVATE_DATA->target_temperature);
			INDIGO_DRIVER_ERROR(DRIVER_NAME, "Target Temperature = %.2f", PRIVATE_DATA->target_temperature);
		}
		return INDIGO_OK;
		// ------------------------------------------------------------------------------- GAMMA
	} else if (indigo_property_match(CCD_GAMMA_PROPERTY, property)) {
		if (!IS_CONNECTED) return INDIGO_OK;
		CCD_GAMMA_PROPERTY->state = INDIGO_IDLE_STATE;
		indigo_property_copy_values(CCD_GAMMA_PROPERTY, property, false);

		pthread_mutex_lock(&PRIVATE_DATA->usb_mutex);
		//ASI_ERROR_CODE res = ASISetControlValue(PRIVATE_DATA->dev_id, ASI_GAMMA, (long)(CCD_GAMMA_ITEM->number.value), ASI_FALSE);
		pthread_mutex_unlock(&PRIVATE_DATA->usb_mutex);
		//if (res) {
		//	INDIGO_DRIVER_ERROR(DRIVER_NAME, "ASISetControlValue(%d, ASI_GAMMA) = %d", PRIVATE_DATA->dev_id, res);
		//	CCD_GAMMA_PROPERTY->state = INDIGO_ALERT_STATE;
		//} else {
			CCD_GAMMA_PROPERTY->state = INDIGO_OK_STATE;
		//}
		indigo_update_property(device, CCD_GAMMA_PROPERTY, NULL);
		return INDIGO_OK;
		// ------------------------------------------------------------------------------- OFFSET
	} else if (indigo_property_match(CCD_OFFSET_PROPERTY, property)) {
		if (!IS_CONNECTED) return INDIGO_OK;
		CCD_OFFSET_PROPERTY->state = INDIGO_IDLE_STATE;
		indigo_property_copy_values(CCD_OFFSET_PROPERTY, property, false);

		pthread_mutex_lock(&PRIVATE_DATA->usb_mutex);
		//ASI_ERROR_CODE res = ASISetControlValue(PRIVATE_DATA->dev_id, ASI_BRIGHTNESS, (long)(CCD_OFFSET_ITEM->number.value), ASI_FALSE);
		pthread_mutex_unlock(&PRIVATE_DATA->usb_mutex);
		//if (res) {
		//	INDIGO_DRIVER_ERROR(DRIVER_NAME, "ASISetControlValue(%d, ASI_BRIGHTNESS) = %d", PRIVATE_DATA->dev_id, res);
		//	CCD_OFFSET_PROPERTY->state = INDIGO_ALERT_STATE;
		//} else {
			CCD_OFFSET_PROPERTY->state = INDIGO_OK_STATE;
		//}

		indigo_update_property(device, CCD_OFFSET_PROPERTY, NULL);
		return INDIGO_OK;
		// ------------------------------------------------------------------------------- GAIN
	} else if (indigo_property_match(CCD_GAIN_PROPERTY, property)) {
		if (!IS_CONNECTED) return INDIGO_OK;
		CCD_GAIN_PROPERTY->state = INDIGO_IDLE_STATE;
		indigo_property_copy_values(CCD_GAIN_PROPERTY, property, false);

		pthread_mutex_lock(&PRIVATE_DATA->usb_mutex);
		//ASI_ERROR_CODE res = ASISetControlValue(PRIVATE_DATA->dev_id, ASI_GAIN, (long)(CCD_GAIN_ITEM->number.value), ASI_FALSE);
		pthread_mutex_unlock(&PRIVATE_DATA->usb_mutex);
		//if (res) {
		//	INDIGO_DRIVER_ERROR(DRIVER_NAME, "ASISetControlValue(%d, ASI_GAIN) = %d", PRIVATE_DATA->dev_id, res);
		//	CCD_GAIN_PROPERTY->state = INDIGO_ALERT_STATE;
		//} else {
			CCD_GAIN_PROPERTY->state = INDIGO_OK_STATE;
		//}

		indigo_update_property(device, CCD_GAIN_PROPERTY, NULL);
		return INDIGO_OK;
		// ------------------------------------------------------------------------------- CCD_FRAME
	} else if (indigo_property_match(CCD_FRAME_PROPERTY, property)) {
		indigo_property_copy_values(CCD_FRAME_PROPERTY, property, false);
		CCD_FRAME_WIDTH_ITEM->number.value = CCD_FRAME_WIDTH_ITEM->number.target = 8 * (int)(CCD_FRAME_WIDTH_ITEM->number.value / 8);
		CCD_FRAME_HEIGHT_ITEM->number.value = CCD_FRAME_HEIGHT_ITEM->number.target = 2 * (int)(CCD_FRAME_HEIGHT_ITEM->number.value / 2);
		if (CCD_FRAME_WIDTH_ITEM->number.value / CCD_BIN_HORIZONTAL_ITEM->number.value < 64)
			CCD_FRAME_WIDTH_ITEM->number.value = 64 * CCD_BIN_HORIZONTAL_ITEM->number.value;
		if (CCD_FRAME_HEIGHT_ITEM->number.value / CCD_BIN_VERTICAL_ITEM->number.value < 64)
			CCD_FRAME_HEIGHT_ITEM->number.value = 64 * CCD_BIN_VERTICAL_ITEM->number.value;
		CCD_FRAME_PROPERTY->state = INDIGO_OK_STATE;
		/* NOTE: BPP can not be set directly because can not be linked to PIXEL_FORMAT_PROPERTY */
		CCD_FRAME_BITS_PER_PIXEL_ITEM->number.value = CCD_FRAME_BITS_PER_PIXEL_ITEM->number.min = CCD_FRAME_BITS_PER_PIXEL_ITEM->number.max = 16;
		if (IS_CONNECTED)
			indigo_update_property(device, CCD_FRAME_PROPERTY, NULL);
		return INDIGO_OK;
	} else if (indigo_property_match(CCD_BIN_PROPERTY, property)) {
		// -------------------------------------------------------------------------------- CCD_BIN
		indigo_property_copy_values(CCD_BIN_PROPERTY, property, false);
		CCD_BIN_PROPERTY->state = INDIGO_OK_STATE;
		int horizontal_bin = (int)CCD_BIN_HORIZONTAL_ITEM->number.value;
		int vertical_bin = (int)CCD_BIN_VERTICAL_ITEM->number.value;
		char name[32] = "";

		for (int i = 0; i < CCD_MODE_PROPERTY->count; i++) {
			indigo_item *item = &CCD_MODE_PROPERTY->items[i];
			item->sw.value = !strcmp(item->name, name);
		}
		CCD_MODE_PROPERTY->state = INDIGO_OK_STATE;
		if (IS_CONNECTED) {
			indigo_update_property(device, CCD_MODE_PROPERTY, NULL);
			indigo_update_property(device, CCD_BIN_PROPERTY, NULL);
		}
		return INDIGO_OK;
	} else if (indigo_property_match(CONFIG_PROPERTY, property)) {
		// -------------------------------------------------------------------------------- CONFIG
		if (indigo_switch_match(CONFIG_SAVE_ITEM, property)) {
			//indigo_save_property(device, NULL, PIXEL_FORMAT_PROPERTY);
		}
	}
	return indigo_ccd_change_property(device, client, property);
}


static indigo_result ccd_detach(indigo_device *device) {
	assert(device != NULL);
	if (CONNECTION_CONNECTED_ITEM->sw.value)
		indigo_device_disconnect(NULL, device->name);
	INDIGO_DEVICE_DETACH_LOG(DRIVER_NAME, device->name);
	return indigo_ccd_detach(device);
}


// -------------------------------------------------------------------------------- hot-plug support

static pthread_mutex_t device_mutex = PTHREAD_MUTEX_INITIALIZER;

static indigo_device *devices[MAXCAMERAS] = {NULL};


static void ethernet_discover(char *network, bool cam_found) {
	static indigo_device ccd_template = INDIGO_DEVICE_INITIALIZER(
		"",
		ccd_attach,
		indigo_ccd_enumerate_properties,
		ccd_change_property,
		NULL,
		ccd_detach
	);
	std::string msg;
	std::string discovery_string;
	std::string interface;
	std::vector<std::string> device_strings;
	std::vector<std::string>::iterator iter;
	int i;

	try {
		FindDeviceEthernet Ethernet;
		msg = Ethernet.Find(std::string(network));
		//msg  = std::string("<d>address=0,interface=usb,deviceType=camera,id=0x49,firmwareRev=0x21,model=AltaU-"
		//                   "4020ML,interfaceStatus=NA</d><d>address=1,interface=usb,model=Filter "
		//                   "Wheel,deviceType=filterWheel,id=0xFFFF,firmwareRev=0xFFEE</d>");
	} catch (std::runtime_error err) {
		std::string text = err.what();
		INDIGO_DRIVER_ERROR(DRIVER_NAME, "Ethernet hot plug failed  %s", text.c_str());
		return;
	}
	if (cam_found) msg.append("<d>address=192.168.2.22,interface=ethernet,port=80,mac=0009510000FF,deviceType=camera,id=0xfeff,firmwareRev=0x0,model=AltaU-4020ML</d>");

	device_strings = GetDeviceVector(msg);
	i = 0;
	for(iter = device_strings.begin(); iter != device_strings.end(); ++iter, ++i) {
		discovery_string = (*iter);
		if (IsDeviceFilterWheel(discovery_string)) continue;

		INDIGO_DRIVER_ERROR(DRIVER_NAME, "LIST device[%d]: string = %s", i, discovery_string.c_str());
		interface = GetInterface(discovery_string);
		if (interface.compare("ethernet") != 0) continue;
		uint16_t id = GetID(discovery_string);
		uint16_t frmwrRev = GetFrmwrRev(discovery_string);
		bool found = false;
		for (int j = 0; j < MAXCAMERAS; j++) {
			indigo_device *device = devices[j];
			if (device) {
				uint16_t c_id = GetID(PRIVATE_DATA->discovery_string);
				uint16_t c_frmwrRev = GetFrmwrRev(PRIVATE_DATA->discovery_string);
				if ((id == c_id) && (frmwrRev == c_frmwrRev)) {
					found = true;
					break;
				}
			}
		}
		if (found) continue;
		INDIGO_DRIVER_ERROR(DRIVER_NAME, "ATTACH device[%d]: string = %s", i, discovery_string.c_str());
		apogee_private_data *private_data = (apogee_private_data *)malloc(sizeof(apogee_private_data));
		assert(private_data != NULL);
		memset(private_data, 0, sizeof(apogee_private_data));
		indigo_device *device = (indigo_device *)malloc(sizeof(indigo_device));
		assert(device != NULL);
		memcpy(device, &ccd_template, sizeof(indigo_device));
		device->private_data = private_data;
		PRIVATE_DATA->discovery_string = discovery_string;
		std::string model = GetModelName(discovery_string);
		snprintf(device->name, INDIGO_NAME_SIZE, "Apogee %s #%d", model.c_str(), id);
		for (int j = 0; j < MAXCAMERAS; j++) {
			if (devices[j] == NULL) {
				indigo_async((void *(*)(void *))indigo_attach_device, devices[j] = device);
				break;
			}
		}
	}

	for (int j = 0; j < MAXCAMERAS; j++) {
		indigo_device *device = devices[j];
		if (device) {
			interface = GetInterface(PRIVATE_DATA->discovery_string);
			if (interface.compare("ethernet") != 0) continue;
			PRIVATE_DATA->available = false;
		}
	}

	i = 0;
	for(iter = device_strings.begin(); iter != device_strings.end(); ++iter, ++i) {
		discovery_string = (*iter);
		INDIGO_DRIVER_ERROR(DRIVER_NAME, "LIST camera[%d]: serial = %s", i, discovery_string.c_str());
		for (int j = 0; j < MAXCAMERAS; j++) {
			indigo_device *device = devices[j];
			if (!device || (discovery_string.compare(PRIVATE_DATA->discovery_string) != 0)) continue;
			interface = GetInterface(PRIVATE_DATA->discovery_string);
			if (interface.compare("ethernet") != 0) continue;
			PRIVATE_DATA->available = true;
		}
	}
	for (int j = 0; j < MAXCAMERAS; j++) {
		indigo_device *device = devices[j];
		if (device && !PRIVATE_DATA->available) {
			interface = GetInterface(PRIVATE_DATA->discovery_string);
			if (interface.compare("ethernet") != 0) continue;
			INDIGO_DRIVER_ERROR(DRIVER_NAME, "DETACH camera[%d]: serial = %s", i, PRIVATE_DATA->discovery_string.c_str());
			indigo_detach_device(device);
			free(device->private_data);
			free(device);
			devices[j] = NULL;
		}
	}
}


static void process_plug_event() {
	static indigo_device ccd_template = INDIGO_DEVICE_INITIALIZER(
		"",
		ccd_attach,
		indigo_ccd_enumerate_properties,
		ccd_change_property,
		NULL,
		ccd_detach
	);
	std::string msg;
	std::string discovery_string;
	std::vector<std::string> device_strings;
	pthread_mutex_lock(&device_mutex);
	try {
		FindDeviceUsb lookUsb;
		msg = lookUsb.Find();
		//msg  = std::string("<d>address=0,interface=usb,deviceType=camera,id=0x49,firmwareRev=0x21,model=AltaU-"
		//                   "4020ML,interfaceStatus=NA</d><d>address=1,interface=usb,model=Filter "
		//                   "Wheel,deviceType=filterWheel,id=0xFFFF,firmwareRev=0xFFEE</d>");
	} catch (std::runtime_error err) {
		std::string text = err.what();
		INDIGO_DRIVER_ERROR(DRIVER_NAME, "USB hot plug failed  %s", text.c_str());
		INDIGO_DRIVER_ERROR(DRIVER_NAME, "There is no way to recover. Restarting!");
		// UGLY hack, but there is no way to recover!
		exit(0);
		return;
	}
	//msg.append("<d>address=192.168.2.22,interface=ethernet,port=80,mac=0009510000FF,deviceType=camera,id=0xfeff,firmwareRev=0x0,model=AltaU-4020ML</d>");

	device_strings = GetDeviceVector(msg);
	std::vector<std::string>::iterator iter;
	int i = 0;
	for(iter = device_strings.begin(); iter != device_strings.end(); ++iter, ++i) {
		discovery_string = (*iter);
		if (IsDeviceFilterWheel(discovery_string)) continue;

		INDIGO_DRIVER_ERROR(DRIVER_NAME, "LIST device[%d]: string = %s", i, discovery_string.c_str());
		std::string interface = GetInterface(discovery_string);
		if (interface.compare("usb") != 0) continue;
		uint16_t id = GetID(discovery_string);
		uint16_t frmwrRev = GetFrmwrRev(discovery_string);
		bool found = false;
		for (int j = 0; j < MAXCAMERAS; j++) {
			indigo_device *device = devices[j];
			if (device) {
				uint16_t c_id = GetID(PRIVATE_DATA->discovery_string);
				uint16_t c_frmwrRev = GetFrmwrRev(PRIVATE_DATA->discovery_string);
				if ((id == c_id) && (frmwrRev == c_frmwrRev)) {
					found = true;
					break;
				}
			}
		}
		if (found) continue;
		INDIGO_DRIVER_ERROR(DRIVER_NAME, "ATTACH device[%d]: string = %s", i, discovery_string.c_str());
		apogee_private_data *private_data = (apogee_private_data *)malloc(sizeof(apogee_private_data));
		assert(private_data != NULL);
		memset(private_data, 0, sizeof(apogee_private_data));
		indigo_device *device = (indigo_device *)malloc(sizeof(indigo_device));
		assert(device != NULL);
		memcpy(device, &ccd_template, sizeof(indigo_device));
		device->private_data = private_data;
		PRIVATE_DATA->discovery_string = discovery_string;
		std::string model = GetItemFromFindStr(discovery_string, "model=");
		snprintf(device->name, INDIGO_NAME_SIZE, "Apogee %s #%d", model.c_str(), id);
		for (int j = 0; j < MAXCAMERAS; j++) {
			if (devices[j] == NULL) {
				indigo_async((void *(*)(void *))indigo_attach_device, devices[j] = device);
				break;
			}
		}
	}
	pthread_mutex_unlock(&device_mutex);
	//ethernet_discover("192.168.0.255",true);
}

static void process_unplug_event() {
	std::string discovery_string;
	std::string msg;
	std::string interface;
	std::vector<std::string> device_strings;

	pthread_mutex_lock(&device_mutex);
	try {
		FindDeviceUsb lookUsb;
		msg = lookUsb.Find();
	} catch (std::runtime_error err) {
		std::string text = err.what();
		INDIGO_DRIVER_ERROR(DRIVER_NAME, "USB hot unplug failed  %s", text.c_str());
		INDIGO_DRIVER_ERROR(DRIVER_NAME, "There is no way to recover. Restarting!");
		// UGLY hack, but there is no way to recover!
		exit(0);
		return;
	}
	//msg.append("<d>address=192.168.2.22,interface=ethernet,port=80,mac=0009510000FF,deviceType=camera,id=0xfeff,firmwareRev=0x0,model=AltaU-4020ML</d>");
	device_strings = GetDeviceVector( msg );
	for (int j = 0; j < MAXCAMERAS; j++) {
		indigo_device *device = devices[j];
		if (device) {
			interface = GetInterface(PRIVATE_DATA->discovery_string);
			if (interface.compare("usb") != 0) continue;
			PRIVATE_DATA->available = false;
		}
	}

	std::vector<std::string>::iterator iter;
	int i = 0;
	for(iter = device_strings.begin(); iter != device_strings.end(); ++iter, ++i) {
		discovery_string = (*iter);
		INDIGO_DRIVER_ERROR(DRIVER_NAME, "LIST camera[%d]: serial = %s", i, discovery_string.c_str());
		for (int j = 0; j < MAXCAMERAS; j++) {
			indigo_device *device = devices[j];
			if (!device || (discovery_string.compare(PRIVATE_DATA->discovery_string) != 0)) continue;
			interface = GetInterface(PRIVATE_DATA->discovery_string);
			if (interface.compare("usb") != 0) continue;
			PRIVATE_DATA->available = true;
		}
	}
	for (int j = 0; j < MAXCAMERAS; j++) {
		indigo_device *device = devices[j];
		if (device && !PRIVATE_DATA->available) {
			interface = GetInterface(PRIVATE_DATA->discovery_string);
			if (interface.compare("usb") != 0) continue;
			INDIGO_DRIVER_ERROR(DRIVER_NAME, "DETACH camera[%d]: serial = %s", i, PRIVATE_DATA->discovery_string.c_str());
			indigo_detach_device(device);
			free(device->private_data);
			free(device);
			devices[j] = NULL;
		}
	}
	pthread_mutex_unlock(&device_mutex);
	//ethernet_discover("192.168.0.255",false);
}


#ifdef ___LIBUSBFIX__
static void *plug_thread_func(void *sid) {
	process_plug_event();
	pthread_exit(NULL);
	return NULL;
}

static void *unplug_thread_func(void *sid) {
	process_unplug_event();
	pthread_exit(NULL);
	return NULL;
}
#endif /* ___LIBUSBFIX__ */


static int hotplug_callback(libusb_context *ctx, libusb_device *dev, libusb_hotplug_event event, void *user_data) {

	struct libusb_device_descriptor descriptor;

	switch (event) {
		case LIBUSB_HOTPLUG_EVENT_DEVICE_ARRIVED: {
			libusb_get_device_descriptor(dev, &descriptor);
			if (descriptor.idVendor != UsbFrmwr::APOGEE_VID)
				break;
#ifdef ___LIBUSBFIX__
			pthread_t plug_thread;
			if (pthread_create(&plug_thread, NULL, plug_thread_func, NULL)) {
				INDIGO_DRIVER_ERROR(DRIVER_NAME,"Error creating thread for hot plug");
			}
#else
			process_plug_event();
#endif /* ___LIBUSBFIX__ */

			break;
		}
		case LIBUSB_HOTPLUG_EVENT_DEVICE_LEFT: {
#ifdef ___LIBUSBFIX__
			pthread_t unplug_thread;
			if (pthread_create(&unplug_thread, NULL, unplug_thread_func, NULL)) {
				INDIGO_DRIVER_ERROR(DRIVER_NAME,"Error creating thread for hot unplug");
			}
#else
			process_unplug_event();
#endif /* ___LIBUSBFIX__ */
		}
	}
	return 0;
};


/*
static int hotplug_callback(libusb_context *ctx, libusb_device *dev, libusb_hotplug_event event, void *user_data) {
	struct libusb_device_descriptor descriptor;
	pthread_mutex_lock(&device_mutex);
	libusb_get_device_descriptor(dev, &descriptor);
	if (descriptor.idVendor == UsbFrmwr::APOGEE_VID) {
		switch (event) {
			case LIBUSB_HOTPLUG_EVENT_DEVICE_ARRIVED: {
				INDIGO_DRIVER_ERROR(DRIVER_NAME, "Hot-plug: vid=%x pid=%x", descriptor.idVendor, descriptor.idProduct);
				indigo_async((void *(*)(void *))usb_hotplug, NULL);
				break;
			}
			case LIBUSB_HOTPLUG_EVENT_DEVICE_LEFT: {
				INDIGO_DRIVER_ERROR(DRIVER_NAME, "Hot-unplug: vid=%x pid=%x", descriptor.idVendor, descriptor.idProduct);
				indigo_async((void *(*)(void *))usb_hotunplug, NULL);
				break;
			}
		}
	}
	pthread_mutex_unlock(&device_mutex);
	return 0;
};
*/

static void remove_all_devices() {
	for (int i = 0; i < MAXCAMERAS; i++) {
		indigo_device **device = &devices[i];
		if (*device == NULL)
			continue;
		indigo_detach_device(*device);
		if (((apogee_private_data *)(*device)->private_data)->buffer)
			free(((apogee_private_data *)(*device)->private_data)->buffer);
		free((*device)->private_data);
		free(*device);
		*device = NULL;
	}
}

static libusb_hotplug_callback_handle callback_handle;

extern char apogee_sysconfdir[2048];

indigo_result indigo_ccd_apogee(indigo_driver_action action, indigo_driver_info *info) {
		static indigo_driver_action last_action = INDIGO_DRIVER_SHUTDOWN;

		SET_DRIVER_INFO(info, "Apogee Camera", __FUNCTION__, DRIVER_VERSION, last_action);

		if (action == last_action)
			return INDIGO_OK;

		switch (action) {
			case INDIGO_DRIVER_INIT: {
				if (getenv("INDIGO_FIRMWARE_BASE") != NULL) {
					strncpy(apogee_sysconfdir, getenv("INDIGO_FIRMWARE_BASE"), 2048);
				}
				for (int i = 0; i < MAXCAMERAS; i++) {
					devices[i] = NULL;
				}
				INDIGO_DRIVER_DEBUG(DRIVER_NAME, "libapogee version: %d.%d.%d", APOGEE_MAJOR_VERSION, APOGEE_MINOR_VERSION, APOGEE_PATCH_VERSION);
				last_action = action;
				indigo_start_usb_event_handler();
				int rc = libusb_hotplug_register_callback(NULL, (libusb_hotplug_event)(LIBUSB_HOTPLUG_EVENT_DEVICE_ARRIVED | LIBUSB_HOTPLUG_EVENT_DEVICE_LEFT), LIBUSB_HOTPLUG_ENUMERATE, UsbFrmwr::APOGEE_VID, LIBUSB_HOTPLUG_MATCH_ANY, LIBUSB_HOTPLUG_MATCH_ANY, hotplug_callback, NULL, &callback_handle);
				INDIGO_DRIVER_DEBUG(DRIVER_NAME, "libusb_hotplug_register_callback ->  %s", rc < 0 ? libusb_error_name(rc) : "OK");
				return rc >= 0 ? INDIGO_OK : INDIGO_FAILED;
			}
			case INDIGO_DRIVER_SHUTDOWN: {
				last_action = action;
				libusb_hotplug_deregister_callback(NULL, callback_handle);
				INDIGO_DRIVER_DEBUG(DRIVER_NAME, "libusb_hotplug_deregister_callback");
				remove_all_devices();
				break;
			}
			case INDIGO_DRIVER_INFO: {
				break;
			}
		}

		return INDIGO_OK;
	}
