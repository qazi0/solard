/*
 * solard -- a tiny local monitor for GoodWe hybrid inverters (ET family:
 * ES, ET, EH, BT, BH), with its own dashboard and history.
 *
 * Reads the inverter's WiFi/LAN dongle over Modbus/TCP (port 502, unit 0xF7)
 * every 5 seconds and serves on port 8768:
 *
 *   GET /              the dashboard page (built into the binary)
 *   GET /api/now       latest reading, battery estimates, today's energy, JSON
 *   GET /api/day?d=YYYY-MM-DD[&x=0]   1-minute curve, detail series ("x", left
 *                      out with x=0) and the day's totals
 *   GET /api/days?from=YYYYMMDD&to=YYYYMMDD   daily totals (week/month/year charts)
 *   GET /api/config    battery size, charge limit and location for the page
 *   GET /ping          "solard ok" (how the QuickSolar app finds this server)
 *
 * Settings, each overriding the one before (none are required):
 *   solard.conf        key=value lines: host, port, unit, http, capacity, lat, lon,
 *                      data, www, utc_offset  (--config FILE, default next to the binary)
 *   SOLARD_HOST, SOLARD_PORT, SOLARD_HTTP, SOLARD_CAPACITY, SOLARD_LAT, SOLARD_LON, SOLARD_DATA
 *   --host --port --http --capacity --lat --lon --data --www --utc-offset
 * Without a host, solard finds the inverter itself (GoodWe's WiFi-kit discovery
 * broadcast, then a scan of the local /24 for Modbus/TCP) and saves it to
 * solard.conf; it searches again if the inverter stops answering (new DHCP address).
 *   solard --discover  prints what it finds and exits.
 *
 * History: the inverter keeps no readable history, so solard records its own
 * in data/ (all of it survives restarts):
 *   YYYY-MM-DD.v2.bin  one 48-byte rec2_t per minute: powers plus battery /
 *                      PV-string / output / grid / temperature detail
 *   YYYY-MM-DD.bin     older 16-byte format (still read and merged)
 *   days.csv           one line per day: totals + measured / counter breakdown
 *   state.csv          the running day + the last reading, rewritten every minute
 *                      and on shutdown, so a restart continues the integration
 *
 * Read-only: it only ever sends function 0x03 (read holding registers).
 * Single-threaded; one poll takes ~0.7 s, HTTP requests are answered between polls.
 * Plain C, no dependencies. Built with Cosmopolitan libc, one binary runs on
 * Linux, macOS, Windows and the BSDs (x86-64 and ARM64); any POSIX cc works too.
 *
 * Register map and scaling from github.com/marcelblijleven/goodwe (et.py, sensor.py):
 *   35100..35224  runtime data   37000..37023  BMS   45356/45358  reserve %
 *   47760  SoC upper limit (charge limit)
 */
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define POLL_MS       5000
#define GAP_S         60.0          /* readings further apart than this are a gap (see "energy") */
#define NA            (-9999.0)     /* "not reported" -> JSON null */

static char host_buf[64];             /* inverter address; empty = find it (see "discovery") */
static const char *inv_host = host_buf;
static int inv_port = 502, inv_unit = 0xF7;
static int http_port = 8768;
static double capacity_kwh = 0;      /* usable battery kWh (setting; batteries don't report it); 0 = unknown */
static int charge_limit = -1;        /* % where the inverter stops charging: read from 47760 (soc_upper_limit);
                                        -1 = not reported */
static double site_lat = -9999, site_lon = -9999;   /* for sunrise/sunset on the page; unset = null */
static int utc_off_min = -9999;      /* fixed UTC offset in minutes, or -9999 = the system's time zone */
static char www_dir[512];            /* serve index.html from here instead of the built-in page */
static char conf_path[600];

/* ---------------------------------------------------------------- state */

typedef struct {
    int ok;                     /* 1 once a read has succeeded */
    long long ts;               /* unix ms of this reading */
    /* PV strings */
    double vpv1, ipv1, vpv2, ipv2;
    int ppv1, ppv2, ppv, pv1_mode, pv2_mode;
    /* grid side */
    double vgrid, igrid, fgrid;
    int pgrid, inv_total_w;     /* inverter's on-grid port power (35125), total inverter power (35138) */
    int grid_w;                 /* active_power (meter): + export, - import */
    int grid_mode;
    /* AC output (back-up port) and loads */
    double vout, iout, fout;
    int pout;                   /* backup_p1 */
    int backup_w, load_w;       /* backup_ptotal, load_ptotal */
    int house_w;                /* house load = inverter AC output + grid import (see poll_inverter) */
    int ups_load;
    /* inverter */
    double t_air, t_module, inv_temp, vbus, vnbus;
    int work_mode, warning_code, h_total;
    unsigned error_bits, diag_bits;
    /* battery */
    double vbat, ibat;          /* ibat: + discharging */
    double bat_vi_w;            /* V x I, + discharging */
    int bat_w;                  /* the inverter's own battery power figure, + discharging */
    int bat_mode;
    int bms, bat_status, chg_lim, dis_lim, modules, bat_protocol;
    unsigned bat_err, bat_warn; /* (H << 16) | L */
    double bat_temp, cell_tmax, cell_tmin, cell_vmax, cell_vmin;
    int soc, soh;
    /* the inverter's own counters, kWh */
    double e_day, e_total, e_exp, e_imp, e_load, e_chg, e_dis;
    double e_exp_total, e_imp_total, e_load_total, e_chg_total, e_dis_total;
} reading_t;

static reading_t cur;
static int reserve_on = 10, reserve_off = 10;    /* % the inverter keeps back */
static double bat_ema = 0;                       /* battery power averaged over ~5 min, W */
static int ema_init = 0;
static char last_err[160] = "starting";
static long long last_err_ts;
static volatile sig_atomic_t stop_now;

