#include "mmc5603nj.h"

#include "board/board.h"
#include "console/prompt.h"
#include "drivers/exti.h"
#include "drivers/gpio.h"
#include "drivers/i2c.h"
#include "drivers/mag.h"
#include "drivers/periph_config.h"
#include "kernel/events.h"
#include "system/logging.h"
#include "os/mutex.h"
#include "system/passert.h"
#include "kernel/util/sleep.h"
#include "services/common/new_timer/new_timer.h"

#define NRF5_COMPATIBLE
#include <mcu.h>

#include <inttypes.h>
#include <stdint.h>

static PebbleMutex *s_mag_mutex;

static bool s_initialized = false;

static int s_use_refcount = 0;

//polling timer
static TimerID s_polling_timer = 0;
static bool s_polling_active = false;

// MMC5603NJ Register Definitions
#define REG_XOUT0                      0x00
#define REG_XOUT1                      0x01
#define REG_YOUT0                      0x02
#define REG_YOUT1                      0x03
#define REG_ZOUT0                      0x04
#define REG_ZOUT1                      0x05
#define REG_XOUT2                      0x06
#define REG_YOUT2                      0x07
#define REG_ZOUT2                      0x08
#define REG_XOUT                       0x00
#define REG_YOUT                       0x02
#define REG_ZOUT                       0x04
#define REG_TOUT                       0x09
#define REG_STATUS1                    0x18
#define REG_ODR                        0x1A
#define REG_INTERNAL_CONTROL_0         0x1B
#define REG_INTERNAL_CONTROL_1         0x1C
#define REG_INTERNAL_CONTROL_2         0x1D
#define REG_ST_X_TH                    0x1E
#define REG_ST_Y_TH                    0x1F
#define REG_ST_Z_TH                    0x20
#define REG_ST_X                       0x27
#define REG_ST_Y                       0x28
#define REG_ST_Z                       0x29
#define WHO_AM_I_REG                   0x39

// MMC5603NJ bit masks 
#define MASK_MEAS_DONE      (1 << 6) | (1 << 5) // Bit 6 for mag, Bit 5 for temp



static bool mmc5603nj_read(uint8_t reg_addr, uint8_t data_len, uint8_t *data) {
  i2c_use(I2C_MMC5603NJ);
  bool rv = i2c_read_register_block(I2C_MMC5603NJ, reg_addr, data_len, data);
  i2c_release(I2C_MMC5603NJ);
  return rv;
}

static bool mmc5603nj_write(uint8_t reg_addr, uint8_t data) {
  i2c_use(I2C_MMC5603NJ);
  bool rv = i2c_write_register_block(I2C_MMC5603NJ, reg_addr, 1, &data);
  i2c_release(I2C_MMC5603NJ);
  return rv;
}

static void mmc5603nj_polling_callback(void *data) {
  if (s_use_refcount == 0) {
      return;
  }
  
  // Check if data is ready by reading status register
  uint8_t status;
  if (mmc5603nj_read(REG_STATUS1, 1, &status)) {
      // Check if magnetometer data is ready (bit 1 according to datasheet)
      if (status & 0x02) { // Assuming bit 1 indicates data ready
          // Post event to trigger data processing
          PebbleEvent e = {
              .type = PEBBLE_ECOMPASS_SERVICE_EVENT,
          };
          event_put(&e);
      }
  }
}

static void mmc5603nj_start_polling(void) {
  if (!s_polling_active) {
      // Start polling timer - adjust frequency based on sample rate
      // For 5Hz sampling, poll every 200ms
      // For 20Hz sampling, poll every 50ms
      uint32_t poll_interval_ms = 200; // Default to 5Hz equivalent
      
      s_polling_timer = new_timer_create();
      new_timer_start(s_polling_timer, poll_interval_ms, 
                     mmc5603nj_polling_callback, NULL, TIMER_START_FLAG_REPEATING);
      s_polling_active = true;
  }
}

static void mmc5603nj_stop_polling(void) {
  if (s_polling_active) {
      new_timer_stop(s_polling_timer);
      new_timer_delete(s_polling_timer);
      s_polling_timer = 0;
      s_polling_active = false;
  }
}

