/*
 * BNO085 / GT-BN008X 台架测试（ESP32-C6，3.3V 直连，无电平转换）
 *
 * 独立于小车 esp32_project，仅用于验证九轴传感器。
 *
 * 接线（I2C 模式）：
 *   ESP32 3V3  -> VCC_3V3
 *   ESP32 GND  -> GND
 *   GPIO6      -> SDA      (按你板子丝印调整 I2C_SDA)
 *   GPIO7      -> SCL      (按你板子丝印调整 I2C_SCL)
 *   3V3        -> CS       (I2C 模式必须高)
 *   GND        -> PS0
 *   GND        -> PS1
 *   AD0        -> GND(0x4A) 或 3V3(0x4B)；本程序先扫描总线
 *   RST/INT    -> 悬空
 *
 * 库：Adafruit BNO08x
 *   arduino-cli lib install "Adafruit BNO08x"
 */

#include <Wire.h>
#include <Adafruit_BNO08x.h>

#define I2C_SDA 6
#define I2C_SCL 7
#define BNO08X_RESET -1

Adafruit_BNO08x bno08x(BNO08X_RESET);
sh2_SensorValue_t sensorValue;

static float yawDeg = 0.0f;        /* 连续角度（不回绕） */
static float yawRawDeg = 0.0f;     /* 原始 -180..180 */
static bool  yawInit = false;
static float yawRateDps = 0.0f;
static uint32_t lastPrint = 0;

void enableReports()
{
  if (!bno08x.enableReport(SH2_GAME_ROTATION_VECTOR, 10000))
  {
    Serial.println("enableReport(GAME_ROTATION_VECTOR) failed");
  }
  if (!bno08x.enableReport(SH2_GYROSCOPE_CALIBRATED, 10000))
  {
    Serial.println("enableReport(GYROSCOPE) failed");
  }
}

void setup()
{
  Serial.begin(115200);
  delay(300);

  Wire.begin(I2C_SDA, I2C_SCL);
  Wire.setClock(400000);

  Serial.println("\nI2C scan:");
  uint8_t found = 0;
  for (uint8_t addr = 0x08; addr < 0x78; addr++)
  {
    Wire.beginTransmission(addr);
    if (Wire.endTransmission() == 0)
    {
      Serial.printf("  found 0x%02X\n", addr);
      found++;
    }
  }
  if (!found)
  {
    Serial.println("  (none) -> 检查 CS=3V3 / PS0,PS1=GND / 供电");
  }

  bool ok = bno08x.begin_I2C(0x4A);
  if (!ok) ok = bno08x.begin_I2C(0x4B);   /* AD0 接 3V3 时地址是 0x4B */
  if (!ok)
  {
    Serial.println("BNO085 init failed (试过 0x4A/0x4B)");
    return;
  }
  Serial.println("BNO085 ready");
  enableReports();
}

void loop()
{
  if (bno08x.wasReset())
  {
    Serial.println("sensor reset -> re-enable reports");
    enableReports();
  }

  if (bno08x.getSensorEvent(&sensorValue))
  {
    if (sensorValue.sensorId == SH2_GAME_ROTATION_VECTOR)
    {
      const float qw = sensorValue.un.gameRotationVector.real;
      const float qx = sensorValue.un.gameRotationVector.i;
      const float qy = sensorValue.un.gameRotationVector.j;
      const float qz = sensorValue.un.gameRotationVector.k;

      const float raw = atan2f(2.0f * (qw * qz + qx * qy),
                               1.0f - 2.0f * (qy * qy + qz * qz)) * 57.29578f;
      if (!yawInit)
      {
        yawDeg = raw;
        yawInit = true;
      }
      else
      {
        float d = raw - yawRawDeg;
        if (d > 180.0f)  d -= 360.0f;
        if (d < -180.0f) d += 360.0f;
        yawDeg += d;
      }
      yawRawDeg = raw;
      lastPrint = millis();
    }
    else if (sensorValue.sensorId == SH2_GYROSCOPE_CALIBRATED)
    {
      yawRateDps = sensorValue.un.gyroscope.z * 57.29578f;
    }
  }

  /* 10 Hz 打印，便于实时观察 */
  static uint32_t lastLog = 0;
  if (millis() - lastLog >= 100 && lastPrint != 0)
  {
    lastLog = millis();
    Serial.printf("yaw=%8.2f deg (raw %7.2f)  gyroZ=%7.2f dps\n",
                  yawDeg, yawRawDeg, yawRateDps);
  }
}