static long long now_ms(void) {
    struct timeval tv; gettimeofday(&tv, NULL);
    return (long long)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

/* ---------------------------------------------------------------- energy
 *
 * The inverter's own daily counters are coarse (0.1 kWh) and, for the battery,
 * wrong: its "today battery discharge" (35211) under-reads -- 0.3 kWh counted
 * while V x I summed to ~0.55 kWh over the same 65 minutes. So solard sums the
 * energy itself from every 5-second reading:
 *
 *   solar      ppv1 + ppv2                                    (35105, 35109)
 *   house      inverter AC output + grid import: total_inverter_power - active_power
 *              (35138, 35140; the back-up port 35170 feeds only part of the house)
 *   battery    V x I (35180 x 35181): + = discharge, - = charge. The inverter's
 *              reported pbattery (35182) is summed too, for comparison only.
 *   grid       active_power (35140): - = import, + = export
 *   losses     (solar + discharge + import) - (house + charge + export), per day
 *
 * Integration: trapezoid between consecutive readings with their actual spacing.
 * Readings more than GAP_S (60 s) apart are a gap and are not integrated.
 *
 * RULE -- day total = measured + fill, per quantity. "fill" comes from the
 * inverter's own daily counters, only for stretches solard did not measure:
 *   - start of the day: when the day's first reading does not follow on from a
 *     reading just before midnight (solard started mid-day, was down over
 *     midnight, or solard was just installed mid-day), the
 *     counter's value then is the energy since the inverter's midnight;
 *   - gaps (> 60 s without a reading, restarts): the counter's increase across
 *     the gap (its new value if it was reset in between);
 *   - the inverter resets its counters at its own midnight, which can lag ours:
 *     if a day's first reading comes in its first hour and the load counter has
 *     not dropped below yesterday's, the counters still hold yesterday's totals
 *     and serve only as the reference for differences, never as a baseline.
 * Grid export has no usable counter (35199 tracks AC output on this system, and
 * export is not enabled), so its fill is always 0.
 * Per day the counters themselves are kept (counter_*, last reading of the day)
 * for comparison, and measured, fill, minutes measured / gap minutes and the
 * first measured minute are reported, so the page can say when a total leans
 * on the counters.
 */
enum { Q_PV, Q_LOAD, Q_CHG, Q_DIS, Q_IMP, Q_EXP, Q_RCHG, Q_RDIS, NQ };
#define NFILL (Q_IMP + 1)       /* quantities a counter can fill */
#define NCTR  (Q_EXP + 1)       /* counters kept (Q_EXP slot = register 35199, AC output) */

/* one day's totals */
typedef struct {
    int date;                                   /* YYYYMMDD, local time */
    float pv, load, chg, dis, imp;              /* day totals, kWh (= measured + fill for v2 rows) */
    int soc_min, soc_max, peak_pv, peak_pv_t, peak_load, peak_load_t;  /* t = minutes after midnight */
    int v2;                                     /* 1: the fields below are valid */
    double m[NQ];                               /* kWh summed from live readings */
    double f[NFILL];                            /* kWh taken from the inverter's counters */
    double c[NCTR];                             /* inverter's daily counters at the day's last reading, -1 = none */
    double meas_s, gap_s;                       /* seconds integrated / seconds of gaps filled from counters */
    int first_min;                              /* minute of the day's first reading, -1 = none */
} day_t;

#define MAX_DAYS 4000
static day_t days[MAX_DAYS];
static int ndays;

/* last reading, for the trapezoid (persisted in state.csv) */
static struct { int valid, date; long long ts; double p[NQ]; } last;

static void powers(const reading_t *n, double *p) {
    double b = n->bat_vi_w;
    p[Q_PV] = n->ppv > 0 ? n->ppv : 0;
    p[Q_LOAD] = n->house_w;
    p[Q_CHG] = b < 0 ? -b : 0;
    p[Q_DIS] = b > 0 ? b : 0;
    p[Q_IMP] = n->grid_w < 0 ? -n->grid_w : 0;
    p[Q_EXP] = n->grid_w > 0 ? n->grid_w : 0;
    p[Q_RCHG] = n->bat_w < 0 ? -n->bat_w : 0;
    p[Q_RDIS] = n->bat_w > 0 ? n->bat_w : 0;
}

static void counters(const reading_t *n, double *c) {
    c[Q_PV] = n->e_day; c[Q_LOAD] = n->e_load; c[Q_CHG] = n->e_chg; c[Q_DIS] = n->e_dis; c[Q_IMP] = n->e_imp;
    c[Q_EXP] = n->e_exp;         /* 35199: not grid export here (see above); reference only */
}

/* energy a counter recorded between two readings */
static double ctr_delta(double prev, double now) {
    if (now < 0) return 0;
    if (prev < 0 || now < prev - 0.05) return now;   /* no earlier value today, or reset in between */
    return now > prev ? now - prev : 0;
}

static void day_v2(day_t *d) {                       /* start the breakdown (new day or an old-format row) */
    d->v2 = 1;
    memset(d->m, 0, sizeof d->m); memset(d->f, 0, sizeof d->f);
    for (int q = 0; q < NCTR; q++) d->c[q] = -1;
    d->meas_s = d->gap_s = 0; d->first_min = -1;
}

static double day_total(const day_t *d, int q) { return d->m[q] + (q < NFILL ? d->f[q] : 0); }
static double day_loss(const day_t *d) {
    return day_total(d, Q_PV) + day_total(d, Q_DIS) + day_total(d, Q_IMP)
         - day_total(d, Q_LOAD) - day_total(d, Q_CHG) - day_total(d, Q_EXP);
}
static double day_fill(const day_t *d) { double s = 0; for (int q = 0; q < NFILL; q++) s += d->f[q]; return s; }

static void day_totals(day_t *d) {
    d->pv = (float)day_total(d, Q_PV); d->load = (float)day_total(d, Q_LOAD);
    d->chg = (float)day_total(d, Q_CHG); d->dis = (float)day_total(d, Q_DIS); d->imp = (float)day_total(d, Q_IMP);
}

/* ---------------------------------------------------------------- modbus */

static int inv_fd = -1;
static unsigned short tid;

static void inv_close(void) { if (inv_fd >= 0) close(inv_fd); inv_fd = -1; }

static int inv_connect(void) {
    struct sockaddr_in a; struct timeval to = { 3, 0 }; int one = 1;
    inv_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (inv_fd < 0) return -1;
    setsockopt(inv_fd, SOL_SOCKET, SO_RCVTIMEO, &to, sizeof to);
    setsockopt(inv_fd, SOL_SOCKET, SO_SNDTIMEO, &to, sizeof to);
    setsockopt(inv_fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET; a.sin_port = htons((unsigned short)inv_port);
    inet_pton(AF_INET, inv_host, &a.sin_addr);
    if (connect(inv_fd, (struct sockaddr *)&a, sizeof a) < 0) { inv_close(); return -1; }
    return 0;
}

static int read_full(int fd, unsigned char *p, int n) {
    int got = 0;
    while (got < n) {
        ssize_t r = read(fd, p + got, (size_t)(n - got));
        if (r <= 0) { if (r < 0 && errno == EINTR) continue; return -1; }
        got += (int)r;
    }
    return 0;
}

/* Read `count` holding registers from `addr` into regs[]. 0 on success. */
static int mb_read(int addr, int count, unsigned short *regs) {
    unsigned char q[12], h[9], d[256];
    if (inv_fd < 0 && inv_connect() < 0) { snprintf(last_err, sizeof last_err, "connect %s:%d failed", inv_host, inv_port); return -1; }
    tid++;
    q[0] = tid >> 8; q[1] = tid & 0xff; q[2] = 0; q[3] = 0; q[4] = 0; q[5] = 6;
    q[6] = (unsigned char)inv_unit; q[7] = 0x03;  /* function 3 = read holding registers, nothing else */
    q[8] = addr >> 8; q[9] = addr & 0xff; q[10] = count >> 8; q[11] = count & 0xff;
    if (write(inv_fd, q, sizeof q) != (ssize_t)sizeof q) goto fail;
    for (;;) {                                  /* skip any stale replies */
        if (read_full(inv_fd, h, 9) < 0) goto fail;
        int len = (h[4] << 8) | h[5];
        if (len < 3 || len > 255) goto fail;
        if (h[7] & 0x80) { snprintf(last_err, sizeof last_err, "modbus exception %d @%d", h[8], addr); inv_close(); return -1; }
        int bc = h[8];
        if (bc != len - 3 || bc > (int)sizeof d) goto fail;
        if (read_full(inv_fd, d, bc) < 0) goto fail;
        if (((h[0] << 8) | h[1]) != tid) continue;
        if (bc != count * 2) goto fail;
        for (int i = 0; i < count; i++) regs[i] = (unsigned short)((d[2 * i] << 8) | d[2 * i + 1]);
        return 0;
    }
fail:
    snprintf(last_err, sizeof last_err, "read @%d failed", addr);
    inv_close();
    return -1;
}

#define U16(b, a)  ((unsigned)(b)[(a) - base])
#define S16(b, a)  ((int)(short)(b)[(a) - base])
#define U32(b, a)  (((unsigned)(b)[(a) - base] << 16) | (b)[(a) - base + 1])
#define S32(b, a)  ((int)U32(b, a))

/* scaling as in goodwe/sensor.py */
static double volt10(unsigned raw) { return raw == 0xffff ? 0 : raw / 10.0; }     /* read_voltage / read_current */
static double temp10(int raw) { return raw == -1 || raw == 32767 ? NA : raw / 10.0; }   /* read_temp */

/* ---------------------------------------------------------------- labels (goodwe/const.py) */

static const char *BAT_MODES[] = { "No battery", "Standby", "Discharging", "Charging", "To be charged", "To be discharged" };
static const char *WORK_MODES[] = { "Wait Mode", "Normal (On-Grid)", "Normal (Off-Grid)", "Fault Mode", "Flash Mode", "Check Mode" };
static const char *GRID_MODES[] = { "Not connected to grid", "Connected to grid", "Fault" };
static const char *PV_MODES[] = { "PV panels not connected", "PV panels connected, no power", "PV panels connected, producing power" };
/* bitmaps: NULL = no label (shown as errN, like the library), "" = deliberately unnamed (skipped) */
static const char *ERROR_CODES[32] = {
    [31] = "Internal Communication Failure", [30] = "EEPROM R/W Failure", [29] = "Fac Failure",
    [28] = "DSP communication failure", [27] = "PhaseAngleFailure", [26] = "", [25] = "Relay Check Failure",
    [24] = "", [23] = "Vac Consistency Failure", [22] = "Fac Consistency Failure", [21] = "",
    [20] = "Back-Up Over Load", [19] = "DC Injection High", [18] = "Isolation Failure", [17] = "Vac Failure",
    [16] = "External Fan Failure", [15] = "PV Over Voltage", [14] = "Utility Phase Failure",
    [13] = "Over Temperature", [12] = "InternalFan Failure", [11] = "DC Bus High", [10] = "Ground I Failure",
    [9] = "Utility Loss", [8] = "AC HCT Failure", [7] = "Relay Device Failure", [6] = "GFCI Device Failure",
    [5] = "", [4] = "GFCI Consistency Failure", [3] = "DCI Consistency Failure", [2] = "",
    [1] = "AC HCT Check Failure", [0] = "GFCI Device Check Failure" };
static const char *DIAG_CODES[32] = {
    [0] = "Battery voltage low", [1] = "Battery SOC low", [2] = "Battery SOC in back", [3] = "BMS: Discharge disabled",
    [4] = "Discharge time on", [5] = "Charge time on", [6] = "Discharge Driver On", [7] = "BMS: Discharge current low",
    [8] = "APP: Discharge current too low", [9] = "Meter communication failure", [10] = "Meter connection reversed",
    [11] = "Self-use load light", [12] = "EMS: discharge current is zero", [13] = "Discharge BUS high PV voltage",
    [14] = "Battery Disconnected", [15] = "Battery Overcharged", [16] = "BMS: Temperature too high",
    [17] = "BMS: Charge too high", [18] = "BMS: Charge disabled", [19] = "Self-use off",
    [20] = "SOC delta too volatile", [21] = "Battery self discharge too high", [22] = "Battery SOC low (off-grid)",
    [23] = "Grid wave unstable", [24] = "Export power limit set", [25] = "PF value set",
    [26] = "Real power limit set", [27] = "DC output on", [28] = "SOC protect off", [30] = "BMS: Emergency charging" };
static const char *BMS_ALARMS[32] = {
    [15] = "Charging over-voltage 3", [14] = "Discharging under-voltage 3", [13] = "Cell temperature high 3",
    [12] = "Communication failure 2", [11] = "Charging circuit failure", [10] = "Discharging circuit failure",
    [9] = "Battery lock", [8] = "Battery break", [7] = "DC bus fault", [6] = "Precharge fault",
    [5] = "Discharging over-current 2", [4] = "Charging over-current 2", [3] = "Cell temperature low 2",
    [2] = "Cell temperature high 2", [1] = "Discharging under-voltage 2", [0] = "Charging over-voltage 2" };
static const char *BMS_WARNINGS[32] = {
    [11] = "System temperature high", [10] = "System temperature low 2", [9] = "System temperature low 1",
    [8] = "Cell imbalance", [7] = "System reboot", [6] = "Communication failure 1",
    [5] = "Discharging over-current 1", [4] = "Charging over-current 1", [3] = "Cell temperature low 1",
    [2] = "Cell temperature high 1", [1] = "Discharging under-voltage 1", [0] = "Charging over-voltage 1" };

static const char *label(const char *const *t, int n, int v) { return v >= 0 && v < n ? t[v] : "Unknown"; }
#define LABEL(t, v) label(t, (int)(sizeof t / sizeof t[0]), v)

/* goodwe's decode_bitmap(): the names of the set bits, comma separated */
static void bits_text(char *out, size_t cap, unsigned v, const char *const *lab) {
    size_t len = 0; out[0] = 0;
    for (int i = 0; i < 32 && len < cap; i++) {
        if (!((v >> i) & 1)) continue;
        char tmp[12]; const char *s = lab[i];
        if (!s) { snprintf(tmp, sizeof tmp, "err%d", i); s = tmp; }
        if (!*s) continue;
        len += (size_t)snprintf(out + len, cap - len, "%s%s", len ? ", " : "", s);
    }
}

/* ---------------------------------------------------------------- json helpers */

typedef struct { char *p; size_t len, cap; } sb_t;
static void sb_init(sb_t *b, size_t cap) { b->p = malloc(cap); b->cap = b->p ? cap : 0; b->len = 0; if (b->p) b->p[0] = 0; }
static void sb_printf(sb_t *b, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void sb_printf(sb_t *b, const char *fmt, ...) {
    if (!b->p) return;
    for (;;) {
        va_list ap; va_start(ap, fmt);
        int n = vsnprintf(b->p + b->len, b->cap - b->len, fmt, ap);
        va_end(ap);
        if (n < 0) return;
        if (b->len + (size_t)n < b->cap) { b->len += (size_t)n; return; }
        size_t cap = (b->cap + (size_t)n + 1) * 2;
        char *q = realloc(b->p, cap);
        if (!q) return;
        b->p = q; b->cap = cap;
    }
}
/* a number, or null for "not reported"; up to 24 per printf call */
static const char *jf(double v, int dec) {
    static char buf[24][32]; static unsigned k;
    if (v <= NA) return "null";
    char *b = buf[k++ % 24];
    snprintf(b, sizeof buf[0], "%.*f", dec, v);
    return b;
}

/* ---------------------------------------------------------------- history */

/* one minute, older format: data/YYYY-MM-DD.bin (still read) */
typedef struct { unsigned int t; short pv, load, bat, grid; unsigned char soc, pad[3]; } rec_t;

/* one minute, v2: data/YYYY-MM-DD.v2.bin -- fixed 48 bytes, host (little-endian) order,
   each field the average of the minute's readings */
typedef struct {
    unsigned short t;                    /* minute of the day */
    short pv, load, bat, grid;           /* W: solar, house (AC delivered), battery V x I (+ discharge), grid (+ export) */
    short bat_rep;                       /* W: the inverter's own battery power figure */
    short ppv1, ppv2;                    /* W */
    unsigned short vpv1, vpv2;           /* 0.1 V */
    unsigned short ipv1, ipv2;           /* 0.01 A */
    unsigned short vbat;                 /* 0.01 V */
    short ibat;                          /* 0.01 A, + discharging */
    short tbat, tinv, tair;              /* 0.1 C (battery, inverter radiator, inverter air); NO_T = not reported */
    unsigned short vout, iout, fout;     /* back-up (house) output: 0.1 V, 0.01 A, 0.01 Hz */
    unsigned short vgrid, fgrid;         /* 0.1 V, 0.01 Hz */
    unsigned short vbus;                 /* 0.1 V */
    unsigned char soc, n;                /* %, readings averaged (0 = converted from the old format) */
} rec2_t;
#define NO_T (-32768)
_Static_assert(sizeof(rec2_t) == 48, "rec2_t is an on-disk format: keep it 48 bytes");

#define MAX_REC 3000                     /* a day's records before de-duplication (old + v2 files) */
static rec2_t today_recs[MAX_REC];
static int ntoday, today_date;
static long long last_days_save;
static char data_dir[600];

/* minute being accumulated */
static struct {
    int min, n, soc, ntbat, ntinv, ntair;
    double pv, load, bat, grid, bat_rep, ppv1, ppv2, vpv1, vpv2, ipv1, ipv2, vbat, ibat,
           vout, iout, fout, vgrid, fgrid, vbus, tbat, tinv, tair;
} acc = { .min = -1 };

static void local_parts(long long ms, int *date, int *minute) {
    time_t t = (time_t)(ms / 1000);
    struct tm tm;
    if (utc_off_min != -9999) { t += utc_off_min * 60; gmtime_r(&t, &tm); }   /* utc_offset setting */
    else localtime_r(&t, &tm);                                               /* the system's time zone */
    *date = (tm.tm_year + 1900) * 10000 + (tm.tm_mon + 1) * 100 + tm.tm_mday;
    *minute = tm.tm_hour * 60 + tm.tm_min;
}

static void day_file(char *out, size_t cap, int date, int ver) {
    snprintf(out, cap, "%s/%04d-%02d-%02d%s", data_dir, date / 10000, date / 100 % 100, date % 100,
             ver == 2 ? ".v2.bin" : ".bin");
}

static day_t *day_get(int date, int create) {
    for (int i = ndays - 1; i >= 0; i--) {
        if (days[i].date == date) return &days[i];
        if (days[i].date < date) break;
    }
    if (!create) return NULL;
    if (ndays == MAX_DAYS) { memmove(days, days + 1, sizeof(day_t) * (MAX_DAYS - 1)); ndays--; }
    int i = ndays;
    while (i > 0 && days[i - 1].date > date) { days[i] = days[i - 1]; i--; }   /* keep sorted */
    memset(&days[i], 0, sizeof(day_t));
    days[i].date = date; days[i].soc_min = 101; days[i].soc_max = -1; days[i].first_min = -1;
    ndays++;
    return &days[i];
}

/* replace a file atomically (Windows' rename won't overwrite: retry after removing) */
static int rename_file(const char *from, const char *to) {
    if (rename(from, to) == 0) return 0;
    remove(to);
    return rename(from, to);
}

static int parse_nums(const char *s, double *v, int max) {
    int k = 0;
    while (k < max) {
        char *e; v[k] = strtod(s, &e);
        if (e == s) break;
        k++;
        if (*e != ',') break;
        s = e + 1;
    }
    return k;
}

#define ROW_COLS (13 + NQ + NFILL + NCTR + 3)
static const char DAYS_HEADER[] =
    "date,pv,load,charge,discharge,import,soc_min,soc_max,peak_pv,peak_pv_min,peak_load,peak_load_min,"
    "v2,m_pv,m_load,m_charge,m_discharge,m_import,m_export,m_charge_reported,m_discharge_reported,"
    "f_pv,f_load,f_charge,f_discharge,f_import,"
    "counter_pv,counter_load,counter_charge,counter_discharge,counter_import,counter_35199,"
    "meas_s,gap_s,first_min\n";

/* one days.csv line. The first 12 columns keep their old meaning (totals), so
   old rows load in this version and new rows load in the old one. */
static void format_row(char *b, size_t cap, const day_t *d) {
    size_t n = (size_t)snprintf(b, cap, "%d,%.2f,%.2f,%.2f,%.2f,%.2f,%d,%d,%d,%d,%d,%d", d->date, d->pv, d->load,
                                d->chg, d->dis, d->imp, d->soc_min, d->soc_max, d->peak_pv, d->peak_pv_t,
                                d->peak_load, d->peak_load_t);
    if (d->v2) {
        n += (size_t)snprintf(b + n, cap - n, ",1");
        for (int q = 0; q < NQ; q++) n += (size_t)snprintf(b + n, cap - n, ",%.4f", d->m[q]);
        for (int q = 0; q < NFILL; q++) n += (size_t)snprintf(b + n, cap - n, ",%.4f", d->f[q]);
        for (int q = 0; q < NCTR; q++) n += (size_t)snprintf(b + n, cap - n, ",%.1f", d->c[q]);
        n += (size_t)snprintf(b + n, cap - n, ",%.0f,%.0f,%d", d->meas_s, d->gap_s, d->first_min);
    }
    snprintf(b + n, cap - n, "\n");
}

static int parse_row(const char *line, day_t *d) {
    double v[ROW_COLS + 8];
    int k = parse_nums(line, v, ROW_COLS + 8);
    if (k < 12 || v[0] < 19000101) return 0;             /* header or junk */
    memset(d, 0, sizeof *d);
    d->date = (int)v[0]; d->pv = (float)v[1]; d->load = (float)v[2]; d->chg = (float)v[3];
    d->dis = (float)v[4]; d->imp = (float)v[5]; d->soc_min = (int)v[6]; d->soc_max = (int)v[7];
    d->peak_pv = (int)v[8]; d->peak_pv_t = (int)v[9]; d->peak_load = (int)v[10]; d->peak_load_t = (int)v[11];
    d->first_min = -1;
    if (k >= ROW_COLS && v[12] == 1) {
        int i = 13;
        d->v2 = 1;
        for (int q = 0; q < NQ; q++) d->m[q] = v[i++];
        for (int q = 0; q < NFILL; q++) d->f[q] = v[i++];
        for (int q = 0; q < NCTR; q++) d->c[q] = v[i++];
        d->meas_s = v[i++]; d->gap_s = v[i++]; d->first_min = (int)v[i++];
    }
    return 1;
}

static void save_days(void) {
    char path[700], tmp[710], row[768];
    snprintf(path, sizeof path, "%s/days.csv", data_dir);
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "w");
    if (!f) return;
    fputs(DAYS_HEADER, f);
    for (int i = 0; i < ndays; i++) { format_row(row, sizeof row, &days[i]); fputs(row, f); }
    if (fclose(f) == 0) rename_file(tmp, path);
}

static void load_days(void) {
    char path[700], line[1024];
    snprintf(path, sizeof path, "%s/days.csv", data_dir);
    FILE *f = fopen(path, "r");
    if (!f) return;
    while (fgets(line, sizeof line, f)) {
        day_t d;
        if (parse_row(line, &d)) { day_t *x = day_get(d.date, 1); *x = d; }
    }
    fclose(f);
}

/* state.csv: "last,<date>,<ts>,<powers...>" + that day's row. Written every
   minute: the pair is always consistent, so a restart picks up exactly there. */
static void save_state(void) {
    if (!last.valid) return;
    day_t *d = day_get(last.date, 0);
    if (!d) return;
    char path[700], tmp[710], row[768];
    snprintf(path, sizeof path, "%s/state.csv", data_dir);
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "w");
    if (!f) return;
    fprintf(f, "last,%d,%lld", last.date, last.ts);
    for (int q = 0; q < NQ; q++) fprintf(f, ",%.1f", last.p[q]);
    format_row(row, sizeof row, d);
    fprintf(f, "\n%s", row);
    if (fclose(f) == 0) rename_file(tmp, path);
}

static void load_state(void) {
    char path[700], line[1024]; double v[NQ + 4]; day_t d;
    snprintf(path, sizeof path, "%s/state.csv", data_dir);
    FILE *f = fopen(path, "r");
    if (!f) return;
    if (fgets(line, sizeof line, f) && !strncmp(line, "last,", 5) && parse_nums(line + 5, v, NQ + 4) == NQ + 2 &&
        fgets(line, sizeof line, f) && parse_row(line, &d) && d.v2 && d.date == (int)v[0]) {
        *day_get(d.date, 1) = d;                /* newer than (or equal to) its days.csv row */
        last.date = (int)v[0]; last.ts = (long long)v[1];
        for (int q = 0; q < NQ; q++) last.p[q] = v[2 + q];
        last.valid = 1;
    }
    fclose(f);
}

static int cmp_rec(const void *a, const void *b) {
    const rec2_t *x = a, *y = b;
    if (x->t != y->t) return (int)x->t - (int)y->t;
    return (x->n > 0) - (y->n > 0);              /* a detailed record sorts after an old one */
}

/* A day's minutes from both formats, sorted, one per minute (the detailed one wins). */
static int load_recs(int date, rec2_t *out, int max) {
    char path[700]; int n = 0; FILE *f;
    day_file(path, sizeof path, date, 1);
    if ((f = fopen(path, "rb"))) {
        rec_t r;
        while (n < max && fread(&r, sizeof r, 1, f) == 1) {
            if (r.t >= 1440) continue;
            rec2_t *o = &out[n++]; memset(o, 0, sizeof *o);
            o->t = (unsigned short)r.t; o->pv = r.pv; o->load = r.load; o->bat = r.bat; o->grid = r.grid;
            o->bat_rep = r.bat; o->soc = r.soc; o->tbat = o->tinv = o->tair = NO_T;
        }
        fclose(f);
    }
    day_file(path, sizeof path, date, 2);
    if ((f = fopen(path, "rb"))) {
        rec2_t r;
        while (n < max && fread(&r, sizeof r, 1, f) == 1) if (r.t < 1440) out[n++] = r;
        fclose(f);
    }
    qsort(out, (size_t)n, sizeof *out, cmp_rec);
    int k = 0;
    for (int i = 0; i < n; i++) {
        if (k && out[k - 1].t == out[i].t) out[k - 1] = out[i];
        else out[k++] = out[i];
    }
    return k;
}

static void load_today(int date) {
    today_date = date;
    ntoday = load_recs(date, today_recs, MAX_REC);
}

static short s16r(double v) { v += v >= 0 ? .5 : -.5; return (short)(v > 32767 ? 32767 : v < -32767 ? -32767 : v); }
static unsigned short u16r(double v) { v += .5; return (unsigned short)(v < 0 ? 0 : v > 65535 ? 65535 : v); }

static void flush_minute(void) {
    if (acc.n == 0 || acc.min < 0) return;
    double k = 1.0 / acc.n;
    rec2_t r; memset(&r, 0, sizeof r);
    r.t = (unsigned short)acc.min;
    r.pv = s16r(acc.pv * k); r.load = s16r(acc.load * k); r.bat = s16r(acc.bat * k); r.grid = s16r(acc.grid * k);
    r.bat_rep = s16r(acc.bat_rep * k); r.ppv1 = s16r(acc.ppv1 * k); r.ppv2 = s16r(acc.ppv2 * k);
    r.vpv1 = u16r(acc.vpv1 * k * 10); r.vpv2 = u16r(acc.vpv2 * k * 10);
    r.ipv1 = u16r(acc.ipv1 * k * 100); r.ipv2 = u16r(acc.ipv2 * k * 100);
    r.vbat = u16r(acc.vbat * k * 100); r.ibat = s16r(acc.ibat * k * 100);
    r.tbat = acc.ntbat ? s16r(acc.tbat / acc.ntbat * 10) : NO_T;
    r.tinv = acc.ntinv ? s16r(acc.tinv / acc.ntinv * 10) : NO_T;
    r.tair = acc.ntair ? s16r(acc.tair / acc.ntair * 10) : NO_T;
    r.vout = u16r(acc.vout * k * 10); r.iout = u16r(acc.iout * k * 100); r.fout = u16r(acc.fout * k * 100);
    r.vgrid = u16r(acc.vgrid * k * 10); r.fgrid = u16r(acc.fgrid * k * 100); r.vbus = u16r(acc.vbus * k * 10);
    r.soc = (unsigned char)acc.soc; r.n = (unsigned char)(acc.n > 255 ? 255 : acc.n);
    if (ntoday < MAX_REC) today_recs[ntoday++] = r;
    char path[700]; day_file(path, sizeof path, today_date, 2);
    FILE *f = fopen(path, "ab");
    if (f) { fwrite(&r, sizeof r, 1, f); fclose(f); }
    memset(&acc, 0, sizeof acc); acc.min = -1;
}

static void accumulate(const reading_t *n) {
    acc.pv += n->ppv; acc.load += n->house_w; acc.bat += n->bat_vi_w; acc.grid += n->grid_w; acc.bat_rep += n->bat_w;
    acc.ppv1 += n->ppv1; acc.ppv2 += n->ppv2; acc.vpv1 += n->vpv1; acc.vpv2 += n->vpv2; acc.ipv1 += n->ipv1; acc.ipv2 += n->ipv2;
    acc.vbat += n->vbat; acc.ibat += n->ibat; acc.vout += n->vout; acc.iout += n->iout; acc.fout += n->fout;
    acc.vgrid += n->vgrid; acc.fgrid += n->fgrid; acc.vbus += n->vbus;
    if (n->bat_temp > NA) { acc.tbat += n->bat_temp; acc.ntbat++; }
    if (n->inv_temp > NA) { acc.tinv += n->inv_temp; acc.ntinv++; }
    if (n->t_air > NA) { acc.tair += n->t_air; acc.ntair++; }
    acc.soc = n->soc; acc.n++;
}

/* The energy bookkeeping for one reading -- see "energy" at the top for the rule. */
static void integrate(const reading_t *n, int date, int minute) {
    double p[NQ], c[NCTR], yc[NCTR]; int q, yday, ymin;
    powers(n, p); counters(n, c);
    local_parts(n->ts - 86400000LL, &yday, &ymin);
    day_t *d = day_get(date, 1);
    if (!d->v2) day_v2(d);                           /* a new day, or a row from the old version */
    double dt = last.valid ? (n->ts - last.ts) / 1000.0 : -1;

    if (last.valid && (last.date == date || last.date == yday) && dt > 0 && dt <= GAP_S) {
        for (q = 0; q < NQ; q++) d->m[q] += (last.p[q] + p[q]) / 2 * dt / 3600000.0;   /* W*s -> kWh */
        d->meas_s += dt;
    } else {
        const double *ref = d->c;                    /* counters at this day's previous reading, -1 = none */
        if (d->c[Q_LOAD] < 0) {                      /* the day's first reading: baseline, unless... */
            day_t *y = day_get(yday, 0);
            if (y && minute < 60) {                  /* ...the inverter hasn't started its new day yet */
                for (q = 0; q < NCTR; q++) yc[q] = y->v2 ? y->c[q] : -1;
                if (!y->v2) { yc[Q_PV] = y->pv; yc[Q_LOAD] = y->load; yc[Q_CHG] = y->chg; yc[Q_DIS] = y->dis; yc[Q_IMP] = y->imp; }
                if (yc[Q_LOAD] >= 0 && c[Q_LOAD] > 0.2 && c[Q_LOAD] >= yc[Q_LOAD] - 0.05) ref = yc;
            }
        } else if (dt > 0) d->gap_s += dt;
        for (q = 0; q < NFILL; q++) d->f[q] += ctr_delta(ref[q], c[q]);
    }
    if (d->first_min < 0) d->first_min = minute;
    for (q = 0; q < NCTR; q++) d->c[q] = c[q];
    day_totals(d);
    last.valid = 1; last.date = date; last.ts = n->ts;
    memcpy(last.p, p, sizeof p);
}

/* Called for every successful poll: builds the 1-minute curve and the day totals. */
static void record(const reading_t *n) {
    int date, minute;
    local_parts(n->ts, &date, &minute);

    if (date != today_date) {                    /* new day (or first poll after start) */
        int first = today_date == 0;
        flush_minute();
        if (!first) save_days();
        load_today(date);                        /* resumes today's curve after a restart */
        if (first && ntoday > 0) {               /* seed the 5-min average from recent history */
            int k = ntoday < 5 ? ntoday : 5; double sum = 0;
            for (int i = ntoday - k; i < ntoday; i++) sum += today_recs[i].bat;
            bat_ema = (sum + n->bat_w) / (k + 1);
        }
    }
    if (minute != acc.min) { flush_minute(); acc.min = minute; save_state(); }
    accumulate(n);
    integrate(n, date, minute);

    day_t *d = day_get(date, 1);
    if (n->soc < d->soc_min) d->soc_min = n->soc;
    if (n->soc > d->soc_max) d->soc_max = n->soc;
    if (n->ppv > d->peak_pv) { d->peak_pv = n->ppv; d->peak_pv_t = minute; }
    if (n->house_w > d->peak_load) { d->peak_load = n->house_w; d->peak_load_t = minute; }

    if (n->ts - last_days_save > 300000) { save_days(); last_days_save = n->ts; }
}

static int parse_date(const char *q, const char *key, int def) {
    const char *p = q ? strstr(q, key) : NULL;
    if (!p) return def;
    p += strlen(key);
    int y, m, d;
    if (sscanf(p, "%4d-%2d-%2d", &y, &m, &d) == 3) return y * 10000 + m * 100 + d;
    if (sscanf(p, "%8d", &y) == 1) return y;
    return def;
}

static int poll_inverter(void) {
    unsigned short r[125], m[24];
    int base = 35100;
    if (mb_read(35100, 125, r) < 0) return -1;
    if (mb_read(37000, 24, m) < 0) return -1;

    reading_t n; memset(&n, 0, sizeof n);
    n.ok = 1; n.ts = now_ms();
    n.vpv1 = volt10(U16(r, 35103)); n.ipv1 = volt10(U16(r, 35104)); n.ppv1 = (int)U32(r, 35105);
    n.vpv2 = volt10(U16(r, 35107)); n.ipv2 = volt10(U16(r, 35108)); n.ppv2 = (int)U32(r, 35109);
    n.ppv = (n.ppv1 > 0 ? n.ppv1 : 0) + (n.ppv2 > 0 ? n.ppv2 : 0);
    n.pv1_mode = U16(r, 35120) & 0xff; n.pv2_mode = U16(r, 35120) >> 8;
    n.vgrid = volt10(U16(r, 35121)); n.igrid = volt10(U16(r, 35122)); n.fgrid = S16(r, 35123) / 100.0;
    n.pgrid = S16(r, 35125);
    n.grid_mode = U16(r, 35136);
    n.inv_total_w = S16(r, 35138);
    n.grid_w = S16(r, 35140);
    n.vout = volt10(U16(r, 35145)); n.iout = volt10(U16(r, 35146)); n.fout = S16(r, 35147) / 100.0;
    n.pout = S16(r, 35150);
    n.backup_w = S16(r, 35170); n.load_w = S16(r, 35172); n.ups_load = U16(r, 35173);
    n.t_air = temp10(S16(r, 35174)); n.t_module = temp10(S16(r, 35175)); n.inv_temp = temp10(S16(r, 35176));
    n.vbus = volt10(U16(r, 35178)); n.vnbus = volt10(U16(r, 35179));
    n.vbat = volt10(U16(r, 35180)); n.ibat = S16(r, 35181) / 10.0; n.bat_w = S32(r, 35182);
    n.bat_mode = U16(r, 35184); n.warning_code = U16(r, 35185); n.work_mode = U16(r, 35187);
    n.error_bits = U32(r, 35189);
    n.e_total = U32(r, 35191) / 10.0; n.e_day = U32(r, 35193) / 10.0; n.e_exp_total = U32(r, 35195) / 10.0;
    n.h_total = (int)U32(r, 35197);
    n.e_exp = U16(r, 35199) / 10.0; n.e_imp_total = U32(r, 35200) / 10.0; n.e_imp = U16(r, 35202) / 10.0;
    n.e_load_total = U32(r, 35203) / 10.0; n.e_load = U16(r, 35205) / 10.0;
    n.e_chg_total = U32(r, 35206) / 10.0; n.e_chg = U16(r, 35208) / 10.0;
    n.e_dis_total = U32(r, 35209) / 10.0; n.e_dis = U16(r, 35211) / 10.0;
    n.diag_bits = U32(r, 35220);
    n.bat_vi_w = n.vbat * n.ibat;
    /* House = everything the inverter outputs (35138) plus what the grid supplies
       (35140: + export, - import). Checked with a 1.1 kW iron: it
       showed up in total_inverter_power and the battery, while backup_ptotal (35170)
       stayed ~240 W -- most of the house hangs on the inverter's grid-side output,
       the back-up port feeds only part of it. */
    n.house_w = n.inv_total_w - n.grid_w;
    if (n.house_w < 0) n.house_w = 0;
    base = 37000;
    n.bms = U16(m, 37000); n.bat_status = U16(m, 37002);
    n.bat_temp = temp10(S16(m, 37003));
    n.chg_lim = U16(m, 37004); n.dis_lim = U16(m, 37005);
    n.soc = U16(m, 37007); n.soh = U16(m, 37008); n.modules = U16(m, 37009);
    n.bat_protocol = U16(m, 37011);
    n.bat_err = (U16(m, 37012) << 16) | U16(m, 37006);
    n.bat_warn = (U16(m, 37013) << 16) | U16(m, 37010);
    n.cell_tmax = U16(m, 37020) ? temp10(S16(m, 37020)) : NA;    /* 0 = not reported by this BMS */
    n.cell_tmin = U16(m, 37021) ? temp10(S16(m, 37021)) : NA;
    n.cell_vmax = U16(m, 37022) ? U16(m, 37022) / 1000.0 : NA;
    n.cell_vmin = U16(m, 37023) ? U16(m, 37023) / 1000.0 : NA;
    cur = n;

    /* Battery power averaged over ~5 minutes for the time-left estimates:
       smooth enough that a fridge compressor or kettle switching on doesn't
       swing "battery will last" by hours, quick enough to follow real changes. */
    double a = (double)POLL_MS / 300000.0;
    if (!ema_init) { bat_ema = n.bat_w; ema_init = 1; }
    else bat_ema += a * (n.bat_w - bat_ema);

    record(&n);
    last_err[0] = 0;
    return 0;
}

static void poll_settings(void) {
    unsigned short v[3];
    if (mb_read(45356, 3, v) == 0) {        /* 45356 reserve on-grid, 45358 off-grid */
        if (v[0] <= 100) reserve_on = v[0];
        if (v[2] <= 100) reserve_off = v[2];
    }
    if (mb_read(47760, 1, v) == 0 && v[0] >= 10 && v[0] <= 100) charge_limit = v[0];   /* SoC upper limit */
}

/* ---------------------------------------------------------------- discovery
 *
 * 1. GoodWe's WiFi-kit discovery: "WIFIKIT-214028-READ" broadcast to UDP 48899;
 *    dongles answer "ip,mac,serial" (as the goodwe library does).
 * 2. Otherwise a scan of this machine's /24 for an open Modbus/TCP port 502.
 * Every candidate must answer a read-only Modbus read of the runtime and BMS
 * blocks (the hybrid register map) before it is used.
 */
#define MAX_CAND 32

static int valid_ip(const char *s) { unsigned a, b, c, d; char x; return sscanf(s, "%u.%u.%u.%u%c", &a, &b, &c, &d, &x) == 4 && a < 256 && b < 256 && c < 256 && d < 256; }

static int probe(const char *ip) {
    const char *save = inv_host; unsigned short r[10];
    int ok = 0;
    for (int attempt = 0; attempt < 2 && !ok; attempt++) {     /* the dongle can be busy serving another reader */
        if (attempt) usleep(800000);
        inv_close(); inv_host = ip;
        ok = mb_read(35100, 10, r) == 0 && mb_read(37000, 10, r) == 0;
    }
    inv_close(); inv_host = save;
    return ok;
}

static int add_cand(char c[][16], int n, const char *ip) {
    for (int i = 0; i < n; i++) if (!strcmp(c[i], ip)) return n;
    if (n < MAX_CAND && valid_ip(ip)) snprintf(c[n++], 16, "%s", ip);
    return n;
}

static int wifikit_broadcast(char c[][16], int n, char serial[][40]) {
    int s = socket(AF_INET, SOCK_DGRAM, 0), one = 1;
    if (s < 0) return n;
    setsockopt(s, SOL_SOCKET, SO_BROADCAST, &one, sizeof one);
    struct sockaddr_in a; memset(&a, 0, sizeof a);
    a.sin_family = AF_INET; a.sin_port = htons(48899); a.sin_addr.s_addr = htonl(INADDR_BROADCAST);
    const char *msg = "WIFIKIT-214028-READ";
    for (int round = 0; round < 3; round++) {
        sendto(s, msg, strlen(msg), 0, (struct sockaddr *)&a, sizeof a);
        long long end = now_ms() + 1000;
        for (;;) {
            int wait = (int)(end - now_ms()); if (wait <= 0) break;
            struct pollfd pf = { s, POLLIN, 0 };
            if (poll(&pf, 1, wait) <= 0) break;
            char b[256]; struct sockaddr_in from; socklen_t fl = sizeof from;
            ssize_t r = recvfrom(s, b, sizeof b - 1, 0, (struct sockaddr *)&from, &fl);
            if (r <= 0) continue;
            b[r] = 0;
            char ip[16] = ""; sscanf(b, "%15[0-9.]", ip);            /* "ip,mac,serial" */
            if (!valid_ip(ip)) inet_ntop(AF_INET, &from.sin_addr, ip, sizeof ip);
            int before = n; n = add_cand(c, n, ip);
            if (n > before) { const char *p = strrchr(b, ','); snprintf(serial[n - 1], 40, "%s", p ? p + 1 : ""); }
        }
    }
    close(s);
    return n;
}

/* this machine's LAN address: the source address a UDP "connect" would use (no packet is sent) */
static int local_ipv4(struct in_addr *out) {
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) return -1;
    struct sockaddr_in a; memset(&a, 0, sizeof a);
    a.sin_family = AF_INET; a.sin_port = htons(53); inet_pton(AF_INET, "1.1.1.1", &a.sin_addr);
    socklen_t l = sizeof a;
    int ok = connect(s, (struct sockaddr *)&a, sizeof a) == 0 && getsockname(s, (struct sockaddr *)&a, &l) == 0;
    close(s);
    if (!ok) return -1;
    *out = a.sin_addr;
    return 0;
}

