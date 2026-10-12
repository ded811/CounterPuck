/* bonds.h: the pairing record of each slot, kept in flash. MIT licence. */
#pragma once
#include <stdbool.h>
#include <stdint.h>

/* 24 bytes; also the format vendor commands 0xA3 and 0xA2 read and write. */
struct bond {
	uint32_t puck_id;       /* random, chosen when pairing */
	uint32_t controller_id;
	char controller_serial[16];
} __packed;

/* A pairing exists when both IDs are non-zero. */
bool bond_valid(const struct bond *b);

/* Read the records from flash into memory. */
int bonds_load(void);
const struct bond *bond_get(int slot);
/* Store a record, or delete it when either ID is zero. */
int bond_set(int slot, const struct bond *b);
int bonds_clear_all(void);
