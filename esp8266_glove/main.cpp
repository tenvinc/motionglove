#define _DEFAULT_SOURCE
extern "C" {
#include <FreeRTOS.h>
#include <esp/uart.h>
#include <esp8266.h>
#include <espressif/esp_common.h>
#include <lwip/tcp.h>
#include <math.h>
#include <semphr.h>
#include <ssid_config.h>
#include <stdio.h>
#include <string.h>
#include <task.h>
}
#include "Kalman.h"
#include "MPU9250.h"
#include "websocket.h"

#define LED_PIN 2

#define SERVER_PORT 8080

#ifdef SERVER_PORT
const u16_t ser_port = SERVER_PORT;
#else
const u16_t ser_port = 80;
#endif

#define POLL_INTERVAL 5

#define MPU9250_I2C_ADDR 0x68
#define MPU9250_SCL_PIN 5
#define MPU9250_SDA_PIN 4

#define CALI_WINDOW 50

#define SYS_PRINTF(fmt, ...) printf("%s: " fmt, "MAIN", ##__VA_ARGS__)
/*********************** Function prototypes *********************************/
void httpd_task(void *pvParameters);
void sensor_task(void *pvParameters);
/*****************************************************************************/

WebSocket websocket;

MPU9250 *mpu9250_dev;
i2c_dev_t i2c1{0, MPU9250_I2C_ADDR};

// static const char WS_HEADER[] = "Upgrade: websocket\r\n";
// static const char WS_SERVER_SWITCH[] = "Switching Protocols";

typedef struct xyz_reading {
  float x;
  float y;
  float z;
} xyz_reading_t;

xyz_reading_t gyroAccul = {0, 0, 0};
xyz_reading_t accelAccul = {0, 0, 0};
xyz_reading_t gyroBias;

KalmanFilter<4, 2, 2, float> mainKF;

SemaphoreHandle_t semaphore_ws;
SemaphoreHandle_t mutex_sen_reading;
xyz_reading_t orientation;
uint32_t timenow;

void ws_task(void *pvParameters) {
  while (1) {
    if (xSemaphoreTake(semaphore_ws, portMAX_DELAY) == pdTRUE) {
      if (xSemaphoreTake(mutex_sen_reading, (TickType_t)portMAX_DELAY) ==
          pdTRUE) {
        char msg[128];
        sprintf(msg,
                "{\"roll\": %5f, \"pitch\": %5f, \"yaw\": %5f, \"time\": %u, "
                "\"containReading\": true}",
                orientation.x, orientation.y, orientation.z, timenow++);
        if (websocket.state == WebSocket::State_t::CONNECTED) {
          websocket.write(msg, strlen(msg));
        }
        // ws_write(ws_pcb, (uint8_t *)msg, strlen(msg), OPCODE_TEXT);
        xSemaphoreGive(mutex_sen_reading);
      }
      xSemaphoreGive(semaphore_ws);
      vTaskDelay(100 / portTICK_PERIOD_MS);
    }
  }
}

void httpd_task(void *pvParameters) {
  err_t err;
  while (1) {
    if (sdk_wifi_get_opmode() == STATION_MODE &&
        sdk_wifi_station_get_connect_status() == STATION_GOT_IP) {
      printf("IP obtained. Now trying to connect to a live server.\n");
      err = websocket.open(192, 168, 1, 104, ser_port);
      // IP4_ADDR(&ser_addr, 192, 168, 1, 104);
      // LOCK_TCPIP_CORE();  // Mutex to make tcp threadsafe
      // ws_pcb = tcp_new();
      // err = tcp_connect(ws_pcb, &ser_addr, ser_port, ws_tcp_connected_cb);
      // UNLOCK_TCPIP_CORE();
      if (err != ERR_OK) {
        printf("Connection could not be established. Please try again.\n");
        continue;
      }
      xSemaphoreGive(semaphore_ws);
      break;
    }
    vTaskDelay(300 / portTICK_PERIOD_MS);
  }
  vTaskDelete(NULL);
}

void getAngleFromAcc(double accelX, double accelY, double accelZ, double &roll,
                     double &pitch) {
  // Follow aviation rotation sequence roll, pitch then yaw.
  // Pitch abt x, roll abt y, yaw abt z
  roll = atan2(accelX, accelZ) * (180.0 / M_PI);
  pitch =
      atan2(-accelY, sqrt(accelX * accelX + accelZ * accelZ)) * (180.0 / M_PI);
}