/* VPN / overlay ranges (Radmin 26.x, Hamachi 25.x, Tailscale/CGNAT 100.64/10): never a home LAN */
static int vpn_range(unsigned ip) {
    unsigned a = ip >> 24, b = (ip >> 16) & 255;
    return a == 26 || a == 25 || (a == 100 && b >= 64 && b < 128);
}

/* scan one /24 (base = a.b.c.0) for an open Modbus/TCP port */
static int scan_subnet(char c[][16], int n, unsigned base, unsigned self) {
    for (unsigned h0 = 1; h0 < 255; h0 += 64) {                   /* 64 connects at a time */
        int fd[64]; struct pollfd pf[64]; unsigned ip[64]; int k = 0;
        for (unsigned h = h0; h < h0 + 64 && h < 255; h++) {
            if ((base | h) == self) continue;
            int f = socket(AF_INET, SOCK_STREAM, 0);
            if (f < 0) continue;
            fcntl(f, F_SETFL, fcntl(f, F_GETFL, 0) | O_NONBLOCK);
            struct sockaddr_in a; memset(&a, 0, sizeof a);
            a.sin_family = AF_INET; a.sin_port = htons((unsigned short)inv_port); a.sin_addr.s_addr = htonl(base | h);
            if (connect(f, (struct sockaddr *)&a, sizeof a) < 0 && errno != EINPROGRESS && errno != EWOULDBLOCK) { close(f); continue; }
            fd[k] = f; ip[k] = base | h; pf[k].fd = f; pf[k].events = POLLOUT; pf[k].revents = 0; k++;
        }
        long long end = now_ms() + 700;
        int left = k;
        while (left > 0) {
            int wait = (int)(end - now_ms()); if (wait <= 0) break;
            if (poll(pf, (nfds_t)k, wait) <= 0) break;
            for (int i = 0; i < k; i++) {
                if (pf[i].fd < 0 || !pf[i].revents) continue;
                int err = 1; socklen_t el = sizeof err;
                getsockopt(pf[i].fd, SOL_SOCKET, SO_ERROR, &err, &el);
                if (err == 0 && (pf[i].revents & POLLOUT)) {
                    struct in_addr x; x.s_addr = htonl(ip[i]); char t[16]; inet_ntop(AF_INET, &x, t, sizeof t);
                    n = add_cand(c, n, t);
                }
                pf[i].fd = -1; left--;
            }
        }
        for (int i = 0; i < k; i++) close(fd[i]);
    }
    return n;
}

