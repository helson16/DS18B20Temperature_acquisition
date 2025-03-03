#include "ds18b20.h"
#include "delay.h"	

/**
 * DS18B20温度传感器驱动 - 适用于STM32F103RCT6
 * 支持同时连接多个DS18B20传感器
 */

#include "ds18b20.h"
#include "delay.h"
#include "stm32f10x.h"
#include "stm32f10x_gpio.h"
#include "stm32f10x_rcc.h"
#include "stm32f10x_flash.h"
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#define DS18B20_DEBUG_FLAG 1
#define DS18B20_CONFIG_MAGIC 0xD5B20123  // 用于验证配置有效性的魔术数字

// 全局变量
ds18b20_device_t ds18b20_devices[MAX_DS18B20_SENSORS]; // 传感器数组
static uint8_t ds18b20_count = 0;         // 已发现的传感器数量
uint8_t ds18b20_config_mode = CONFIG_MODE_NORMAL;  // 默认为正常模式


// 添加预定义的ROM码数组
const uint8_t PREDEFINED_ROM_CODES[5][8] = {
    {0x28, 0xF2, 0xE5, 0xB0, 0x06, 0x00, 0x00, 0x4A}, // Sensor 1
    {0x28, 0xEA, 0x88, 0xB1, 0x06, 0x00, 0x00, 0xC2}, // Sensor 2
    {0x28, 0x96, 0xDA, 0xB0, 0x06, 0x00, 0x00, 0xBE}, // Sensor 3
    {0x28, 0x01, 0xB5, 0xB0, 0x06, 0x00, 0x00, 0xF4}, // Sensor 4
    {0x28, 0xB7, 0xAD, 0xAF, 0x06, 0x00, 0x00, 0xA1}  // Sensor 5
};

// 全局变量
 ds18b20_device_t ds18b20_devices[MAX_DS18B20_SENSORS]; // 传感器数组
//static uint8_t ds18b20_count = 0;                       // 已发现的传感器数量

// 微秒延时函数
static void Delay_us(uint32_t us)
{
    uint32_t i;
    for (i = 0; i < us * 8; i++) {
        __NOP();
    }
}

// 配置GPIO为输出模式
static void ow_output_mode(void)
{
    GPIO_InitTypeDef GPIO_InitStruct;
    
    GPIO_InitStruct.GPIO_Pin = OW_PIN;
    GPIO_InitStruct.GPIO_Mode = GPIO_Mode_Out_PP;
    GPIO_InitStruct.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(OW_PORT, &GPIO_InitStruct);
}

// 配置GPIO为输入模式
static void ow_input_mode(void)
{
    GPIO_InitTypeDef GPIO_InitStruct;
    
    GPIO_InitStruct.GPIO_Pin = OW_PIN;
    GPIO_InitStruct.GPIO_Mode = GPIO_Mode_IN_FLOATING;
    GPIO_Init(OW_PORT, &GPIO_InitStruct);
}

// 读取1-Wire总线上的一位数据
static uint8_t ow_read_bit(void)
{
    uint8_t bit = 0;
    
    ow_output_mode();
    GPIO_ResetBits(OW_PORT, OW_PIN);  // 拉低总线
    Delay_us(2);                      // 保持2us
    
    ow_input_mode();                  // 释放总线
    Delay_us(10);                     // 等待数据稳定
    
    bit = GPIO_ReadInputDataBit(OW_PORT, OW_PIN); // 读取数据位
    Delay_us(50);                     // 完成时隙
    
    return bit;
}

// 向1-Wire总线写入一位数据
static void ow_write_bit(uint8_t bit)
{
    ow_output_mode();
    
    if (bit) {
        // 写"1"
        GPIO_ResetBits(OW_PORT, OW_PIN);  // 拉低总线
        Delay_us(2);                      // 保持低电平2us
        
        GPIO_SetBits(OW_PORT, OW_PIN);    // 释放总线
        Delay_us(60);                     // 保持高电平
    } else {
        // 写"0"
        GPIO_ResetBits(OW_PORT, OW_PIN);  // 拉低总线
        Delay_us(60);                     // 保持低电平60us
        
        GPIO_SetBits(OW_PORT, OW_PIN);    // 释放总线
        Delay_us(2);                      // 恢复间隔
    }
}

