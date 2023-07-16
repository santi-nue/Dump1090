/**\file    airports.h
 * \ingroup Main
 */
#ifndef _AIRPORTS_H
#define _AIRPORTS_H

#include "misc.h"

/**
 * \def AIRPORT_DATABASE_CSV
 * Our default airport-database relative to `Modes.where_am_I`.
 */
#define AIRPORT_DATABASE_CSV   "airport-codes.csv"

/**
 * \def AIRPORT_DATABASE_CACHE
 * Our airport API cache in the `%TEMP%\\dump1090` directory.
 */
#define AIRPORT_DATABASE_CACHE  "airport-api-cache.csv"

/**
 * \def AIRPORT_FREQ_CSV
 * Our airport-frequency database relative to `Modes.where_am_I`.
 */
#define AIRPORT_FREQ_CSV  "airport-frequencies.csv"

uint32_t airports_init (void);
void     airports_exit (bool free_airports);
void     airports_show_stats (void);
bool     airports_update_CSV (const char *file);

void     airports_API_show_stats (void);
void     airports_API_remove_stale (uint64_t now);
bool     airports_API_get_flight_info (const char *call_sign, uint32_t addr,
                                       const char **departure, const char **destination);

void     airports_API_flight_log_entering (const aircraft *a);
void     airports_API_flight_log_resolved (const aircraft *a);
void     airports_API_flight_log_leaving (const aircraft *a);

#endif /* _AIRPORTS_H */
