/* SPDX-License-Identifier: MIT */
/* The Shift Light menu (plugin/ShiftLight.xml) and the files the plugin and the worker share. One place for both, so
 * tests/test_plugin.py can check the XML against them. */
#ifndef SL_PLUGIN_IDS_H
#define SL_PLUGIN_IDS_H

/* actions (raAction: handled by libShiftLight.so; hmiAction: XML only) and models (raBoolModel) */
#define SLP_ACT_ENTRY 10   /* hmiAction -> state 100 */
#define SLP_ACT_ON 21      /* checkbox "Shift light" */
#define SLP_ACT_POINT 22   /* hmiAction -> state 200 ("Shift point") */
#define SLP_ACT_SPORT 23   /* checkbox "Only in SPORT and SPORT+" */
#define SLP_ACT_MANUAL 24  /* checkbox "Only in manual mode (M/S)" */
#define SLP_ACT_RPM0 31    /* radio buttons 31..37: 5,000..6,500 rpm (6,750 and 7,000 went: the car holds revs and
                              cuts power before orange can be acted on, test car 2026-11-19) */
#define SLP_MODEL_ON 51
#define SLP_MODEL_SPORT 53
#define SLP_MODEL_MANUAL 54
#define SLP_MODEL_RPM0 61  /* 61..67 */
#define SLP_RPM_N 7
#define SLP_RPM_FIRST 5000
#define SLP_RPM_STEP 250
#define SLP_RPM_DEFAULT 5500

/* on the persistent partition, in the plugin's own folder (the switches survive a reboot) */
#define SLP_DIR "/var/opt/hmi/data/ID5/Plugins/ShiftLight"
#define SLP_F_ON "on"                /* exists: the shift light runs */
#define SLP_F_SPORT "sport-only"     /* exists: only in the sport modes of car-map */
#define SLP_F_MANUAL "manual-only"   /* exists: only in the manual gears of car-map */
#define SLP_F_RPM "shift-rpm"        /* the shift point, decimal */
#define SLP_F_MAP "car-map"          /* which raw car values count: "mode35up 3 4", "gear 5 6" (filled in after a test) */
#define SLP_F_PENDING "boot_pending" /* boot guard */
#define SLP_F_STRIKES "boot_strikes"
#define SLP_F_DISABLED "disabled"    /* exists: the plugin does nothing (remove it to enable again) */
#define SLP_F_EVENTS "events.log"     /* rare events of the plugin and the worker (flash, ~32 KB at most) */
#define SLP_F_STARTS "starts"         /* iDrive starts with the plugin, counted */
#define SLP_F_WORKER "shiftlight-worker.so" /* out/libshiftlight.so (a name the HMI's plugin loader cannot mix up) */

/* in RAM (/dev/shmem: gone at a reboot) */
#define SLP_CAR "/dev/shmem/shiftlight-car" /* the plugin's car data: mode, mode35up, gear, shift, nav, pwf (-1 unknown) */
#define SLP_PID "/dev/shmem/shiftlight.pid"     /* the worker's pid */
#define SLP_ASIDE "/dev/shmem/shiftlight-aside" /* exists while the worker leaves the HUD to another client */
#define SLP_PLOG "/dev/shmem/shiftlight-plugin.log"
#define SLP_WLOG "/dev/shmem/shiftlight.log"

#endif