// 发送复位脉冲并检测存在脉冲
static uint8_t ow_reset(void)
{
    uint8_t presence;
    
    ow_output_mode();
    GPIO_ResetBits(OW_PORT, OW_PIN);      // 拉低总线
    Delay_us(480);                        // 至少480us
    
    GPIO_SetBits(OW_PORT, OW_PIN);        // 释放总线
    ow_input_mode();
    Delay_us(70);                         // 等待器件响应
    
    presence = !GPIO_ReadInputDataBit(OW_PORT, OW_PIN); // 检查存在脉冲
    Delay_us(410);                        // 等待存在脉冲结束
    
    return presence;
}

// 发送一个字节
static void ow_write_byte(uint8_t byte)
{
    uint8_t i;
    
    for (i = 0; i < 8; i++) {
        ow_write_bit(byte & 0x01);
        byte >>= 1;
    }
}

// 读取一个字节
static uint8_t ow_read_byte(void)
{
    uint8_t i;
    uint8_t byte = 0;
    
    for (i = 0; i < 8; i++) {
        byte >>= 1;
        if (ow_read_bit()) {
            byte |= 0x80;
        }
    }
    
    return byte;
}

// 计算CRC校验
static uint8_t calculate_crc(uint8_t *data, uint8_t length)
{
    uint8_t crc = 0;
    uint8_t i, j;
    
    for (i = 0; i < length; i++) {
        crc ^= data[i];
        for (j = 0; j < 8; j++) {
            if (crc & 0x01) {
                crc = (crc >> 1) ^ 0x8C;
            } else {
                crc >>= 1;
            }
        }
    }
    
    return crc;
}

// Flash操作函数
static uint8_t Flash_ErasePage(uint32_t page_address)
{
    FLASH_Status status;
    
    FLASH_Unlock();
    status = FLASH_ErasePage(page_address);
    FLASH_Lock();
    
    return (status == FLASH_COMPLETE);
}

static uint8_t Flash_WriteData(uint32_t address, uint32_t *data, uint16_t count)
{
    FLASH_Status status;
    uint16_t i;
    
    FLASH_Unlock();
    
    for (i = 0; i < count; i++) {
        status = FLASH_ProgramWord(address + (i * 4), data[i]);
        if (status != FLASH_COMPLETE) {
            FLASH_Lock();
            return 0;
        }
    }
    
    FLASH_Lock();
    return 1;
}

// 初始化函数，改为加载保存的配置
void DS18B20_Init(void)
{
    RCC_APB2PeriphClockCmd(OW_RCC, ENABLE);
    
    ow_output_mode();
    GPIO_SetBits(OW_PORT, OW_PIN);
    
    // 清空设备数组
    memset(ds18b20_devices, 0, sizeof(ds18b20_devices));
    
    // 尝试从Flash加载配置
    if (!DS18B20_LoadConfig()) {
        printf("No valid configuration found, initializing with defaults\r\n");
        // 如果没有有效配置，则初始化为默认状态
        for (uint8_t i = 0; i < MAX_DS18B20_SENSORS; i++) {
            ds18b20_devices[i].present = 0;
            ds18b20_devices[i].last_temperature = 0.0f;
        }
    }
    
    Delay_ms(100);
    
    // 检测总线上的传感器
    ds18b20_count = 0;
    for (uint8_t i = 0; i < MAX_DS18B20_SENSORS; i++) {
        if (DS18B20_CheckSensorPresent(i)) {
            ds18b20_count++;
            ds18b20_devices[i].present = 1;
        } else {
            ds18b20_devices[i].present = 0;
        }
    }
    
    printf("DS18B20 initialization complete, %d sensors active\r\n", ds18b20_count);
}