void sensor_task(void *pvParameters) {
  double roll, pitch;
  xyz_reading_t accelReading, gyroReading, magReading;
  float timeDelta;
  if (i2c_init(i2c1.bus, MPU9250_SCL_PIN, MPU9250_SDA_PIN, I2C_FREQ_400K) !=
      0) {
    SYS_PRINTF("Error has occurred while initializing MPU9250 I2C.\n");
  }
  mpu9250_dev = new MPU9250(i2c1, NULL);
  if (mpu9250_dev->begin() != 0) {
    SYS_PRINTF("Initialization of IMU has failed.\n");
  }
  printf("Finished INIT of MPU9250. Calibrating now. Do not touch...\n");
  for (int n = 0; n < CALI_WINDOW; n++) {
    mpu9250_dev->readSensors();
    accelAccul.x += mpu9250_dev->getXAccelMs2();
    accelAccul.y += mpu9250_dev->getYAccelMs2();
    accelAccul.z += mpu9250_dev->getZAccelMs2();
    gyroAccul.x += mpu9250_dev->getXGyroDps();
    gyroAccul.y += mpu9250_dev->getYGyroDps();
    gyroAccul.z += mpu9250_dev->getZGyroDps();
  }
  gyroBias = {gyroAccul.x / CALI_WINDOW, gyroAccul.y / CALI_WINDOW,
              gyroAccul.z / CALI_WINDOW};
  getAngleFromAcc((double)(accelAccul.x / CALI_WINDOW),
                  (double)(accelAccul.y / CALI_WINDOW),
                  (double)(accelAccul.z / CALI_WINDOW), roll, pitch);
  mainKF.setInitial((float)roll, (float)pitch);
  printf("Calibration done.\n");

  while (1) {
    mpu9250_dev->readSensors();
    accelReading = {mpu9250_dev->getXAccelMs2(), mpu9250_dev->getYAccelMs2(),
                    mpu9250_dev->getZAccelMs2()};
    gyroReading = {mpu9250_dev->getXGyroDps() - gyroBias.x,
                   mpu9250_dev->getYGyroDps() - gyroBias.y,
                   mpu9250_dev->getZGyroDps() - gyroBias.z};
    magReading = {mpu9250_dev->getMagXuT(), mpu9250_dev->getMagYuT(),
                  mpu9250_dev->getMagZuT()};
    timeDelta = 0.02;
    getAngleFromAcc((double)accelReading.x, (double)accelReading.y,
                    (double)accelReading.z, roll, pitch);
    MatrixT<2, 1, float> measurement = {roll, pitch};
    MatrixT<2, 1, float> update = {gyroReading.y,
                                   gyroReading.x};  // flipped x and y axis
    MatrixT<4, 1, float> final = mainKF.filter(update, measurement, timeDelta);
    // SYS_PRINTF("Data: Roll: %5f, Pitch: %5f\n", final(0, 0), final(2, 0));
    if (xSemaphoreTake(mutex_sen_reading, (TickType_t)0) == pdTRUE) {
      orientation.x = final(0, 0);
      orientation.y = final(2, 0);
      orientation.z = 0;
      xSemaphoreGive(mutex_sen_reading);
    }
    vTaskDelay(20 / portTICK_PERIOD_MS);
  }
  SYS_PRINTF("Deleting the sensor_task now.\n");
  free(mpu9250_dev);
  vTaskDelete(NULL);
}

extern "C" void user_init(void) {
  uart_set_baud(0, 115200);
  printf("SDK version:%s\n", sdk_system_get_sdk_version());

  struct sdk_station_config config;
  memcpy(config.ssid, WIFI_SSID, sizeof(WIFI_SSID));
  memcpy(config.password, WIFI_PASS, sizeof(WIFI_PASS));

  /* required to call wifi_set_opmode before station_set_config */
  sdk_wifi_set_opmode(STATION_MODE);
  sdk_wifi_station_set_config(&config);
  sdk_wifi_station_connect();

  /* turn off LED */
  gpio_enable(LED_PIN, GPIO_OUTPUT);
  gpio_write(LED_PIN, true);

  /* initialize tasks */
  semaphore_ws = xSemaphoreCreateBinary();
  mutex_sen_reading = xSemaphoreCreateMutex();
  if (mutex_sen_reading == NULL) {
    SYS_PRINTF("Mutex not created successfully!\n");
  }
  xSemaphoreGive(mutex_sen_reading);

  xTaskCreate(&httpd_task, "HTTP Daemon", 1024, NULL, 2, NULL);
  xTaskCreate(&ws_task, "Websocket Daemon", 1024, NULL, 3, NULL);
  // xTaskCreate(&sensor_task, "Sensor task", 1024, NULL, 2, NULL);
}
