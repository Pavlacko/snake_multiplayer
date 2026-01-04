#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <stdint.h>

#define SNAKE_PROTO_MAGIC 0x534E414B  
#define SNAKE_PROTO_VER   1
#define SNAKE_NAME_MAX    32

typedef enum {
    MSG_HELLO   = 1,  
    MSG_WELCOME = 2,
    MSG_PING    = 3,  
    MSG_PONG    = 4,  
    MSG_BYE     = 5   
} MsgType;

#pragma pack(push, 1)
typedef struct {
    uint32_t magic;   
    uint16_t version; 
    uint16_t type;    
    uint32_t len; 
} MsgHeader;
#pragma pack(pop)

typedef struct {
    char name[SNAKE_NAME_MAX];
} MsgHello;

typedef struct {
    int32_t player_id;
} MsgWelcome;

#endif