// 搜索所有传感器
uint8_t DS18B20_SearchSensors(void)
{
    uint8_t sensor_count = 0;
    uint8_t rom_codes[MAX_DS18B20_SENSORS][8];
    uint8_t devices_found = 0;
    uint8_t last_device = 0;
    uint8_t last_discrepancy = 0;
    
    printf("Searching for DS18B20 sensors...\r\n");
    
    // 检查总线上是否有设备
    if (!ow_reset()) {
        printf("No devices present on 1-Wire bus\r\n");
        return 0;
    }
    
    // 搜索ROM算法
    do {
        uint8_t rom_code[8];
        uint8_t bit_number = 1;
        uint8_t last_zero = 0;
        uint8_t rom_byte_number = 0;
        uint8_t rom_byte_mask = 1;
        uint8_t search_result = 0;
        uint8_t id_bit, cmp_id_bit;
        
        // 发送搜索ROM命令
        ow_reset();
        ow_write_byte(DS18B20_CMD_SEARCH_ROM);
        
        // 64位ROM码搜索
        for (bit_number = 1; bit_number <= 64; bit_number++) {
            // 读取两位：ID位和补位
            id_bit = ow_read_bit();
            cmp_id_bit = ow_read_bit();
            
            if (id_bit && cmp_id_bit) {
                // 没有设备或错误
                break;
            }
            
            if (bit_number <= last_discrepancy)
                rom_byte_mask = ((last_device >> (bit_number - 1)) & 0x01);
            else
                rom_byte_mask = (id_bit != cmp_id_bit) ? id_bit : 1;
            
            // 搜索分支选择
            ow_write_bit(rom_byte_mask);
            
            // 保存ROM码位
            if (rom_byte_mask) {
                rom_code[rom_byte_number] |= rom_byte_mask;
            } else {
                rom_code[rom_byte_number] &= ~rom_byte_mask;
                last_zero = bit_number;
            }
            
            // 准备下一位
            rom_byte_mask <<= 1;
            if (rom_byte_mask == 0) {
                rom_byte_number++;
                rom_byte_mask = 1;
            }
        }
        
        // 检查是否找到设备
        if (bit_number == 65) {
            // 检查CRC
            uint8_t crc = calculate_crc(rom_code, 7);
            if (crc == rom_code[7]) {
                // 保存找到的ROM码
                memcpy(rom_codes[devices_found], rom_code, 8);
                devices_found++;
                
                printf("Found device %d, ROM: ", devices_found);
                for (int i = 0; i < 8; i++) {
                    printf("%02X ", rom_code[i]);
                }
                printf("\r\n");
                
                // 更新搜索状态
                last_discrepancy = last_zero;
                search_result = 1;
                
                if (devices_found >= MAX_DS18B20_SENSORS) {
                    // 达到最大支持数量
                    break;
                }
            } else {
                printf("CRC error in device search\r\n");
                search_result = 0;
            }
        } else {
            search_result = 0;
        }
        
        // 检查是否继续搜索
        if (!search_result || !last_discrepancy) {
            break;
        }
        
    } while (devices_found < MAX_DS18B20_SENSORS);
    
    // 如果处于学习模式，不要覆盖现有配置
    if (ds18b20_config_mode == CONFIG_MODE_NORMAL) {
        // 清空之前的配置
        for (uint8_t i = 0; i < MAX_DS18B20_SENSORS; i++) {
            ds18b20_devices[i].present = 0;
        }
        
        // 使用找到的ROM码更新设备数组
        for (uint8_t i = 0; i < devices_found; i++) {
            memcpy(ds18b20_devices[i].rom_code, rom_codes[i], 8);
            ds18b20_devices[i].present = 1;
        }
        
        ds18b20_count = devices_found;
    }
    
    printf("Search complete, found %d DS18B20 sensors\r\n", devices_found);
    return devices_found;
}

// 检测单个传感器
uint8_t DS18B20_DiscoverSingleSensor(uint8_t *rom_code)
{
    if (!ow_reset()) {
        return 0;  // 总线上没有设备
    }
    
    // 使用读ROM命令 (只有总线上只有一个设备时可用)
    ow_write_byte(DS18B20_CMD_READ_ROM);
    
    // 读取ROM码
    for (uint8_t i = 0; i < 8; i++) {
        rom_code[i] = ow_read_byte();
    }
    
    // 验证CRC
    uint8_t crc = calculate_crc(rom_code, 7);
    if (crc != rom_code[7]) {
        printf("CRC error in ROM code\r\n");
        return 0;
    }
    
    // 验证ROM码首字节是否为0x28 (DS18B20的家族码)
    if (rom_code[0] != 0x28) {
        printf("Device is not a DS18B20 (family code: %02X)\r\n", rom_code[0]);
        return 0;
    }
    
    printf("Discovered DS18B20, ROM: ");
    for (int i = 0; i < 8; i++) {
        printf("%02X ", rom_code[i]);
    }
    printf("\r\n");
    
    return 1;
}

