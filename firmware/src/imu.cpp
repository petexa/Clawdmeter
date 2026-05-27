#include "imu.h"

// S3-BOX-3 has no QMI8658 IMU. Always report rotation 0 (fixed landscape).
void imu_init(void) {}
void imu_tick(void) {}
uint8_t imu_get_rotation(void) { return 0; }