/* Find the inverter: returns 0 and fills out[] with its address, or -1. verbose: print what was found. */
static int discover(char *out, size_t cap, int verbose) {
    char c[MAX_CAND][16], serial[MAX_CAND][40]; int n = 0;
    memset(serial, 0, sizeof serial);
    n = wifikit_broadcast(c, n, serial);
    int from_bc = n;
    for (int pass = 0; pass < 2; pass++) {
        if (pass == 1) {
            /* this machine's own /24 (unless it's a VPN address), then the most common home
               ranges: reachable through a second router, where the broadcast doesn't get */
            struct in_addr me; unsigned self = 0, nets[4]; int k = 0;
            if (local_ipv4(&me) == 0) self = ntohl(me.s_addr);
            if (self && !vpn_range(self)) nets[k++] = self & 0xFFFFFF00u;
            const unsigned common[] = { 0xC0A80100u, 0xC0A80000u, 0x0A000000u };   /* 192.168.1, 192.168.0, 10.0.0 */
            for (int j = 0; j < 3; j++) { int dup = 0; for (int x = 0; x < k; x++) dup |= nets[x] == common[j]; if (!dup) nets[k++] = common[j]; }
            int before = n;
            for (int j = 0; j < k && n == before; j++) n = scan_subnet(c, n, nets[j], self);
        }
        for (int i = pass ? from_bc : 0; i < n; i++) {
            int ok = probe(c[i]);
            if (verbose) printf("  %-15s %s%s%s\n", c[i], ok ? "GoodWe hybrid inverter (Modbus/TCP)" : "no answer to the hybrid registers",
                                serial[i][0] ? "  serial " : "", serial[i]);
            if (ok) { snprintf(out, cap, "%s", c[i]); return 0; }
        }
    }
    return -1;
}

