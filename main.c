
#include "..\\main.h"
#include "mycommon.h"
#include "ds18b20.h"
// Main function
int main(void) {
    HardWare_Init();
    SoftWare_Init();
    // Create start task
    xTaskCreate((TaskFunction_t)start_task,
                "start_task",
                START_STK_SIZE,
                NULL,
                START_TASK_PRIO,
                &StartTask_Handler);
    
    vTaskStartScheduler();
    
    while(1); // Should never reach here
}

void start_task(void* pvParameters) {
    taskENTER_CRITICAL();
    // 创建信号量（动态分配，使用 xSemaphoreCreateBinary）
    RS485_RECEIVE_DATA = xSemaphoreCreateBinary();
    if (RS485_RECEIVE_DATA == NULL)
        printf("RS485_RECEIVE_DATA create Err!\r\n");

    // 创建任务（使用 xTaskCreate）
    xTaskCreate((TaskFunction_t)USART1_Config_task,
                (const char*)"USART1_Config_task",
                (uint16_t)USART1_Receive_STK_SIZE,
                (void*)NULL,
                (UBaseType_t)USART1_Receive_TASK_PRIO,
                (TaskHandle_t*)&USART1_Receive_Handler);
			
		xTaskCreate((TaskFunction_t)watchdog_task,
								(const char*)"watchdog_task",
								(uint16_t)64,
								(void*)NULL,
								(UBaseType_t)1,
								(TaskHandle_t*)NULL);

    xTaskCreate((TaskFunction_t)LED_task,
                (const char*)"LED_task",
                (uint16_t)LED_STK_SIZE,
                (void*)NULL,
                (UBaseType_t)LED_TASK_PRIO,
                (TaskHandle_t*)&LED_Handler);

    xTaskCreate((TaskFunction_t)RS485_task,
                (const char*)"RS485_task",
                (uint16_t)RS485_STK_SIZE,
                (void*)NULL,
                (UBaseType_t)RS485_TASK_PRIO,
                (TaskHandle_t*)&RS485_Handler);

    // 启用 UART 中断
    USART_ITConfig(USART1, USART_IT_IDLE, ENABLE);
    taskEXIT_CRITICAL();

    // 删除自身任务
    vTaskDelete(StartTask_Handler);
}

// 在 FreeRTOS 中创建一个低优先级任务专门喂狗
void watchdog_task(void* pvParameters) {
    while (1) {
        Kick_Dog();
        vTaskDelay(500); // 每 500ms 喂狗
    }
}

void LED_task(void* pvParameters) {
    printf("LED_task Start......\r\n");
    uint8_t led_state = 0;
    if (Communication_mode_Switch) {
        LAN_LED_set(1);
    } else {
        TLE_LED_Set(1);
    }

    while (1) {
        if (onenet_info.net_work == 1 && led_state == 0) {
            Online_LED_Set(1);
            led_state = 1;
        } else if (onenet_info.net_work == 0 && led_state == 1) {
            Online_LED_Set(0);
            led_state = 0;
        }
				
        vTaskDelay(2000);
    }
}

