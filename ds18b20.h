#ifndef __DS18B20_H
#define __DS18B20_H 
#include "sys.h"   
// DS18B20命令集
#define DS18B20_CMD_CONVERT_T       0x44  // 温度转换命令
#define DS18B20_CMD_READ_SCRATCHPAD 0xBE  // 读暂存器命令
#define DS18B20_CMD_WRITE_SCRATCHPAD 0x4E // 写暂存器命令
#define DS18B20_CMD_COPY_SCRATCHPAD 0x48  // 将暂存器内容复制到EEPROM
#define DS18B20_CMD_RECALL_EEPROM   0xB8  // 从EEPROM恢复暂存器
#define DS18B20_CMD_READ_POWER_SUPPLY 0xB4 // 读电源供电状态
#define DS18B20_CMD_SKIP_ROM        0xCC  // 跳过ROM命令
#define DS18B20_CMD_SEARCH_ROM      0xF0  // 搜索ROM命令
#define DS18B20_CMD_READ_ROM        0x33  // 读ROM命令
#define DS18B20_CMD_MATCH_ROM       0x55  // 匹配ROM命令

// 1-Wire端口定义 (可根据实际接线修改)
#define OW_PORT     GPIOB
#define OW_PIN      GPIO_Pin_7
#define OW_RCC      RCC_APB2Periph_GPIOB
// button for position
#define BUTTON_GPIO GPIOB
#define BUTTON_PIN GPIO_Pin_6

// 可支持的最大传感器数量
#define MAX_DS18B20_SENSORS 5
#define DS18B20_TEMP_MIN -55.0f
#define DS18B20_TEMP_MAX 125.0f
// Flash存储相关定义
#define FLASH_PAGE_SIZE             2048    // STM32F103RC页大小
#define FLASH_CONFIG_PAGE_ADDR      0x0803F000  // 使用最后一页存储配置

// 配置模式
#define CONFIG_MODE_NORMAL          0       // 正常模式
#define CONFIG_MODE_LEARNING        1       // 学习模式
// 传感器ROM码存储结构
typedef struct {
    uint8_t present;              // 传感器是否存在
    uint8_t rom_code[8];          // 传感器64位ROM码
    float last_temperature;       // 上次读取的温度值
    uint32_t last_read_time;      // 上次读取时间戳
} ds18b20_device_t;
 // 配置数据结构
typedef struct {
    uint32_t magic;               // 魔术数字，用于验证配置有效性
    uint8_t configured;           // 是否已配置
    ds18b20_device_t devices[MAX_DS18B20_SENSORS]; // 传感器配置
} ds18b20_config_t;

extern ds18b20_device_t ds18b20_devices[MAX_DS18B20_SENSORS]; // 传感器数组
extern uint8_t ds18b20_config_mode;  // 配置模式
void DS18B20_Init(void);
uint8_t DS18B20_SearchSensors(void);
void DS18B20_SetResolution(uint8_t sensor_id, uint8_t resolution);
void DS18B20_ReadAllTemperatures(float *temperatures);
uint8_t DS18B20_CheckSensorPresent(uint8_t sensor_index);
float DS18B20_ReadTemperature(uint8_t sensor_id);
// 新增配置功能
void DS18B20_SetConfigMode(uint8_t mode);
uint8_t DS18B20_GetConfigMode(void);
void DS18B20_LearnSensor(uint8_t position);
uint8_t DS18B20_DiscoverSingleSensor(uint8_t *rom_code);
void DS18B20_SaveConfig(void);
uint8_t DS18B20_LoadConfig(void);
void DS18B20_PrintConfig(void);

#endif