/* ---------------------------------------------------------------- settings */

static void set_opt(const char *k, const char *v) {
    if (!strcmp(k, "host")) snprintf(host_buf, sizeof host_buf, "%s", v);
    else if (!strcmp(k, "port")) inv_port = atoi(v);
    else if (!strcmp(k, "unit")) inv_unit = (int)strtol(v, NULL, 0);
    else if (!strcmp(k, "http")) http_port = atoi(v);
    else if (!strcmp(k, "capacity")) capacity_kwh = atof(v);
    else if (!strcmp(k, "lat")) site_lat = atof(v);
    else if (!strcmp(k, "lon")) site_lon = atof(v);
    else if (!strcmp(k, "data")) snprintf(data_dir, sizeof data_dir, "%s", v);
    else if (!strcmp(k, "www")) snprintf(www_dir, sizeof www_dir, "%s", v);
    else if (!strcmp(k, "utc_offset") || !strcmp(k, "utc-offset")) {          /* +05:00, -0330, 5 */
        int sg = v[0] == '-' ? -1 : 1, h = 0, m = 0; const char *p = v + (v[0] == '+' || v[0] == '-');
        if (strchr(p, ':')) sscanf(p, "%d:%d", &h, &m); else { int x = atoi(p); if (strlen(p) > 2) { h = x / 100; m = x % 100; } else h = x; }
        utc_off_min = sg * (h * 60 + m);
    }
}