// 学习特定位置的传感器
void DS18B20_LearnSensor(uint8_t position)
{
    if (position >= MAX_DS18B20_SENSORS) {
        printf("Invalid position %d\r\n", position);
        return;
    }
    
    printf("Learning sensor for position %d...\r\n", position + 1);
    printf("Please ensure only ONE sensor is connected to the bus\r\n");
    
    uint8_t rom_code[8];
    if (DS18B20_DiscoverSingleSensor(rom_code)) {
        // 保存ROM码到指定位置
        memcpy(ds18b20_devices[position].rom_code, rom_code, 8);
        ds18b20_devices[position].present = 1;
        
        printf("Learned sensor for position %d: ", position + 1);
        for (int i = 0; i < 8; i++) {
            printf("%02X ", rom_code[i]);
        }
        printf("\r\n");
        
        // 测试一下传感器
        float temp = DS18B20_ReadTemperature(position);
        if (temp > DS18B20_TEMP_MIN && temp < DS18B20_TEMP_MAX) {
            printf("Sensor test successful: %.2f°C\r\n", temp);
        } else {
            printf("Sensor test failed: %.2f°C\r\n", temp);
        }
    } else {
        printf("Failed to learn sensor for position %d\r\n", position + 1);
    }
}

// 设置配置模式
void DS18B20_SetConfigMode(uint8_t mode)
{
    if (mode > CONFIG_MODE_LEARNING) {
        return;
    }
    
    ds18b20_config_mode = mode;
    printf("DS18B20 config mode set to: %s\r\n", 
           mode == CONFIG_MODE_NORMAL ? "Normal" : "Learning");
}

// 获取当前配置模式
uint8_t DS18B20_GetConfigMode(void)
{
    return ds18b20_config_mode;
}

// 保存配置到Flash
void DS18B20_SaveConfig(void)
{
    ds18b20_config_t config;
    config.magic = DS18B20_CONFIG_MAGIC;
    config.configured = 1;
    memcpy(config.devices, ds18b20_devices, sizeof(ds18b20_devices));
    
    // 擦除配置页
    if (!Flash_ErasePage(FLASH_CONFIG_PAGE_ADDR)) {
        printf("Failed to erase Flash page\r\n");
        return;
    }
    
    // 写入配置数据
    if (!Flash_WriteData(FLASH_CONFIG_PAGE_ADDR, (uint32_t *)&config, sizeof(config) / 4)) {
        printf("Failed to write configuration to Flash\r\n");
        return;
    }
    
    printf("Configuration saved successfully\r\n");
}

// 从Flash加载配置
uint8_t DS18B20_LoadConfig(void)
{
    ds18b20_config_t *config = (ds18b20_config_t *)FLASH_CONFIG_PAGE_ADDR;
    
    // 检查魔术数字和配置标志
    if (config->magic != DS18B20_CONFIG_MAGIC || !config->configured) {
        printf("No valid configuration found in Flash\r\n");
        return 0;
    }
    
    // 加载设备配置
    memcpy(ds18b20_devices, config->devices, sizeof(ds18b20_devices));
    
    printf("Configuration loaded from Flash\r\n");
    
    // 打印配置信息
    DS18B20_PrintConfig();
    
    return 1;
}

// 打印当前配置
void DS18B20_PrintConfig(void)
{
    printf("\r\n--- DS18B20 Sensor Configuration ---\r\n");
    
    for (uint8_t i = 0; i < MAX_DS18B20_SENSORS; i++) {
        printf("Position %d: ", i + 1);
        
        if (ds18b20_devices[i].present) {
            printf("ROM: ");
            for (uint8_t j = 0; j < 8; j++) {
                printf("%02X ", ds18b20_devices[i].rom_code[j]);
            }
            printf("\r\n");
        } else {
            printf("Not configured\r\n");
        }
    }
    
    printf("------------------------------------\r\n\n");
	}



