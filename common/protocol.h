#pragma once
#include <stdint.h>
#include "types.h"

typedef enum {
    MSG_HELLO   = 1,
    MSG_WELCOME = 2,
    MSG_PING    = 3,
    MSG_BYE     = 4
} MsgType;

typedef struct __attribute__((packed)) {
    uint16_t type;   
    uint32_t len;    
} MsgHeader;

typedef struct __attribute__((packed)) {
    char name[SNAKE_NAME_MAX];
} MsgHello;

typedef struct __attribute__((packed)) {
    int32_t player_id;   
} MsgWelcome;
