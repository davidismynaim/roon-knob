#include "room_cfg.h"

#include "platform/platform_storage.h"

static rk_room_t s_room = RK_ROOM_LOUNGE;

void room_cfg_init(void) {
    rk_room_cfg_t cfg;
    if (platform_storage_read_room(&cfg)) {
        s_room = (rk_room_t)cfg.room;
    } else {
        s_room = RK_ROOM_LOUNGE;
    }
}

rk_room_t room_cfg_get_current(void) {
    return s_room;
}

bool room_cfg_set_current(rk_room_t room) {
    s_room = room;
    rk_room_cfg_t cfg;
    rk_room_cfg_set_defaults(&cfg);
    cfg.room = (uint8_t)room;
    return platform_storage_write_room(&cfg);
}