void RS485_task(void* pvParameters) {
    BaseType_t err = pdFALSE;
    uint8_t button_pressed = 0;
    printf("RS485_task Start......\r\n");
    float temperatures[MAX_DS18B20_SENSORS] = {0}; // 存储5个温度点的数据
  
    // 初始化DS18B20系统
    DS18B20_Init();
    
    // 检查是否处于学习模式（可以通过按键触发）
    if (GPIO_ReadInputDataBit(BUTTON_GPIO, BUTTON_PIN) == 0) { //PB6连接了按键
        button_pressed = 1;
        printf("Button pressed at startup, entering learning mode...\r\n");
        DS18B20_SetConfigMode(CONFIG_MODE_LEARNING);
    }
    
    // 如果在学习模式，引导用户进行传感器学习
    if (DS18B20_GetConfigMode() == CONFIG_MODE_LEARNING) {
        printf("*** DS18B20 LEARNING MODE ***\r\n");
        printf("Please connect sensors one by one when prompted\r\n");
        
        for (uint8_t i = 0; i < MAX_DS18B20_SENSORS; i++) {
            printf("\r\nConnect ONLY the sensor for position %d, then press button\r\n", i+1);
            
            // 等待用户按下按键
            while (GPIO_ReadInputDataBit(BUTTON_GPIO, BUTTON_PIN) != 0) {
                vTaskDelay(100);
            }
            
            // 等待按键释放
            while (GPIO_ReadInputDataBit(BUTTON_GPIO, BUTTON_PIN) == 0) {
                vTaskDelay(100);
            }
            
            // 学习当前位置的传感器
            DS18B20_LearnSensor(i);
            
            printf("Please disconnect the sensor before continuing\r\n");
            vTaskDelay(2000);
        }
        
        // 学习完成，保存配置
        DS18B20_SaveConfig();
        printf("Learning complete! System will now restart in normal mode\r\n");
        
        // 切换回正常模式
        DS18B20_SetConfigMode(CONFIG_MODE_NORMAL);
        
        // 为了安全起见，重新初始化
        DS18B20_Init();
    }
    
    // 正常运行模式下，检查传感器状态并打印配置
    printf("Running in normal mode\r\n");
    uint8_t sensor_count = 0;
    for (uint8_t i = 0; i < MAX_DS18B20_SENSORS; i++) {
        if (ds18b20_devices[i].present) {
            sensor_count++;
        }
    }
    printf("Found %d configured DS18B20 sensors\r\n", sensor_count);
    
    // 设置所有存在的传感器为最高精度(12位)
    for (uint8_t i = 0; i < MAX_DS18B20_SENSORS; i++) {
        if (ds18b20_devices[i].present) {
            DS18B20_SetResolution(i, 3);
        }
    }
    
    while (1) {
        if (RS485_SEND_DATA != NULL) {
            err = xSemaphoreTake(RS485_SEND_DATA, (TickType_t)1000);
            if (err == pdTRUE) {
                USART2_Send_Read_sensor();//modbus-rtu
                
                // 周期性检查所有传感器状态
                for (uint8_t i = 0; i < MAX_DS18B20_SENSORS; i++) {
                    if (DS18B20_CheckSensorPresent(i)) {
                        ds18b20_devices[i].present = 1;
                    }
                }
                
                // 读取所有当前连接的传感器温度
                DS18B20_ReadAllTemperatures(temperatures);
                
                // 定义一个函数指针数组，指向所有温度点变量的地址
                float* temp_points[MAX_DS18B20_SENSORS] = {
                    &current_data.data_temp_point1,
                    &current_data.data_temp_point2,
                    &current_data.data_temp_point3,
                    &current_data.data_temp_point4,
                    &current_data.data_temp_point5
                };
                printf("\n");
                // 循环赋值，并添加温度范围检查
                for (uint8_t i = 0; i < MAX_DS18B20_SENSORS; i++) {
                    if (ds18b20_devices[i].present && temperatures[i] > -999.0f) {
                        // 判断温度是否在有效范围内
                        if (temperatures[i] >= DS18B20_TEMP_MIN && temperatures[i] <= DS18B20_TEMP_MAX) {
                            // 只有在传感器连接且温度在有效范围内时才更新数据
                            *temp_points[i] = temperatures[i];
                            printf("Position %d Temp: %.2f°C (Valid)\n", i+1, temperatures[i]);
                        } else {
                            // 温度超出范围，可能是传感器故障或噪声干扰
                            printf("Position %d Temp: %.2f°C (Out of range, not updated)\n", i+1, temperatures[i]);
                            // 可以选择在这里设置一个错误标志，或保持之前的有效值
                        }
                    } else {
                        printf("Position %d: Not connected or invalid reading\n", i+1);
                    }
                }
                
                upload_sensor_state = current_sensor_state;
                memset(&upload_server_data, 0, sizeof(collector_data));
                upload_server_data = current_data;
                memset(&lcd_data, 0, sizeof(collector_data));
                lcd_data = current_data;
            }
        }
        
        // 检查按键，是否进入学习模式
        if (!button_pressed && GPIO_ReadInputDataBit(BUTTON_GPIO, BUTTON_PIN) == 0) {
            button_pressed = 1;
            printf("Button pressed! Hold for 3 seconds to enter learning mode...\r\n");
            
            // 等待3秒确认长按
            uint8_t count = 0;
            while (GPIO_ReadInputDataBit(BUTTON_GPIO, BUTTON_PIN) == 0 && count < 30) {
                vTaskDelay(100);
                count++;
            }
            
            if (count >= 30) {
                printf("Entering learning mode...\r\n");
                
                // 等待按键释放
                while (GPIO_ReadInputDataBit(BUTTON_GPIO, BUTTON_PIN) == 0) {
                    vTaskDelay(100);
                }
                
                // 重启系统进入学习模式
                NVIC_SystemReset();
            } else {
                printf("Button released too early, continuing normal operation\r\n");
            }
        } else if (GPIO_ReadInputDataBit(BUTTON_GPIO, BUTTON_PIN) != 0) {
            button_pressed = 0;
        }
        
        vTaskDelay(1000);
    }
}

void USART1_Config_task(void* pvParameters) {
    BaseType_t err = pdFALSE;
    printf("USART1_Config_task Start......\r\n");

    while (1) {
        if (USART1_RECEIVE_DATA != NULL) {
            err = xSemaphoreTake(USART1_RECEIVE_DATA, (TickType_t)1000);

            if (err == pdTRUE) {
                Usart1_receive_process_event();
                memset(Usart1.uart_buf, 0, sizeof(Usart1.uart_buf));
                SysMng.USART1_RECEIVETIME = 0;
            }
        }
        vTaskDelay(150);
    }
}


