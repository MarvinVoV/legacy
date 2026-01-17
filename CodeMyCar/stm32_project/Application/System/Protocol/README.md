## 协议分层设计

+---------------------+
| UART Frame Header | protocol_header_t
+---------------------+
| Application Payload | 自定义指令结构
+---------------------+
| Checksum & Frame End|
+---------------------+

## 控制掩码工具方法使用示例

```c++
uint8_t fields = CTRL_FIELD_MOTOR | CTRL_FIELD_SERVO;

// 检查是否包含电机控制
if(is_ctrl_field_set(fields, CTRL_FIELD_MOTOR)) {
// 处理电机指令
}

// 检查是否同时包含电机和舵机
if(are_ctrl_fields_set(fields, CTRL_FIELD_MOTOR|CTRL_FIELD_SERVO)) {
// 处理组合指令
}

```