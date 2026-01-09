#pragma once
#include <stdint.h>

#define MAX_PLAYERS 4
#define MAX_FRUITS 4
#define MAX_SEGMENTS 64
#define SNAKE_NAME_MAX 32

enum {
    MSG_HELLO = 1,
    MSG_WELCOME = 2,
    MSG_CONFIG = 3,
    MSG_INPUT = 4,
    MSG_STATE = 5,
    MSG_PAUSE_TOGGLE = 6,
    MSG_LEAVE = 7,
    MSG_BYE = 8
};

#pragma pack(push, 1)

typedef struct {
    int16_t x;
    int16_t y;
} Cell;

typedef struct {
    uint16_t type;
    uint32_t len;
} MsgHeader;

typedef struct {
    char name[SNAKE_NAME_MAX];
} MsgHello;

typedef struct {
    uint32_t player_id;
} MsgWelcome;

typedef struct {
    uint16_t w;
    uint16_t h;
    uint8_t mode;
    uint8_t world;
    uint16_t time_limit_sec;
    uint32_t map_len;
} MsgConfig;

typedef struct {
    uint8_t dir;
} MsgInput;

typedef struct {
    uint8_t player_id;
    uint8_t connected;
    uint8_t active;
    uint8_t alive;
    uint8_t paused;
    uint16_t score;
    uint8_t dir;
    uint16_t len;
    Cell body[MAX_SEGMENTS];
} PlayerState;

typedef struct {
    Cell pos;
    uint32_t visited_mask;
} FruitState;

typedef struct {
    uint32_t tick_ms;
    uint8_t game_over;
    uint8_t mode;
    uint16_t w;
    uint16_t h;
    uint16_t time_left_sec;
    uint16_t global_freeze_ms;
    uint8_t num_players;
    PlayerState players[MAX_PLAYERS];
    uint8_t num_fruits;
    FruitState fruits[MAX_FRUITS];
} MsgState;

#pragma pack(pop)
