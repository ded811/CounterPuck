/* identity.h: who this puck says it is. The values come from puck_config.h. MIT licence. */
#pragma once
#include <stdint.h>

void identity_init(void);
const char *identity_serial(void);        /* 13 characters, the USB serial number */
const char *identity_board_serial(void);  /* 13 characters */
const char *identity_revision(void);      /* short revision string */
uint32_t identity_build_time(void);
uint32_t identity_board_revision(void);