static void load_conf(const char *path) {
    FILE *f = fopen(path, "r"); char line[512];
    if (!f) return;
    while (fgets(line, sizeof line, f)) {
        char *p = line; while (*p == ' ' || *p == '\t') p++;
        if (*p == '#' || !*p) continue;
        char *eq = strchr(p, '='); if (!eq) continue;
        *eq = 0; char *k = p, *v = eq + 1;
        for (char *e = eq - 1; e >= k && (*e == ' ' || *e == '\t'); e--) *e = 0;
        while (*v == ' ' || *v == '\t') v++;
        v[strcspn(v, "\r\n")] = 0;
        for (char *e = v + strlen(v) - 1; e >= v && (*e == ' ' || *e == '\t'); e--) *e = 0;
        set_opt(k, v);
    }
    fclose(f);
}

/* remember a discovered inverter address in solard.conf (other lines kept) */
static void save_host(void) {
    char tmp[610], line[512]; snprintf(tmp, sizeof tmp, "%s.tmp", conf_path);
    FILE *in = fopen(conf_path, "r"), *out = fopen(tmp, "w"); int done = 0;
    if (!out) { if (in) fclose(in); return; }
    while (in && fgets(line, sizeof line, in)) {
        char *p = line; while (*p == ' ' || *p == '\t') p++;
        if (!strncmp(p, "host", 4) && strchr(p, '=')) { if (!done) fprintf(out, "host = %s\n", host_buf); done = 1; }
        else fputs(line, out);
    }
    if (!done) fprintf(out, "host = %s\n", host_buf);
    if (in) fclose(in);
    if (fclose(out) == 0) rename_file(tmp, conf_path);
}

/* ---------------------------------------------------------------- http */

static void send_all(int fd, const char *p, size_t n) {
    while (n) { ssize_t w = write(fd, p, n); if (w <= 0) return; p += w; n -= (size_t)w; }
}

static void reply(int fd, int code, const char *ctype, const char *body, size_t len) {
    char h[256];
    int n = snprintf(h, sizeof h, "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\n"
                     "Cache-Control: no-store\r\nAccess-Control-Allow-Origin: *\r\nConnection: close\r\n\r\n",
                     code, code == 200 ? "OK" : "Not Found", ctype, len);
    send_all(fd, h, (size_t)n);
    send_all(fd, body, len);
}

static void reply_sb(int fd, sb_t *b) {
    if (b->p) reply(fd, 200, "application/json", b->p, b->len);
    else reply(fd, 404, "text/plain", "out of memory", 13);
    free(b->p);
}

/* A day's energy as JSON: totals (what the pages show) + where they came from. */
static void day_json(sb_t *b, const day_t *d) {
    if (!d) { sb_printf(b, "null"); return; }
    sb_printf(b, "{\"pv\":%.2f,\"load\":%.2f,\"charge\":%.2f,\"discharge\":%.2f,\"import\":%.2f,"
              "\"soc_min\":%d,\"soc_max\":%d,\"peak_pv\":%d,\"peak_pv_min\":%d,\"peak_load\":%d,\"peak_load_min\":%d",
              d->pv, d->load, d->chg, d->dis, d->imp, d->soc_min > 100 ? -1 : d->soc_min, d->soc_max,
              d->peak_pv, d->peak_pv_t, d->peak_load, d->peak_load_t);
    if (!d->v2) { sb_printf(b, ",\"basis\":\"counters\"}"); return; }   /* rows from the older format */
    sb_printf(b, ",\"export\":%.3f,\"loss\":%.3f,\"basis\":\"measured\","
              "\"meas\":{\"pv\":%.3f,\"load\":%.3f,\"charge\":%.3f,\"discharge\":%.3f,\"import\":%.3f,\"export\":%.3f},"
              "\"fill\":{\"pv\":%.3f,\"load\":%.3f,\"charge\":%.3f,\"discharge\":%.3f,\"import\":%.3f},"
              "\"counter\":{\"pv\":%s,\"load\":%s,\"charge\":%s,\"discharge\":%s,\"import\":%s,\"ac_out_35199\":%s},"
              "\"bat_reported\":{\"charge\":%.3f,\"discharge\":%.3f},"
              "\"fill_kwh\":%.3f,\"meas_min\":%.1f,\"gap_min\":%.1f,\"first_min\":%d}",
              day_total(d, Q_EXP), day_loss(d),
              d->m[Q_PV], d->m[Q_LOAD], d->m[Q_CHG], d->m[Q_DIS], d->m[Q_IMP], d->m[Q_EXP],
              d->f[Q_PV], d->f[Q_LOAD], d->f[Q_CHG], d->f[Q_DIS], d->f[Q_IMP],
              jf(d->c[Q_PV] < 0 ? NA : d->c[Q_PV], 1), jf(d->c[Q_LOAD] < 0 ? NA : d->c[Q_LOAD], 1),
              jf(d->c[Q_CHG] < 0 ? NA : d->c[Q_CHG], 1), jf(d->c[Q_DIS] < 0 ? NA : d->c[Q_DIS], 1),
              jf(d->c[Q_IMP] < 0 ? NA : d->c[Q_IMP], 1), jf(d->c[Q_EXP] < 0 ? NA : d->c[Q_EXP], 1),
              d->m[Q_RCHG], d->m[Q_RDIS], day_fill(d), d->meas_s / 60.0, d->gap_s / 60.0, d->first_min);
}