uint8_t DS18B20_CheckSensorPresent(uint8_t sensor_index)
{
    if (sensor_index >= MAX_DS18B20_SENSORS) return 0;
    
    // 复位总线
    if (!ow_reset()) {
        return 0;  // 总线上没有设备
    }
    
    // 使用匹配ROM命令
    ow_write_byte(DS18B20_CMD_MATCH_ROM);
    
//    // 发送预定义的ROM码
//    for (uint8_t i = 0; i < 8; i++) {
//        ow_write_byte(ds18b20_devices[sensor_index].rom_code[i]);
//    }
    // 发送ROM码
    for (uint8_t i = 0; i < 8; i++) {
        ow_write_byte(ds18b20_devices[sensor_index].rom_code[i]);
    }
    // 发送读取暂存器命令
    ow_write_byte(DS18B20_CMD_READ_SCRATCHPAD);
    
    // 读取9字节暂存器数据
    uint8_t scratchpad[9];
    for (uint8_t i = 0; i < 9; i++) {
        scratchpad[i] = ow_read_byte();
    }
    
    // 复位总线
    ow_reset();
    
    // 通过验证CRC来检查传感器是否真的存在
    return (calculate_crc(scratchpad, 8) == scratchpad[8]);
}

// 启动所有传感器的温度转换
void DS18B20_StartConversion(void)
{
    if (ow_reset()) {
        ow_write_byte(DS18B20_CMD_SKIP_ROM);     // 跳过ROM命令 (广播命令)
        ow_write_byte(DS18B20_CMD_CONVERT_T);    // 启动温度转换
    }
}

// 启动指定传感器的温度转换
void DS18B20_StartConversionById(uint8_t sensor_id)
{
    if (sensor_id >= ds18b20_count) return;
    
    if (ow_reset()) {
        ow_write_byte(DS18B20_CMD_MATCH_ROM);    // 匹配ROM命令
        
        // 发送64位ROM码
        for (uint8_t i = 0; i < 8; i++) {
            ow_write_byte(ds18b20_devices[sensor_id].rom_code[i]);
        }
        
        ow_write_byte(DS18B20_CMD_CONVERT_T);    // 启动温度转换
    }
}

float DS18B20_ReadTemperature(uint8_t sensor_id)
{
    uint8_t temp_lsb, temp_msb;
    uint8_t scratchpad[9];
    int16_t raw_temp;
    float temperature;
    
    if (sensor_id >= MAX_DS18B20_SENSORS || !ds18b20_devices[sensor_id].present) {
        return -999.0f;  // 无效的传感器ID
    }
    
    // 添加重试机制
    uint8_t retry = 3;
    uint8_t success = 0;
    
    while (retry-- && !success) {
        // 先复位总线
        if (!ow_reset()) {
            Delay_ms(10);
            continue;  // 重置失败，重试
        }
        
        // 发送匹配ROM命令
        ow_write_byte(DS18B20_CMD_MATCH_ROM);    
        
        // 发送64位ROM码
        for (uint8_t i = 0; i < 8; i++) {
            ow_write_byte(ds18b20_devices[sensor_id].rom_code[i]);
        }
        
        // 发送转换命令
        ow_write_byte(DS18B20_CMD_CONVERT_T);
        
        // 等待转换完成
        Delay_ms(1000);  // 使用更精确的延时
        
        // 检查转换是否完成（可选）
        ow_input_mode();
        uint8_t conversion_done = GPIO_ReadInputDataBit(OW_PORT, OW_PIN);
        if (!conversion_done) {
            Delay_ms(250); // 额外等待时间
        }
        
        // 再次复位总线
        if (!ow_reset()) {
            Delay_ms(10);
            continue;
        }
        
        // 再次发送匹配ROM命令
        ow_write_byte(DS18B20_CMD_MATCH_ROM);    
        
        // 再次发送64位ROM码
        for (uint8_t i = 0; i < 8; i++) {
            ow_write_byte(ds18b20_devices[sensor_id].rom_code[i]);
        }
        
        // 发送读暂存器命令
        ow_write_byte(DS18B20_CMD_READ_SCRATCHPAD);
        
        // 读取9字节暂存器数据
        for (uint8_t i = 0; i < 9; i++) {
            scratchpad[i] = ow_read_byte();
        }
        
        // 验证CRC
        if (calculate_crc(scratchpad, 8) == scratchpad[8]) {
            success = 1;
        } else {
            Delay_ms(10); // 延时后重试
        }
    }
    
    if (!success) {
        // 标记传感器为不存在
        ds18b20_devices[sensor_id].present = 0;
        printf("CRC Error for sensor %d, marking as disconnected\r\n", sensor_id+1);
        return -999.0f;
    }
    
    // 解析温度值
    temp_lsb = scratchpad[0];
    temp_msb = scratchpad[1];
    
    raw_temp = (int16_t)((temp_msb << 8) | temp_lsb);
    temperature = raw_temp * 0.0625f;
    
    // 保存最新温度值
    ds18b20_devices[sensor_id].last_temperature = temperature;
    
    return temperature;
}

