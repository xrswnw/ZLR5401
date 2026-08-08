#ifndef __APP_STDLIB_H
#define __APP_STDLIB_H

/* 位操作辅助宏*/
#define StateBit_IsSet(obj, state)   (((obj) & (state)) != 0)
#define StateBit_Clear(obj, state)    ((obj) &= (~(state)))
#define StateBit_Set(obj, state)      ((obj) |= (state))
#define State_Set(obj, state)         ((obj) = (state))

#endif /* __APP_STDLIB_H */