static void api_now(int fd) {
    const reading_t *r = &cur;
    int reserve = r->grid_mode == 1 ? reserve_on : reserve_off;
    double stored = r->soc / 100.0 * capacity_kwh;
    double usable = (r->soc - reserve) / 100.0 * capacity_kwh; if (usable < 0) usable = 0;
    double to_full = (100 - r->soc) / 100.0 * capacity_kwh;
    double empty_h = -1, full_h = -1;
    if (capacity_kwh > 0 && bat_ema > 30) empty_h = usable * 1000.0 / bat_ema;
    if (capacity_kwh > 0 && bat_ema < -30) full_h = to_full * 1000.0 / -bat_ema;
    char errs[640], diag[640], berr[640], bwarn[640];
    bits_text(errs, sizeof errs, r->error_bits, ERROR_CODES);
    bits_text(diag, sizeof diag, r->diag_bits, DIAG_CODES);
    bits_text(berr, sizeof berr, r->bat_err, BMS_ALARMS);
    bits_text(bwarn, sizeof bwarn, r->bat_warn, BMS_WARNINGS);
    sb_t b; sb_init(&b, 8192);
    sb_printf(&b, "{\"ok\":%d,\"ts\":%lld,\"age_s\":%.1f,\"err\":\"%s\",",
              r->ok, r->ts, r->ok ? (now_ms() - r->ts) / 1000.0 : -1.0, last_err);
    sb_printf(&b, "\"pv\":{\"w\":%d,\"s1\":{\"v\":%.1f,\"a\":%.1f,\"w\":%d,\"mode\":%d,\"mode_label\":\"%s\"},"
              "\"s2\":{\"v\":%.1f,\"a\":%.1f,\"w\":%d,\"mode\":%d,\"mode_label\":\"%s\"}},",
              r->ppv, r->vpv1, r->ipv1, r->ppv1, r->pv1_mode, LABEL(PV_MODES, r->pv1_mode),
              r->vpv2, r->ipv2, r->ppv2, r->pv2_mode, LABEL(PV_MODES, r->pv2_mode));
    /* home_w: house load (inverter output + grid import). dc_net_w: the library's "house consumption"
       (pv + battery - grid, DC side). loss_w: DC in - AC out right now. */
    sb_printf(&b, "\"home_w\":%d,\"backup_w\":%d,\"load_w\":%d,\"dc_net_w\":%d,\"loss_w\":%.0f,",
              r->house_w, r->backup_w, r->load_w, r->ppv + r->bat_w - r->grid_w,
              r->ppv + r->bat_vi_w - r->grid_w - r->house_w);
    sb_printf(&b, "\"grid\":{\"w\":%d,\"mode\":%d,\"mode_label\":\"%s\",\"up\":%s,\"v\":%.1f,\"a\":%.1f,\"hz\":%.2f,"
              "\"inv_port_w\":%d,\"import_total_kwh\":%.1f},",
              r->grid_w, r->grid_mode, LABEL(GRID_MODES, r->grid_mode), r->grid_mode == 1 ? "true" : "false",
              r->vgrid, r->igrid, r->fgrid, r->pgrid, r->e_imp_total);
    sb_printf(&b, "\"battery\":{\"w\":%d,\"w_vi\":%.0f,\"w_avg\":%.0f,\"v\":%.1f,\"a\":%.1f,\"soc\":%d,\"soh\":%d,\"temp\":%s,"
              "\"mode\":\"%s\",\"capacity_kwh\":%.1f,\"reserve\":%d,\"stored_kwh\":%.2f,\"usable_kwh\":%.2f,"
              "\"hours_to_empty\":%.2f,\"hours_to_full\":%.2f,\"charge_limit\":%s,",
              r->bat_w, r->bat_vi_w, bat_ema, r->vbat, r->ibat, r->soc, r->soh, jf(r->bat_temp, 1),
              LABEL(BAT_MODES, r->bat_mode), capacity_kwh, reserve, stored, usable, empty_h, full_h, jf(charge_limit > 0 ? charge_limit : NA, 0));
    sb_printf(&b, "\"charge_limit_a\":%d,\"discharge_limit_a\":%d,\"status\":%d,\"bms\":%d,\"protocol\":%d,\"modules\":%d,"
              "\"warnings\":\"%s\",\"errors\":\"%s\",\"warning_bits\":%u,\"error_bits\":%u,"
              "\"cell_v_max\":%s,\"cell_v_min\":%s,\"cell_t_max\":%s,\"cell_t_min\":%s,"
              "\"life_charge_kwh\":%.1f,\"life_discharge_kwh\":%.1f,\"cycles\":%.1f},",
              r->chg_lim, r->dis_lim, r->bat_status, r->bms, r->bat_protocol, r->modules, bwarn, berr,
              r->bat_warn, r->bat_err, jf(r->cell_vmax, 3), jf(r->cell_vmin, 3), jf(r->cell_tmax, 1),
              jf(r->cell_tmin, 1), r->e_chg_total, r->e_dis_total, capacity_kwh > 0 ? r->e_dis_total / capacity_kwh : 0);
    sb_printf(&b, "\"inverter\":{\"v\":%.1f,\"a\":%.1f,\"hz\":%.2f,\"w\":%d,\"total_w\":%d,\"ups_load\":%d,"
              "\"temp_air\":%s,\"temp_radiator\":%s,\"temp_module\":%s,\"bus_v\":%.1f,\"nbus_v\":%.1f,"
              "\"work_mode\":%d,\"work_mode_label\":\"%s\",\"errors\":\"%s\",\"error_bits\":%u,\"warning_code\":%d,"
              "\"diag\":\"%s\",\"diag_bits\":%u,\"hours_total\":%d},",
              r->vout, r->iout, r->fout, r->pout, r->inv_total_w, r->ups_load,
              jf(r->t_air, 1), jf(r->inv_temp, 1), jf(r->t_module > 0 ? r->t_module : NA, 1), r->vbus, r->vnbus,
              r->work_mode, LABEL(WORK_MODES, r->work_mode), errs, r->error_bits, r->warning_code,
              diag, r->diag_bits, r->h_total);
    sb_printf(&b, "\"work_mode\":%d,\"inv_temp\":%s,\"today\":", r->work_mode, jf(r->inv_temp, 1));
    day_t *d = today_date ? day_get(today_date, 0) : NULL;
    if (d && d->v2) day_json(&b, d);
    else sb_printf(&b, "{\"pv\":%.1f,\"load\":%.1f,\"charge\":%.1f,\"discharge\":%.1f,\"import\":%.1f,\"export\":0,"
                   "\"basis\":\"counters\",\"counter\":{\"pv\":%.1f,\"load\":%.1f,\"charge\":%.1f,\"discharge\":%.1f,"
                   "\"import\":%.1f,\"ac_out_35199\":%.1f}}",
                   r->e_day, r->e_load, r->e_chg, r->e_dis, r->e_imp,
                   r->e_day, r->e_load, r->e_chg, r->e_dis, r->e_imp, r->e_exp);
    sb_printf(&b, ",\"lifetime\":{\"pv\":%.1f,\"load\":%.1f,\"import\":%.1f,\"ac_out_35195\":%.1f,"
              "\"charge\":%.1f,\"discharge\":%.1f},\"pv_total_kwh\":%.1f}",
              r->e_total, r->e_load_total, r->e_imp_total, r->e_exp_total, r->e_chg_total, r->e_dis_total, r->e_total);
    reply_sb(fd, &b);
}

/* detail series in /api/day ("x"): column name, field, signed?, scale, decimals */
#define XF(name, field, sgn, scale, dec) { name, offsetof(rec2_t, field), sgn, scale, dec }
static const struct { const char *name; size_t off; int sgn; double scale; int dec; } XCOLS[] = {
    XF("vbat", vbat, 0, 0.01, 2), XF("ibat", ibat, 1, 0.01, 2), XF("bat_w", bat, 1, 1, 0), XF("bat_rep", bat_rep, 1, 1, 0),
    XF("tbat", tbat, 1, 0.1, 1), XF("vpv1", vpv1, 0, 0.1, 1), XF("ipv1", ipv1, 0, 0.01, 2), XF("ppv1", ppv1, 1, 1, 0),
    XF("vpv2", vpv2, 0, 0.1, 1), XF("ipv2", ipv2, 0, 0.01, 2), XF("ppv2", ppv2, 1, 1, 0),
    XF("vout", vout, 0, 0.1, 1), XF("iout", iout, 0, 0.01, 2), XF("fout", fout, 0, 0.01, 2),
    XF("vgrid", vgrid, 0, 0.1, 1), XF("fgrid", fgrid, 0, 0.01, 2),
    XF("tinv", tinv, 1, 0.1, 1), XF("tair", tair, 1, 0.1, 1), XF("vbus", vbus, 0, 0.1, 1),
};

/* GET /api/day?d=YYYY-MM-DD : 1-minute curve [minute,pv,load,bat,grid,soc], detail columns, totals */
static void api_day(int fd, const char *q) {
    int now_date, now_min;
    local_parts(now_ms(), &now_date, &now_min);
    int date = parse_date(q, "d=", now_date);
    int want_x = !(q && strstr(q, "x=0"));
    rec2_t *recs = NULL; int n = 0, own = 0;
    if (date == today_date) { recs = today_recs; n = ntoday; }
    else if ((recs = malloc(sizeof(rec2_t) * MAX_REC))) { own = 1; n = load_recs(date, recs, MAX_REC); }
    sb_t b; sb_init(&b, 4096 + (size_t)n * (want_x ? 200 : 40));
    sb_printf(&b, "{\"date\":%d,\"today\":%s,\"now_min\":%d,\"points\":[", date, date == now_date ? "true" : "false", now_min);
    for (int i = 0; i < n; i++)
        sb_printf(&b, "%s[%u,%d,%d,%d,%d,%u]", i ? "," : "", recs[i].t, recs[i].pv, recs[i].load, recs[i].bat,
                  recs[i].grid, recs[i].soc);
    sb_printf(&b, "]");
    if (want_x) {                                /* only minutes recorded in the detailed format */
        sb_printf(&b, ",\"x\":{\"t\":[");
        int k = 0;
        for (int i = 0; i < n; i++) if (recs[i].n) sb_printf(&b, "%s%u", k++ ? "," : "", recs[i].t);
        sb_printf(&b, "]");
        for (size_t c = 0; c < sizeof XCOLS / sizeof XCOLS[0]; c++) {
            sb_printf(&b, ",\"%s\":[", XCOLS[c].name);
            k = 0;
            for (int i = 0; i < n; i++) {
                if (!recs[i].n) continue;
                const char *p = (const char *)&recs[i] + XCOLS[c].off;
                int raw = XCOLS[c].sgn ? *(const short *)p : *(const unsigned short *)p;
                if (XCOLS[c].sgn && raw == NO_T) sb_printf(&b, "%snull", k++ ? "," : "");
                else sb_printf(&b, "%s%.*f", k++ ? "," : "", XCOLS[c].dec, raw * XCOLS[c].scale);
            }
            sb_printf(&b, "]");
        }
        sb_printf(&b, "}");
    }
    sb_printf(&b, ",\"summary\":");
    day_json(&b, day_get(date, 0));
    sb_printf(&b, "}");
    reply_sb(fd, &b);
    if (own) free(recs);
}

