#ifndef I2C_TP_H
#define I2C_TP_H

/* Intel LPSS I2C + HID over I2C 内置触控板驱动。
 * 懒初始化 + 轮询：由 PS/2 鼠标轮询路径在事件队列为空时调用。 */
void i2c_tp_poll(void);

#endif /* I2C_TP_H */
