/*
 * sys_utils.h
 *
 *  Created on: Apr 4, 2025
 *      Author: marvin
 */

#ifndef SYSTEM_UTILS_INC_MACROS_H_
#define SYSTEM_UTILS_INC_MACROS_H_

#ifndef MAX
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#endif

#ifndef MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif

#ifndef CLAMP
#define CLAMP(value, min, max) ((value) < (min) ? (min) : ((value) > (max) ? (max) : (value)))
#endif

#ifndef CLAMP_FLOAT
#define CLAMP_FLOAT(val, min, max) (fmaxf((min), fminf((val), (max))))
#endif


#ifndef SIGN
#define SIGN(x) ((x > 0) ? 1 : ((x < 0) ? -1 : 0))
#endif

#ifndef IS_TIM_APB1_INSTANCE
#define IS_TIM_APB1_INSTANCE(TIMx) \
(((TIMx) == TIM2)  || ((TIMx) == TIM3)  || \
((TIMx) == TIM4)  || ((TIMx) == TIM5)  || \
((TIMx) == TIM12) || ((TIMx) == TIM13) || \
((TIMx) == TIM14) || ((TIMx) == TIM6)  || \
((TIMx) == TIM7))
#endif


// Q16.16格式转换
#ifndef Q16_16_TO_FLOAT
#define Q16_16_TO_FLOAT(x) ((float)(x) / 65536.0f)
#endif

#ifndef Q15_TO_FLOAT
#define Q15_TO_FLOAT(x) ((float)(x) / 32768.0f)
#endif

#ifndef Q7_TO_FLOAT
#define Q7_TO_FLOAT(x)  ((float)(x) / 128.0f)
#endif

#endif /* SYSTEM_UTILS_INC_MACROS_H_ */
