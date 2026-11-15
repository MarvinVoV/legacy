/*
 * display_eye.h
 *
 * 机器人双眼：局部刷新、自然眨眼、视线与情绪平滑过渡。
 */

#ifndef MODULES_DISPLAY_INC_DISPLAY_EYE_H_
#define MODULES_DISPLAY_INC_DISPLAY_EYE_H_

#include <stdbool.h>
#include <stdint.h>

/** 启动眼睛动画任务（内部完成首帧渲染） */
void DisplayEye_Start(void);

/** 停止动画，当前传输完成后清屏（任务上下文调用） */
void DisplayEye_Stop(void);

/** 设置视线方向（-1..1，右/下为正），可被外部状态调用 */
void DisplayEye_SetGaze(float x, float y);

/** 设置开合度（0=闭眼，1=睁眼） */
void DisplayEye_SetOpenness(float openness);

/** Atomically update the semantic mood and conversation phase. */
void DisplayEye_SetExpression(uint8_t mood, uint8_t voice);

/** 设置双眼主色（RGB565，兼容原虹膜接口） */
void DisplayEye_SetIrisColor(uint16_t color);

/** 恢复自动视线、眨眼与情绪配色；保留当前情绪 */
void DisplayEye_ResetOverrides(void);

#endif /* MODULES_DISPLAY_INC_DISPLAY_EYE_H_ */
