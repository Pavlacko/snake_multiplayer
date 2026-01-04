#ifndef TYPES_H
#define TYPES_H

#include <stdint.h>

typedef struct {
    int32_t x;
    int32_t y;
} Vec2i;

typedef enum {
    DIR_UP = 0,
    DIR_RIGHT = 1,
    DIR_DOWN = 2,
    DIR_LEFT = 3
} Direction;

#endif