//! Move the mag into power down mode, which is a low power mode where we're not actively sampling
//! the sensor or firing interrupts.
static bool prv_enter_standby_mode(void) {
    // Ask to enter standby mode
    //we're in continous measurement. disable it to put in power down. 0 to cmm_en in internal control 2
    if (!mmc5603nj_write(REG_INTERNAL_CONTROL_2, 0x00)) {
      return false;
    }
  
    psleep(200);
  
    return false;
  }

// Ask the compass for a 8-bit value that's programmed into the IC at the
// factory. Useful as a sanity check to make sure everything came up properly.
bool mmc5603nj_check_whoami(void) {
    static const uint8_t COMPASS_WHOAMI_BYTE = 0x10;
  
    uint8_t whoami = 0;
  
    PBL_LOG(LOG_LEVEL_INFO, "Checking WHO_AM_I register (0x39)...");
    mag_use();
    if (!mmc5603nj_read(WHO_AM_I_REG, 1, &whoami)) {
      PBL_LOG(LOG_LEVEL_ERROR, "Failed to read WHO_AM_I register");
      mag_release();
      return false;
    }
    mag_release();
  
    PBL_LOG(LOG_LEVEL_INFO, "Read compass whoami byte 0x%x, expecting 0x%x",
        whoami, COMPASS_WHOAMI_BYTE);
  
    bool success = (whoami == COMPASS_WHOAMI_BYTE);
    if (success) {
      PBL_LOG(LOG_LEVEL_INFO, "WHO_AM_I check PASSED");
    } else {
      PBL_LOG(LOG_LEVEL_ERROR, "WHO_AM_I check FAILED - wrong device or communication issue");
    }
  
    return success;
}

void mmc5603nj_init(void) {
    if (s_initialized) {
      return;
    }
    PBL_LOG(LOG_LEVEL_INFO, "Initializing MMC5603NJ magnetometer...");
    s_mag_mutex = mutex_create();
  
    s_initialized = true;

    psleep(50);

    if (!mmc5603nj_check_whoami()) {
      PBL_LOG(LOG_LEVEL_ERROR, "Failed to query Mag - WHO_AM_I check failed");
    } else {
      PBL_LOG(LOG_LEVEL_INFO, "WHO_AM_I check passed - device responding");
    }
    PBL_LOG(LOG_LEVEL_INFO, "MMC5603NJ initialization completed");

    
}
  
void mag_use(void) {
    PBL_ASSERTN(s_initialized);
  
    mutex_lock(s_mag_mutex);
  
    if (s_use_refcount == 0) {
      mmc5603nj_start_polling();
    }
    ++s_use_refcount;
  
    mutex_unlock(s_mag_mutex);
}
  
void mag_release(void) {
    PBL_ASSERTN(s_initialized && s_use_refcount != 0);
  
    mutex_lock(s_mag_mutex);
  
    --s_use_refcount;
    if (s_use_refcount == 0) {
      // We need to put the magnetometer into standby mode and read the data register to reset
      // the state so it's ready for next time.
      prv_enter_standby_mode();
  
      uint8_t raw_data[9];
      //burst read of x y z axis data
      mmc5603nj_read(REG_XOUT, sizeof(raw_data), raw_data);
  
      //stop polling
      mmc5603nj_stop_polling();
      
    }
  
    mutex_unlock(s_mag_mutex);
}

// aligns magnetometer data with the coordinate system we have adopted
// for the watch
// using only 16 bit data from register for now
static int16_t align_coord_system(int axis, uint8_t *raw_data) {
    int offset = 2 * BOARD_CONFIG_MAG.mag_config.axes_offsets[axis];
    bool do_invert = BOARD_CONFIG_MAG.mag_config.axes_inverts[axis];
    
    // Reconstruct the 16-bit unsigned value from the first two registers
    uint16_t unsigned_mag_field = ((uint16_t)raw_data[offset] << 8) | raw_data[offset + 1];

    // Convert the 16-bit unsigned value to a signed value by subtracting the
    // 16-bit zero-field offset (32768, which is 2^15)
    int16_t signed_mag_field = (int16_t)(unsigned_mag_field - 32768);
    // Apply inversion if required by the board configuration
    if (do_invert) {
        signed_mag_field = -signed_mag_field;
    }
    
    return signed_mag_field;
  }


  