// 修改读取所有传感器温度的函数
void DS18B20_ReadAllTemperatures(float *temperatures)
{
    // 为所有传感器（包括不存在的）设置一个默认的错误值
    for (uint8_t i = 0; i < MAX_DS18B20_SENSORS; i++) {
        temperatures[i] = -999.0f;
    }
    
    // 只读取存在的传感器
    for (uint8_t i = 0; i < MAX_DS18B20_SENSORS; i++) {
        if (ds18b20_devices[i].present) {
            float temp = DS18B20_ReadTemperature(i);
            temperatures[i] = temp;
            #if DS18B20_debug_flag
            // 打印传感器编号和温度
            printf("Sensor %d ROM: ", i+1);
            for (int j = 0; j < 8; j++) {
                printf("%02X ", ds18b20_devices[i].rom_code[j]);
            }
            printf(" Temp: %.2f°C\r\n", temp);
						#endif
        }
    }
}
// 配置传感器分辨率 (9-12位)
// resolution: 0=9位(0.5°C), 1=10位(0.25°C), 2=11位(0.125°C), 3=12位(0.0625°C)
void DS18B20_SetResolution(uint8_t sensor_id, uint8_t resolution)
{
    uint8_t config;
    
    if (sensor_id >= ds18b20_count || !ds18b20_devices[sensor_id].present) {
        return;  // 无效的传感器ID
    }
    
    // 范围检查并计算配置寄存器值
    if (resolution > 3) resolution = 3;
    config = 0x1F | (resolution << 5);  // 配置寄存器 (位5-6为分辨率)
    
    if (!ow_reset()) {
        return;  // 重置失败
    }
    
    ow_write_byte(DS18B20_CMD_MATCH_ROM);    // 匹配ROM命令
    
    // 发送64位ROM码
    for (uint8_t i = 0; i < 8; i++) {
        ow_write_byte(ds18b20_devices[sensor_id].rom_code[i]);
    }
    
    ow_write_byte(DS18B20_CMD_WRITE_SCRATCHPAD);  // 写暂存器命令
    ow_write_byte(0x00);        // TH寄存器 (高温报警阈值)
    ow_write_byte(0x00);        // TL寄存器 (低温报警阈值)
    ow_write_byte(config);      // 配置寄存器
}


// 获取已发现的传感器数量
uint8_t DS18B20_GetSensorCount(void)
{
    return ds18b20_count;
}

// 获取指定传感器的ROM码
void DS18B20_GetROMCode(uint8_t sensor_id, uint8_t *rom_code)
{
    if (sensor_id >= ds18b20_count) return;
    
    for (uint8_t i = 0; i < 8; i++) {
        rom_code[i] = ds18b20_devices[sensor_id].rom_code[i];
    }
}

// 根据ROM码查找传感器ID
int8_t DS18B20_FindSensorByROM(uint8_t *rom_code)
{
    for (uint8_t i = 0; i < ds18b20_count; i++) {
        uint8_t match = 1;
        for (uint8_t j = 0; j < 8; j++) {
            if (ds18b20_devices[i].rom_code[j] != rom_code[j]) {
                match = 0;
                break;
            }
        }
        if (match) return i;
    }
    return -1;  // 未找到
}