/* GET /api/days?from=YYYYMMDD&to=YYYYMMDD : daily totals
   [date, pv, load, charge, discharge, import, peak_pv, loss, measured minutes, counter fill kWh]
   (the last three are null for days from before the measured accounting) */
static void api_days(int fd, const char *q) {
    int from = parse_date(q, "from=", 0), to = parse_date(q, "to=", 99999999);
    sb_t b; sb_init(&b, 256 + (size_t)ndays * 120);
    sb_printf(&b, "{\"first\":%d,\"days\":[", ndays ? days[0].date : 0);
    int k = 0;
    for (int i = 0; i < ndays; i++) {
        day_t *d = &days[i];
        if (d->date < from || d->date > to) continue;
        sb_printf(&b, "%s[%d,%.2f,%.2f,%.2f,%.2f,%.2f,%d,%s,%s,%s]", k++ ? "," : "",
                  d->date, d->pv, d->load, d->chg, d->dis, d->imp, d->peak_pv,
                  jf(d->v2 ? day_loss(d) : NA, 2), jf(d->v2 ? d->meas_s / 60.0 : NA, 0), jf(d->v2 ? day_fill(d) : NA, 2));
    }
    sb_printf(&b, "]}");
    reply_sb(fd, &b);
}

#if defined(__has_include)
#if __has_include("index_html.h")
#include "index_html.h"                  /* the dashboard, generated from index.html at build time */
#define HAVE_PAGE 1
#endif
#endif

static void serve_file(int fd, const char *name, const char *ctype) {
    char path[640]; struct stat st;
    snprintf(path, sizeof path, "%s/%s", www_dir, name);
    int f = www_dir[0] ? open(path, O_RDONLY) : -1;
    if (f < 0 || fstat(f, &st) < 0) {
        if (f >= 0) close(f);
#ifdef HAVE_PAGE
        reply(fd, 200, ctype, (const char *)index_html, index_html_len);
#else
        reply(fd, 404, "text/plain", "not found", 9);
#endif
        return;
    }
    char *b = malloc((size_t)st.st_size + 1);
    ssize_t got = b ? read(f, b, (size_t)st.st_size) : -1;
    close(f);
    if (got >= 0) reply(fd, 200, ctype, b, (size_t)got);
    free(b);
}

static void handle(int fd) {
    char req[2048]; struct timeval to = { 2, 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &to, sizeof to);
    ssize_t n = read(fd, req, sizeof req - 1);
    if (n <= 0) { close(fd); return; }
    req[n] = 0;
    char *p = strchr(req, ' '); if (!p) { close(fd); return; }
    p++;
    char *q = NULL, *e = strchr(p, ' '); if (e) *e = 0;
    if ((e = strchr(p, '?'))) { *e = 0; q = e + 1; }
    if (!strcmp(p, "/api/now")) api_now(fd);
    else if (!strcmp(p, "/api/day")) api_day(fd, q);
    else if (!strcmp(p, "/api/days")) api_days(fd, q);
    else if (!strcmp(p, "/ping")) reply(fd, 200, "text/plain", "solard ok", 9);
    else if (!strcmp(p, "/api/config")) {       /* the page's setup, for browsers (the app keeps its own) */
        char c[320];
        int n = snprintf(c, sizeof c, "{\"app\":false,\"configured\":true,\"capacity_kwh\":%.1f,\"charge_limit\":%d,"
                         "\"lat\":%s,\"lon\":%s,\"sun_mode\":\"auto\",\"sunrise\":\"06:00\",\"sunset\":\"18:00\"}",
                         capacity_kwh, charge_limit > 0 ? charge_limit : 100, jf(site_lat, 4), jf(site_lon, 4));
        reply(fd, 200, "application/json", c, (size_t)n);
    }
    else if (!strcmp(p, "/") || !strcmp(p, "/index.html")) serve_file(fd, "index.html", "text/html; charset=utf-8");
    else reply(fd, 404, "text/plain", "not found", 9);
    close(fd);
}

/* ---------------------------------------------------------------- main */

static void on_term(int sig) { (void)sig; stop_now = 1; }

static const char *const ENV_KEYS[][2] = {
    { "SOLARD_HOST", "host" }, { "SOLARD_PORT", "port" }, { "SOLARD_HTTP", "http" }, { "SOLARD_CAPACITY", "capacity" },
    { "SOLARD_LAT", "lat" }, { "SOLARD_LON", "lon" }, { "SOLARD_DATA", "data" }, { "SOLARD_WWW", "www" },
    { "SOLARD_UTC_OFFSET", "utc_offset" } };

int main(int argc, char **argv) {
    int only_discover = 0;
    /* where the binary lives: default home of solard.conf and data/ */
    char home[512] = ".";
    const char *slash = strrchr(argv[0], '/'), *bs = strrchr(argv[0], '\\');
    if (bs > slash) slash = bs;
    if (slash) snprintf(home, sizeof home, "%.*s", (int)(slash - argv[0]), argv[0]);
    if (getenv("SOLARD_CONFIG")) snprintf(conf_path, sizeof conf_path, "%s", getenv("SOLARD_CONFIG"));
    else snprintf(conf_path, sizeof conf_path, "%s/solard.conf", home);
    for (int i = 1; i + 1 < argc; i++) if (!strcmp(argv[i], "--config")) snprintf(conf_path, sizeof conf_path, "%s", argv[i + 1]);
    char *cs = strrchr(conf_path, '/');                            /* data/ defaults to beside the config */
    snprintf(data_dir, sizeof data_dir, "%.*s/data", cs ? (int)(cs - conf_path) : 1, cs ? conf_path : ".");

    load_conf(conf_path);                                          /* 1. solard.conf */
    for (size_t k = 0; k < sizeof ENV_KEYS / sizeof ENV_KEYS[0]; k++)   /* 2. environment */
        if (getenv(ENV_KEYS[k][0])) set_opt(ENV_KEYS[k][1], getenv(ENV_KEYS[k][0]));
    for (int i = 1; i < argc; i++) {                               /* 3. command line */
        const char *a = argv[i];
        if (!strcmp(a, "--discover")) only_discover = 1;
        else if (!strcmp(a, "--help") || !strcmp(a, "-h")) {
            printf("solard -- monitor for GoodWe hybrid inverters\n"
                   "usage: solard [--config FILE] [--host IP] [--port 502] [--http 8768] [--capacity KWH]\n"
                   "              [--lat DEG --lon DEG] [--data DIR] [--www DIR] [--utc-offset +HH:MM]\n"
                   "       solard --discover     find the inverter on this network and exit\n"
                   "settings file: %s (key = value; env SOLARD_HOST etc. also work)\n", conf_path);
            return 0;
        }
        else if (!strncmp(a, "--", 2) && i + 1 < argc && strcmp(a, "--config")) set_opt(a + 2, argv[++i]);
        else if (!strcmp(a, "--config")) i++;
    }

    if (only_discover) {
        printf("searching for a GoodWe inverter...\n");
        char ip[64];
        if (discover(ip, sizeof ip, 1) == 0) { printf("found: %s\n", ip); return 0; }
        printf("none found\n");
        return 1;
    }
    signal(SIGPIPE, SIG_IGN);
    struct sigaction sa; memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_term;                                 /* save the running day on stop/restart */
    sigaction(SIGTERM, &sa, NULL); sigaction(SIGINT, &sa, NULL);
    setvbuf(stdout, NULL, _IOLBF, 0);

    mkdir(data_dir, 0755);
    load_days();
    load_state();

    int ls = socket(AF_INET, SOCK_STREAM, 0), one = 1;
    struct sockaddr_in a; memset(&a, 0, sizeof a);
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    a.sin_family = AF_INET; a.sin_port = htons((unsigned short)http_port); a.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(ls, (struct sockaddr *)&a, sizeof a) < 0 || listen(ls, 16) < 0) { perror("bind"); return 1; }
    printf("solard: dashboard on http://<this machine>:%d/  data in %s  settings %s\n", http_port, data_dir, conf_path);

    long long next_poll = 0, next_settings = 0, next_discover = 0;
    int fails = 0;
    while (!stop_now) {
        long long t = now_ms();
        /* no inverter yet, or it stopped answering for 5 minutes (new address?): look for it */
        if ((!host_buf[0] || fails >= 60) && t >= next_discover) {
            char ip[64];
            snprintf(last_err, sizeof last_err, "searching for the inverter");
            if (discover(ip, sizeof ip, 0) == 0) {
                if (strcmp(ip, host_buf)) { printf("solard: inverter at %s\n", ip); snprintf(host_buf, sizeof host_buf, "%s", ip); save_host(); }
                inv_close(); fails = 0; next_poll = 0; next_settings = 0;
            }
            next_discover = now_ms() + 60000;
        }
        if (host_buf[0] && t >= next_poll) {
            if (t >= next_settings) { poll_settings(); next_settings = t + 600000; }
            if (poll_inverter() < 0) { fails++; last_err_ts = t; }
            else fails = 0;
            /* back off gently if the dongle stops answering */
            next_poll = now_ms() + (fails ? (fails < 6 ? 5000 : 30000) : POLL_MS);
        }
        struct pollfd pf = { ls, POLLIN, 0 };
        int wait = (int)((host_buf[0] ? next_poll : next_discover) - now_ms()); if (wait < 0) wait = 0; if (wait > 5000) wait = 5000;
        if (poll(&pf, 1, wait) > 0) {
            int c = accept(ls, NULL, NULL);
            if (c >= 0) handle(c);
        }
    }
    save_state();
    save_days();
    return 0;
}