// callers responsibility to know if there is valid data to be read
MagReadStatus mag_read_data(MagData *data) {
    mutex_lock(s_mag_mutex);
  
    if (s_use_refcount == 0) {
      mutex_unlock(s_mag_mutex);
      return (MagReadMagOff);
    }
  
    MagReadStatus rv = MagReadSuccess;
    uint8_t raw_data[9];
    uint8_t status1 = 0;
  
    // Reg_status1 bit 1 tells us mag data is ready to be read
    if (!mmc5603nj_read(REG_STATUS1, 1, &status1)) {
      rv = MagReadCommunicationFail;
      goto done;
    } 

    // Check if the Meas_m_done bit (bit 6) is NOT set
    if (!(status1 & 0x40)) {
      rv = MagReadNoMag;
      goto done;
    }

    // Read magnetometer data
    if (!mmc5603nj_read(REG_XOUT, sizeof(raw_data), raw_data)) {
      rv = MagReadCommunicationFail;
      goto done;
    }
  
    // TODO: shouldn't happen at low sample rate, but handle case where some data
    // is overwritten
    if ((raw_data[0] & 0xf0) != 0) {
      PBL_LOG(LOG_LEVEL_INFO, "Some Mag Sample Data was overwritten, "
              "dr_status=0x%x", raw_data[0]);
      rv = MagReadClobbered; // we still need to read the data to clear the int
    }
  
    // map raw data to watch coord system
    data->x = align_coord_system(0, &raw_data[0]);
    data->y = align_coord_system(1, &raw_data[0]);
    data->z = align_coord_system(2, &raw_data[0]);
  
  done:
    mutex_unlock(s_mag_mutex);
    return (rv);
  }
  
bool mag_change_sample_rate(MagSampleRate rate) {
    mutex_lock(s_mag_mutex);
  
    if (s_use_refcount == 0) {
      mutex_unlock(s_mag_mutex);
      return (true);
    }
  
    bool success = false;
  
    // Enter standby state since we can only change sample rate in this mode.
    if (!prv_enter_standby_mode()) {
      goto done;
    }
  
    // ODR register range (1to255hzs)
    // oversampling values at zero and just set the data rate bits.
    uint8_t new_sample_rate_value = 0;
    uint32_t poll_interval_ms = 200;
    switch(rate) {
      case MagSampleRate20Hz:
        new_sample_rate_value = 20;
        poll_interval_ms = 50; // 50ms for 20Hz
        break;
      case MagSampleRate5Hz:
        new_sample_rate_value = 5;
        poll_interval_ms = 200; // 200ms for 5Hz 
        break;
      default: //invalid sample rate
        goto done;
    }
  
    // Write the new sample rate into odr register
    // Set the Cmm_freq_en bit to 1
    // Set the Cmm_en bit to 1
    // active mode.
    if(!mmc5603nj_write(REG_ODR, new_sample_rate_value)) {
      goto done;
    }
    if(!mmc5603nj_write(REG_INTERNAL_CONTROL_0, (0x80))){ //set cmm_freq_en to 1
        goto done;
    }
    if(!mmc5603nj_write(REG_INTERNAL_CONTROL_2, (0x10))){ //set cmm_en to 1
        goto done;
    }

    mmc5603nj_stop_polling();
    s_polling_timer = new_timer_create();
    new_timer_start(s_polling_timer, poll_interval_ms, 
                   mmc5603nj_polling_callback, NULL, TIMER_START_FLAG_REPEATING);
    s_polling_active = true;
    success = true;
  done:
    mutex_unlock(s_mag_mutex);
  
    return (success);
}
  
void mag_start_sampling(void) {
    PBL_LOG(LOG_LEVEL_INFO, "Starting magnetometer sampling...");
    mag_use();
  
    //enable auto set/reset
    PBL_LOG(LOG_LEVEL_INFO, "Enabling auto set/reset (reg 0x1B = 0x20)...");
    if(!mmc5603nj_write(REG_INTERNAL_CONTROL_0, 0x20)){
      PBL_LOG(LOG_LEVEL_ERROR, "Failed to enable auto set/reset");
    } else {
      PBL_LOG(LOG_LEVEL_INFO, "Auto set/reset enabled successfully");
    }
  
    PBL_LOG(LOG_LEVEL_INFO, "Setting sample rate to 5Hz...");
    mag_change_sample_rate(MagSampleRate5Hz);
    PBL_LOG(LOG_LEVEL_INFO, "Magnetometer sampling started");
}

