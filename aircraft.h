/**\file    aircraft.h
 * \ingroup Main
 */
#ifndef _AIRCRAFT_H
#define _AIRCRAFT_H

#include "misc.h"

/**
 * \def AIRCRAFT_DATABASE_CSV
 * Our default aircraft-database relative to `Modes.where_am_I`.
 */
#define AIRCRAFT_DATABASE_CSV   "aircraftDatabase.csv"

/**
 * \def AIRCRAFT_DATABASE_URL
 * The default URL for the `--database-update` option.
 */
#define AIRCRAFT_DATABASE_URL   "https://opensky-network.org/datasets/metadata/aircraftDatabase.zip"

/**
 * \def AIRCRAFT_DATABASE_TMP
 * The basename for downloading a new `aircraftDatabase.csv`.
 *
 * E.g. Use WinInet API to download:<br>
 *  `AIRCRAFT_DATABASE_URL` -> `c:\temp\aircraft-database-temp.zip`
 *
 * extract this using: <br>
 *  `unzip -p c:\temp\aircraft-database-temp.zip > c:\temp\aircraft-database-temp.csv`
 *
 * (the `-p` option ignores the embedded path: `media/data/samples/metadata/aircraftDatabase.csv`) <br>
 * and finally call: <br>
 *   `CopyFile ("c:\\temp\\aircraft-database-temp.csv", <final_destination>)`.
 */
#define AIRCRAFT_DATABASE_TMP  "aircraft-database-temp"

/**
 * \enum a_show_t
 * The "show-state" for an aircraft.
 */
typedef enum a_show_t {
        A_SHOW_FIRST_TIME = 1,
        A_SHOW_LAST_TIME,
        A_SHOW_NORMAL,
        A_SHOW_NONE,
      } a_show_t;

/**
 * Describes an aircraft from a .CSV-file.
 */
typedef struct aircraft_CSV {
        uint32_t addr;
        char     reg_num [10];
        char     manufact [30];
        char     call_sign [20];
      } aircraft_CSV;

/**
 * Structure used to describe an aircraft in interactive mode.
 */
typedef struct aircraft {
        uint32_t addr;              /**< ICAO address */
        char     flight [9];        /**< Flight number */
        int      altitude;          /**< Altitude */
        uint32_t speed;             /**< Velocity computed from EW and NS components. In Knots. */
        int      heading;           /**< Horizontal angle of flight. */
        bool     heading_is_valid;  /**< Have a valid heading. */
        uint64_t seen_first;        /**< Tick-time (in milli-sec) at which the first packet was received. */
        uint64_t seen_last;         /**< Tick-time (in milli-sec) at which the last packet was received. */
        uint64_t EST_seen_last;     /**< Tick-time (in milli-sec) at which the last estimated position was done. */
        uint32_t messages;          /**< Number of Mode S messages received. */
        int      identity;          /**< 13 bits identity (Squawk). */
        a_show_t show;              /**< The plane's show-state */
        double   distance;          /**< Distance (in meters) to home position */
        double   EST_distance;      /**< Estimated `distance` based on last `speed` and `heading` */
        double   sig_levels [4];    /**< RSSI signal-levels from the last 4 messages */
        int      sig_idx;

        /* Encoded latitude and longitude as extracted by odd and even
         * CPR encoded messages.
         */
        int      odd_CPR_lat;       /**< Encoded odd CPR latitude */
        int      odd_CPR_lon;       /**< Encoded odd CPR longitude */
        int      even_CPR_lat;      /**< Encoded even CPR latitude */
        int      even_CPR_lon;      /**< Encoded even CPR longitude */
        uint64_t odd_CPR_time;      /**< Tick-time for reception of an odd CPR message */
        uint64_t even_CPR_time;     /**< Tick-time for reception of an even CPR message */
        pos_t    position;          /**< Coordinates obtained from decoded CPR data. */
        pos_t    EST_position;      /**< Estimated position based on last `speed` and `heading`. */

        const aircraft_CSV *CSV;  /**< A pointer to a CSV record (or NULL). */
        struct aircraft    *next; /**< Next aircraft in our linked list. */
      } aircraft;


#ifdef USE_SQLITE3
bool aircraft_sql3_create_db (const char *db_file);
bool aircraft_sql3_add_entry (const aircraft_CSV *rec);
#endif

bool                aircraft_CSV_load (void);
bool                aircraft_CSV_update (const char *db_file, const char *url);
const aircraft_CSV *aircraft_CSV_lookup_entry (uint32_t addr);
aircraft           *aircraft_create (uint32_t addr, uint64_t now);
aircraft           *aircraft_find (uint32_t addr);
int                 aircraft_numbers (void);
uint32_t            aircraft_get_addr (uint8_t a0, uint8_t a1, uint8_t a2);
const char         *aircraft_get_details (const uint8_t *_a);
const char         *aircraft_get_country (uint32_t addr);
bool                aircraft_is_military (uint32_t addr);

#endif /* _AIRCRAFT_H */