/*********************************************************************
 * SNACK DISPENSER
 * * DESCRIPTION:
 * An embedded C application for a snack vending machine utilizing
 * a keypad, 16x2 LCD, 7-segment display, and stepper motor.
 * * CORE FUNCTIONALITIES:
 * - UNIVERSAL PQIV LOADER: Manages a rolling ring of 8 persistent 
 * process IDs for image rendering to prevent desktop flickering.
 * - DUAL ANIMATION ENGINE: Non-blocking frame handler for simultaneous
 * door movement and snack dispensing.
 * - SMART DISPENSING: Synchronizes stepper motor cycles (3s per item)
 * with visual frame updates.
 * - DUAL MODE INTERFACE: 
 * 1. NORMAL: Product selection, 9s idle timer, and payment simulation.
 * 2. SERVICE: Password-protected (1234) mode with custom DIP 
 * port mapping for restocking, sound testing, and motor diagnostics.
 * * TECHNICAL SPECS:
 * - Door Animation: 4 frames at 800ms intervals.
 * - Sound Cues: DAC-driven beeps for keypresses, errors, success, 
 * and slot-specific dispensing tones.
 * - Timing: High-precision monotonic clock handling for non-blocking loops.
 *********************************************************************/

#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <time.h>
#include <stdlib.h>
#include <ctype.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/stat.h>

#include "library.h"

/* ===== Ports (NORMAL mapping) ===== */
#define LEDPORT_NORMAL 0x3A
#define LCDPORT_NORMAL 0x3B
#define SMPORT_NORMAL  0x39
#define KBDPORT_NORMAL 0x3C

/* ===== Ports (ADMIN mapping via DIP) ===== */
#define LEDPORT_ADMIN  0x1A
#define LCDPORT_ADMIN  0x1B
#define SMPORT_ADMIN   0x19
#define KBDPORT_ADMIN  0x1C

/* Runtime ports */
static unsigned char gLedPort = LEDPORT_NORMAL;
static unsigned char gLcdPort = LCDPORT_NORMAL;
static unsigned char gSmPort  = SMPORT_NORMAL;
static unsigned char gKbdPort = KBDPORT_NORMAL;

static void set_port_mapping(int admin)
{
    if (admin) {
        gLedPort = LEDPORT_ADMIN;
        gLcdPort = LCDPORT_ADMIN;
        gSmPort  = SMPORT_ADMIN;
        gKbdPort = KBDPORT_ADMIN;
    } else {
        gLedPort = LEDPORT_NORMAL;
        gLcdPort = LCDPORT_NORMAL;
        gSmPort  = SMPORT_NORMAL;
        gKbdPort = KBDPORT_NORMAL;
    }
}

/* ===== Keypad scan constants ===== */
#define Col7Lo 0xF7
#define Col6Lo 0xFB
#define Col5Lo 0xFD
#define Col4Lo 0xFE

static const unsigned char ScanTable[12] =
/* 0..9, A, B */
{
    0xB7, 0x7E, 0xBE, 0xDE,
    0x7D, 0xBD, 0xDD, 0x7B,
    0xBB, 0xDB, 0x77, 0xD7
};
static unsigned char ScanCode;

/* ===== 7-seg ===== */
static const unsigned char Bin2LED[] =
{
    0x40, 0x79, 0x24, 0x30,
    0x19, 0x12, 0x02, 0x78,
    0x00, 0x18, 0x08, 0x03,
    0x46, 0x21, 0x06, 0x0E
};

static void seg_blank(void) { CM3_outport(gLedPort, 0xFF); }
static void seg_show_digit(int d)
{
    if (d < 0 || d > 9) { seg_blank(); return; }
    CM3_outport(gLedPort, Bin2LED[d]);
}

/* ===== Stepper ===== */
static int full_seq_drive[4] = {0x08, 0x04, 0x02, 0x01};

/* ===== Images ===== */
#define IMG_MENU           "/tmp/menu.jpg"
#define IMG_THANKS         "/tmp/success.jpg"
#define IMG_DISP_FALLBACK  "/tmp/dispensing.jpg"

#define IMG_DISP_1         "/tmp/disp_1.jpg"
#define IMG_DISP_2         "/tmp/disp_2.jpg"
#define IMG_DISP_3         "/tmp/disp_3.jpg"
#define IMG_DISP_4         "/tmp/disp_4.jpg"

/* Service UI images */
#define IMG_SERVICE_MANUAL "/tmp/service.jpg"
#define IMG_MENU_SERVICE   "/tmp/menu_service.jpg"
#define IMG_RESTOCK        "/tmp/restock.jpg"
#define IMG_SOUND          "/tmp/sound.jpg"
#define IMG_MOTOR          "/tmp/motor.jpg"

/* Door animation frames */
#define IMG_DOOR_1         "/tmp/door_1.jpg"
#define IMG_DOOR_2         "/tmp/door_2.jpg"
#define IMG_DOOR_3         "/tmp/door_3.jpg"
#define IMG_DOOR_4         "/tmp/door_4.jpg"

/* Zoom images (normal) */
#define IMG_ZOOM_1         "/tmp/cheetos.jpg"
#define IMG_ZOOM_2         "/tmp/lays.jpg"
#define IMG_ZOOM_3         "/tmp/doritos.jpg"
#define IMG_ZOOM_4         "/tmp/pocky.jpg"

/* Zoom images (OUT OF STOCK pre-rendered) */
#define IMG_ZOOM_1_OOS     "/tmp/cheetos_oos.jpg"
#define IMG_ZOOM_2_OOS     "/tmp/lays_oos.jpg"
#define IMG_ZOOM_3_OOS     "/tmp/doritos_oos.jpg"
#define IMG_ZOOM_4_OOS     "/tmp/pocky_oos.jpg"

/* ===== Keys ===== */
#define KEY_BACK  'A'   /* '*' */
#define KEY_ENTER 'B'   /* '#' */

/* ===== Timeouts ===== */
#define IDLE_MS 9000
#define SVC_GATE_TIMEOUT_MS    8000
#define RETURN_GATE_TIMEOUT_MS 8000
#define MAX_COUNT 15

/* ===== Sleeps ===== */
#define USLEEP_ERR_SHORT_US        700000
#define USLEEP_ERR_LONG_US        1200000
#define USLEEP_SUCCESS_SCREEN_US  5000000
#define USLEEP_SVC_DONE_US        1200000
#define USLEEP_OOS_SCREEN_US      4500000

static long long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000LL + ts.tv_nsec / 1000000LL;
}

/* ===== LCD ===== */
static void initlcd(void);
static void lcd_writecmd(char cmd);
static void LCDprint(char *sptr);
static void lcddata(unsigned char cmd);

static void lcd_clear(void)
{
    lcd_writecmd(0x01);
    usleep(2000);
}
static void lcd_line2(void) { lcd_writecmd(0xC0); }

static void lcd_print2(const char *l1, const char *l2)
{
    char a[17], b[17];
    snprintf(a, sizeof(a), "%-16.16s", l1);
    snprintf(b, sizeof(b), "%-16.16s", l2);
    initlcd();
    lcd_clear();
    lcd_writecmd(0x80);
    LCDprint(a);
    lcd_line2();
    LCDprint(b);
}

/* ===== Files ===== */
static int file_exists(const char *p)
{
    struct stat st;
    return (stat(p, &st) == 0);
}

/* ===== PQIV / X helpers ===== */
static void env_for_x(void)
{
    const char *disp = getenv("DISPLAY");
    if (!disp || !*disp) {
        if (file_exists("/tmp/.X11-unix/X1")) disp = ":1";
        else if (file_exists("/tmp/.X11-unix/X0")) disp = ":0";
        else disp = ":0";
        setenv("DISPLAY", disp, 1);
    }

    const char *home = getenv("HOME");
    if (home && *home) {
        char xa[512];
        snprintf(xa, sizeof(xa), "%s/.Xauthority", home);
        setenv("XAUTHORITY", xa, 1);
    } else {
        unsetenv("XAUTHORITY");
    }

    setenv("NO_AT_BRIDGE", "1", 1);
}

static void kill_pid_soft_hard(pid_t p)
{
    if (p <= 0) return;
    kill(p, SIGTERM);
    usleep(60000);
    kill(p, SIGKILL);
}

/* ===== UNIVERSAL PQIV ROLLING 8 (NO killall) ===== */
#define PQIV_KEEP 8
static pid_t pqiv_ring[PQIV_KEEP] = {0};
static int pqiv_pos = 0;
static int pqiv_count = 0;

static void pqiv_kill_all_spawned(void)
{
    for (int i = 0; i < PQIV_KEEP; i++) {
        kill_pid_soft_hard(pqiv_ring[i]);
        pqiv_ring[i] = 0;
    }
    pqiv_pos = 0;
    pqiv_count = 0;
}

static void show_image(const char *path)
{
    if (pqiv_count >= PQIV_KEEP) {
        kill_pid_soft_hard(pqiv_ring[pqiv_pos]);
        pqiv_ring[pqiv_pos] = 0;
    }

    pid_t pid = fork();
    if (pid == 0) {
        env_for_x();
        execlp("pqiv", "pqiv", "-f", path, (char*)NULL);
        _exit(127);
    } else if (pid > 0) {
        pqiv_ring[pqiv_pos] = pid;
        pqiv_pos = (pqiv_pos + 1) % PQIV_KEEP;
        if (pqiv_count < PQIV_KEEP) pqiv_count++;
    }

    usleep(25000);
}

/* ===== Shared Animation Engine (non-blocking) ===== */
typedef struct {
    int active;
    const char **frames;
    int nframes;

    int idx;
    int direction;         /* +1 forward, -1 backward */
    long long next_ms;
    int frame_ms;

    int oneshot_done;      /* 1 when reached end */
} Anim;

static void anim_start(Anim *a, const char **frames, int nframes, int direction, int frame_ms)
{
    a->active = 1;
    a->frames = frames;
    a->nframes = nframes;
    a->direction = (direction >= 0) ? +1 : -1;
    a->frame_ms = frame_ms;
    a->oneshot_done = 0;

    a->idx = (a->direction > 0) ? 0 : (nframes - 1);
    a->next_ms = now_ms(); /* show immediately */
}

static void anim_tick(Anim *a)
{
    if (!a->active || a->oneshot_done) return;

    long long t = now_ms();
    if (t < a->next_ms) return;

    const char *p = a->frames[a->idx];
    if (!file_exists(p)) p = IMG_MENU;
    show_image(p);

    a->next_ms = t + a->frame_ms;

    if (a->direction > 0) {
        if (a->idx == a->nframes - 1) {
            a->oneshot_done = 1;
            a->active = 0;
        } else {
            a->idx++;
        }
    } else {
        if (a->idx == 0) {
            a->oneshot_done = 1;
            a->active = 0;
        } else {
            a->idx--;
        }
    }
}

/* Door frames */
static const char* door_frames[] = { IMG_DOOR_1, IMG_DOOR_2, IMG_DOOR_3, IMG_DOOR_4 };
static const int DOOR_N = 4;
#define DOOR_FRAME_MS 800

/* Dispense frames */
static const char* disp_frames[] = { IMG_DISP_1, IMG_DISP_2, IMG_DISP_3, IMG_DISP_4 };
static const int DISP_N = 4;
#define DISP_FRAME_MS DOOR_FRAME_MS

static Anim gDoorAnim;
static Anim gDispAnim;

/* ===== Exit handling (NO killall) ===== */
static void cleanup(void) { pqiv_kill_all_spawned(); }
static void on_sig(int sig)
{
    (void)sig;
    cleanup();
    _exit(0);
}

/* ===== Keypad ===== */
static unsigned char ProcKey(void)
{
    for (unsigned char j = 0; j < 12; j++) {
        if (ScanCode == ScanTable[j]) {
            if (j > 9) return (unsigned char)(j + 0x37); /* A, B */
            return (unsigned char)(j + 0x30);            /* 0-9 */
        }
    }
    return 0xFF;
}

static unsigned char ScanKey(void)
{
    CM3_outport(gKbdPort, Col7Lo);
    ScanCode = CM3_inport(gKbdPort);
    ScanCode |= 0x0F;
    ScanCode &= Col7Lo;
    if (ScanCode != Col7Lo) return ProcKey();

    CM3_outport(gKbdPort, Col6Lo);
    ScanCode = CM3_inport(gKbdPort);
    ScanCode |= 0x0F;
    ScanCode &= Col6Lo;
    if (ScanCode != Col6Lo) return ProcKey();

    CM3_outport(gKbdPort, Col5Lo);
    ScanCode = CM3_inport(gKbdPort);
    ScanCode |= 0x0F;
    ScanCode &= Col5Lo;
    if (ScanCode != Col5Lo) return ProcKey();

    CM3_outport(gKbdPort, Col4Lo);
    ScanCode = CM3_inport(gKbdPort);
    ScanCode |= 0x0F;
    ScanCode &= Col4Lo;
    if (ScanCode != Col4Lo) return ProcKey();

    return 0xFF;
}

static void wait_key_release(void)
{
    while (ScanKey() != 0xFF) usleep(12000);
}

/* ===== Motor helpers ===== */
static void motor_write_phase(int phase)
{
    CM3_outport(gSmPort, full_seq_drive[phase & 3]);
}

/* ===== DAC beeps ===== */
static void dac_write(unsigned char v)
{
    CM3PortWrite(3, v);
    CM3PortWrite(5, v);
}

static void beep_square(int duration_ms, int half_period_us, unsigned char hi, unsigned char lo)
{
    long long end = now_ms() + duration_ms;
    while (now_ms() < end) {
        dac_write(hi);
        usleep(half_period_us);
        dac_write(lo);
        usleep(half_period_us);
    }
    dac_write(0);
}

static void beep_keypress(void) { beep_square(25, 650, 200, 20); }
static void beep_error(void)    { beep_square(140, 1400, 180, 0); }

static void beep_success(void)
{
    beep_square(70, 800, 220, 10);
    usleep(35000);
    beep_square(70, 500, 220, 10);
}

static void beep_payment_ok(void)
{
    beep_square(60, 900, 220, 0);
    usleep(20000);
    beep_square(60, 650, 220, 0);
}

/* Dispensing sound cues (slot-specific) */
static void beep_dispensing_slot(int slot)
{
    switch (slot) {
        case 1: beep_square(180, 900, 220, 0); break;
        case 2: beep_square(220, 700, 220, 0); break;
        case 3: beep_square(260, 550, 220, 0); break;
        case 4: beep_square(320, 450, 220, 0); break;
        default: beep_square(200, 650, 220, 0); break;
    }
}

/* ===== Items ===== */
typedef struct {
    int index;
    const char *name;
    float price;
    const char *img;
    const char *img_oos;
    int stock;
} Item;

static void format_money(char out[12], float v)
{
    snprintf(out, 12, "$%.2f", (double)(v + 0.0001f));
}

static int find_slot_by_index(Item *items, int n, int idx)
{
    for (int i = 0; i < n; i++) if (items[i].index == idx) return i;
    return -1;
}

/* ===== 9s timer (normal mode only) ===== */
static long long idle_deadline = 0;
static int last_shown = -1;

static void timer_start_or_reset(void)
{
    idle_deadline = now_ms() + IDLE_MS;
    last_shown = -1;
}

static int timer_seconds_left(long long t)
{
    if (idle_deadline <= 0) return -1;
    long long rem = idle_deadline - t;
    if (rem <= 0) return 0;
    int sec = (int)((rem + 999) / 1000);
    if (sec > 9) sec = 9;
    if (sec < 0) sec = 0;
    return sec;
}

static void timer_update_display(long long t)
{
    int left = timer_seconds_left(t);
    if (left < 0) return;
    if (left != last_shown) { seg_show_digit(left); last_shown = left; }
}

static void timer_stop_and_blank(void)
{
    idle_deadline = 0;
    last_shown = -1;
    seg_blank();
}

/* ===== Service 7-seg blink ===== */
static long long svc_blink_next = 0;
static int svc_blink_on = 1;

static void service_blink_tick(long long t)
{
    if (svc_blink_next == 0) {
        svc_blink_next = t + 500;
        svc_blink_on = 1;
        seg_show_digit(0);
        return;
    }
    if (t >= svc_blink_next) {
        svc_blink_next += 500;
        svc_blink_on = !svc_blink_on;
        if (svc_blink_on) seg_show_digit(0);
        else seg_blank();
    }
}

static void service_blink_reset(void)
{
    svc_blink_next = 0;
    svc_blink_on = 1;
    seg_show_digit(0);
}

/* ===== Service menu LCD (fits 16 chars) ===== */
static void service_menu_screen(const char *typed)
{
    show_image(IMG_SERVICE_MANUAL);

    char l1[17], l2[17];
    if (typed && *typed) snprintf(l1, sizeof(l1), "Svc:%-12.12s", typed);
    else                snprintf(l1, sizeof(l1), "Svc:");
    snprintf(l2, sizeof(l2), "B=OK 1-4/1234");
    lcd_print2(l1, l2);
}

/* ===== DIP gate prompts ===== */
static void show_service_gate_prompt(void)
{
    show_image(IMG_MENU);
    lcd_print2("Flip SA5 DIP", "Press any key");
}
static void show_return_gate_prompt(void)
{
    show_image(IMG_SERVICE_MANUAL);
    lcd_print2("Revert SA5 DIP", "Press any key");
}

/* ===== Dispense motor timing: 3 seconds per item ===== */
#define TOTAL_STEPS_PER_ITEM 60
#define DISPENSE_CYCLE_US 3000000
#define DISP_PHASE_DELAY_US (DISPENSE_CYCLE_US / (TOTAL_STEPS_PER_ITEM * 4))

static void run_one_dispense_cycle_with_anim(void)
{
    static int phase = 0;

    for (int s = 0; s < TOTAL_STEPS_PER_ITEM; s++) {
        anim_tick(&gDispAnim);
        for (int i = 0; i < 4; i++) {
            motor_write_phase(phase);
            phase = (phase + 1) & 3;
            anim_tick(&gDispAnim);
            usleep(DISP_PHASE_DELAY_US);
        }
    }
    CM3_outport(gSmPort, 0x00);
}

/* ===== Service motor test: short spin once + 0.5s gap, repeat N cycles ===== */
#define MOTOR_STEPS_PER_CYCLE 18   /* smaller = less rotation (tune 12..30) */
#define MOTOR_PHASE_DELAY_US  4500 /* tune speed */

static void motor_spin_one_cycle(void)
{
    static int phase = 0;

    for (int s = 0; s < MOTOR_STEPS_PER_CYCLE; s++) {
        for (int i = 0; i < 4; i++) {
            motor_write_phase(phase);
            phase = (phase + 1) & 3;
            usleep(MOTOR_PHASE_DELAY_US);
        }
    }
    CM3_outport(gSmPort, 0x00);
}

static void run_motor_test_cycles(int cycles)
{
    if (cycles < 1) cycles = 1;
    if (cycles > 15) cycles = 15;

    for (int c = 0; c < cycles; c++) {
        motor_spin_one_cycle();
        usleep(500000); /* 0.5s delay */
    }
}

/* ===== MAIN ===== */
int main(void)
{
    atexit(cleanup);
    signal(SIGINT, on_sig);
    signal(SIGTERM, on_sig);

    CM3DeviceInit();
    CM3DeviceSpiInit(0);

    CM3PortInit(4);
    CM3PortInit(1);
    CM3PortInit(0);
    CM3PortInit(3);
    CM3PortInit(5);

    Item items[] = {
        {  3, "Cheetos", 1.50f, IMG_ZOOM_1, IMG_ZOOM_1_OOS, 1 },
        {  8, "Lays",    1.50f, IMG_ZOOM_2, IMG_ZOOM_2_OOS, 2 },
        { 11, "Doritos", 1.50f, IMG_ZOOM_3, IMG_ZOOM_3_OOS, 3 },
        { 22, "Pocky",   1.75f, IMG_ZOOM_4, IMG_ZOOM_4_OOS, 4 },
    };
    const int N = (int)(sizeof(items) / sizeof(items[0]));

    enum {
        ST_MENU = 0,
        ST_AMOUNT,
        ST_PAY,

        ST_SVC_GATE,
        ST_RETURN_GATE,

        ST_DOOR_OPENING,
        ST_DOOR_CLOSING,

        ST_SVC_MENU,

        /* Service substates */
        ST_SVC_DISPENSE_IDX,
        ST_SVC_DISPENSE_AMT,
        ST_SVC_RESTOCK_IDX,
        ST_SVC_RESTOCK_QTY,
        ST_SVC_SOUND_SEL,
        ST_SVC_MOTOR_CYC,

        ST_DISPENSING
    } st = ST_MENU;

    /* normal buffers */
    char selbuf[8] = {0};
    int sellen = 0;

    char amtbuf[8] = {0};
    int amtlen = 0;

    /* service input buffer */
    char svcbuf[8] = {0};
    int svclen = 0;

    int chosen_slot = -1;
    int amount = 0;
    float total = 0.0f;

    int pay_zero_count = 0;
    int index_timer_active = 0;
    int service_mode = 0;

    long long svc_gate_deadline = 0;
    long long return_gate_deadline = 0;

    int svc_disp_slot = -1;
    int restock_slot = -1;

    show_image(IMG_MENU);
    lcd_print2("Enter Index:", "B to enter");
    timer_stop_and_blank();

    while (1) {
        long long t = now_ms();

        /* tick animations globally */
        anim_tick(&gDoorAnim);
        anim_tick(&gDispAnim);

        /* door transitions */
        if (st == ST_DOOR_OPENING && gDoorAnim.oneshot_done) {
            st = ST_SVC_MENU;
            sellen = 0; selbuf[0] = '\0';
            service_menu_screen(selbuf);
        }
        if (st == ST_DOOR_CLOSING && gDoorAnim.oneshot_done) {
            service_mode = 0;
            set_port_mapping(0);
            st = ST_MENU;

            sellen = 0; selbuf[0] = '\0';
            amtlen = 0; amtbuf[0] = '\0';
            chosen_slot = -1;
            index_timer_active = 0;

            show_image(IMG_MENU);
            lcd_print2("Enter Index:", "B to enter");
            timer_stop_and_blank();
        }

        /* Gate timeouts */
        if (st == ST_SVC_GATE && svc_gate_deadline > 0 && t >= svc_gate_deadline) {
            beep_error();
            svc_gate_deadline = 0;
            set_port_mapping(0);
            service_mode = 0;
            st = ST_MENU;
            show_image(IMG_MENU);
            lcd_print2("Enter Index:", "B to enter");
            continue;
        }
        if (st == ST_RETURN_GATE && return_gate_deadline > 0 && t >= return_gate_deadline) {
            beep_error();
            return_gate_deadline = 0;
            service_mode = 1;
            set_port_mapping(1);
            st = ST_SVC_MENU;
            service_menu_screen(selbuf);
            continue;
        }

        /* 7-seg behavior */
        if (service_mode) service_blink_tick(t);
        else {
            int timer_active = (st == ST_AMOUNT) || (st == ST_PAY) || (st == ST_MENU && index_timer_active);
            if (timer_active) {
                timer_update_display(t);
                if (timer_seconds_left(t) == 0) {
                    beep_error();
                    st = ST_MENU;
                    sellen = 0; selbuf[0] = '\0';
                    amtlen = 0; amtbuf[0] = '\0';
                    chosen_slot = -1;
                    index_timer_active = 0;
                    timer_stop_and_blank();
                    show_image(IMG_MENU);
                    lcd_print2("Enter Index:", "B to enter");
                    continue;
                }
            }
        }

        /* Dispensing state */
        if (st == ST_DISPENSING) {
            if (!gDispAnim.active && !gDispAnim.oneshot_done) {
                anim_start(&gDispAnim, disp_frames, DISP_N, +1, DISP_FRAME_MS);
            }

            for (int i = 0; i < amount; i++) {
                run_one_dispense_cycle_with_anim();
                if (i != amount - 1) usleep(150000);
            }

            while (!gDispAnim.oneshot_done) {
                anim_tick(&gDispAnim);
                usleep(20000);
            }

            show_image(IMG_THANKS);
            lcd_print2("Done!", "Thank you");
            beep_success();
            usleep(USLEEP_SUCCESS_SCREEN_US);

            items[chosen_slot].stock -= amount;
            if (items[chosen_slot].stock < 0) items[chosen_slot].stock = 0;

            st = ST_MENU;
            chosen_slot = -1;
            amount = 0;
            total = 0.0f;
            pay_zero_count = 0;

            sellen = 0; selbuf[0] = '\0';
            amtlen = 0; amtbuf[0] = '\0';
            index_timer_active = 0;
            timer_stop_and_blank();

            gDispAnim.oneshot_done = 0;
            show_image(IMG_MENU);
            lcd_print2("Enter Index:", "B to enter");
            continue;
        }

        unsigned char k = ScanKey();
        if (k == 0xFF) { usleep(20000); continue; }

        beep_keypress();
        wait_key_release();

        /* Gate confirms (any key) */
        if (st == ST_SVC_GATE) {
            svc_gate_deadline = 0;
            service_mode = 1;
            service_blink_reset();
            st = ST_DOOR_OPENING;

            gDoorAnim.oneshot_done = 0;
            anim_start(&gDoorAnim, door_frames, DOOR_N, +1, DOOR_FRAME_MS);
            continue;
        }
        if (st == ST_RETURN_GATE) {
            return_gate_deadline = 0;
            st = ST_DOOR_CLOSING;

            gDoorAnim.oneshot_done = 0;
            anim_start(&gDoorAnim, door_frames, DOOR_N, -1, DOOR_FRAME_MS);
            continue;
        }

        /* BACK (A) */
        if (k == KEY_BACK) {
            if (!service_mode) {
                if (st == ST_MENU) {
                    if (sellen > 0) { sellen--; selbuf[sellen] = '\0'; }
                    show_image(IMG_MENU);
                    char l1[17];
                    snprintf(l1, sizeof(l1), "Enter Index:%-4.4s", selbuf);
                    lcd_print2(l1, "B to enter");
                    if (sellen == 0) { index_timer_active = 0; timer_stop_and_blank(); }
                } else {
                    st = ST_MENU;
                    chosen_slot = -1;
                    amtlen = 0; amtbuf[0] = '\0';
                    show_image(IMG_MENU);
                    lcd_print2("Enter Index:", "B to enter");
                    timer_stop_and_blank();
                    index_timer_active = 0;
                }
            } else {
                /* service: A always returns to service menu */
                st = ST_SVC_MENU;
                sellen = 0; selbuf[0] = '\0';
                svclen = 0; svcbuf[0] = '\0';
                svc_disp_slot = -1;
                restock_slot = -1;
                service_menu_screen(selbuf);
            }
            continue;
        }

        /* DIGITS */
        if (isdigit((int)k)) {
            if (!service_mode) {
                if (st == ST_MENU) {
                    if (sellen < 4) { selbuf[sellen++] = (char)k; selbuf[sellen] = '\0'; }
                    show_image(IMG_MENU);
                    char l1[17];
                    snprintf(l1, sizeof(l1), "Enter Index:%-4.4s", selbuf);
                    lcd_print2(l1, "B to enter");

                    if (!index_timer_active && sellen > 0) {
                        index_timer_active = 1;
                        timer_start_or_reset();
                        timer_update_display(now_ms());
                    }
                } else if (st == ST_AMOUNT) {
                    if (amtlen < 3) { amtbuf[amtlen++] = (char)k; amtbuf[amtlen] = '\0'; }
                    char l1[17], l2[17];
                    snprintf(l1, sizeof(l1), "Enter amount:%-3.3s", amtbuf);
                    snprintf(l2, sizeof(l2), "Stock: %d", items[chosen_slot].stock);
                    lcd_print2(l1, l2);
                } else if (st == ST_PAY) {
                    if (k == '0') pay_zero_count++; else pay_zero_count = 0;

                    if (pay_zero_count >= 2) {
                        beep_payment_ok();
                        lcd_print2("Payment OK", "Dispensing...");

                        gDispAnim.oneshot_done = 0;
                        gDispAnim.active = 0;
                        st = ST_DISPENSING;
                        continue;
                    } else {
                        lcd_print2("Pay: enter 00", "Press 0 twice");
                    }
                }
            } else {
                /* ===== SERVICE MODE digits ===== */
                if (st == ST_SVC_MENU) {
                    if (sellen < 4) { selbuf[sellen++] = (char)k; selbuf[sellen] = '\0'; }
                    service_menu_screen(selbuf);
                } else {
                    if (svclen < 4) { svcbuf[svclen++] = (char)k; svcbuf[svclen] = '\0'; }

                    if (st == ST_SVC_DISPENSE_IDX) {
                        show_image(IMG_MENU_SERVICE);
                        char l1[17]; snprintf(l1, sizeof(l1), "Disp idx:%-4.4s", svcbuf);
                        lcd_print2(l1, "B=OK  A=Back");
                    } else if (st == ST_SVC_DISPENSE_AMT) {
                        show_image(IMG_MENU_SERVICE);
                        char l1[17]; snprintf(l1, sizeof(l1), "Amt:%-4.4s", svcbuf);
                        lcd_print2(l1, "B=Run A=Back");
                    } else if (st == ST_SVC_RESTOCK_IDX) {
                        show_image(IMG_RESTOCK);
                        char l1[17]; snprintf(l1, sizeof(l1), "Restock idx:%-4.4s", svcbuf);
                        lcd_print2(l1, "B=OK  A=Back");
                    } else if (st == ST_SVC_RESTOCK_QTY) {
                        show_image(IMG_RESTOCK);
                        char l1[17]; snprintf(l1, sizeof(l1), "New stock:%-4.4s", svcbuf);
                        lcd_print2(l1, "B=OK  A=Back");
                    } else if (st == ST_SVC_SOUND_SEL) {
                        show_image(IMG_SOUND);
                        char l1[17]; snprintf(l1, sizeof(l1), "Sound 1-8:%-2.2s", svcbuf);
                        lcd_print2(l1, "B=Play A=Back");
                    } else if (st == ST_SVC_MOTOR_CYC) {
                        show_image(IMG_MOTOR);
                        char l1[17]; snprintf(l1, sizeof(l1), "Motor cyc:%-2.2s", svcbuf);
                        lcd_print2(l1, "B=Run A=Back");
                    }
                }
            }
            continue;
        }

        /* ENTER (B) */
        if (k == KEY_ENTER) {
            if (!service_mode) {
                if (st == ST_MENU) {
                    if (sellen == 0) {
                        beep_error();
                        lcd_print2("No index", "Type digits");
                        usleep(USLEEP_ERR_SHORT_US);
                        show_image(IMG_MENU);
                        lcd_print2("Enter Index:", "B to enter");
                        continue;
                    }

                    /* enter service: 1234 + B */
                    if (strcmp(selbuf, "1234") == 0) {
                        sellen = 0; selbuf[0] = '\0';
                        index_timer_active = 0;
                        timer_stop_and_blank();

                        show_service_gate_prompt();
                        usleep(120000);

                        set_port_mapping(1);
                        st = ST_SVC_GATE;
                        svc_gate_deadline = now_ms() + SVC_GATE_TIMEOUT_MS;
                        continue;
                    }

                    int idx = atoi(selbuf);
                    chosen_slot = find_slot_by_index(items, N, idx);
                    sellen = 0; selbuf[0] = '\0';
                    index_timer_active = 0;
                    timer_stop_and_blank();

                    if (chosen_slot < 0) {
                        beep_error();
                        lcd_print2("Invalid index", "Try 3/8/11/22");
                        usleep(USLEEP_ERR_LONG_US);
                        show_image(IMG_MENU);
                        lcd_print2("Enter Index:", "B to enter");
                        continue;
                    }

                    show_image(items[chosen_slot].img);

                    if (items[chosen_slot].stock <= 0) {
                        show_image(items[chosen_slot].img_oos);
                        lcd_print2(items[chosen_slot].name, "OUT OF STOCK");
                        usleep(USLEEP_OOS_SCREEN_US);
                        chosen_slot = -1;
                        show_image(IMG_MENU);
                        lcd_print2("Enter Index:", "B to enter");
                        continue;
                    }

                    st = ST_AMOUNT;
                    amtlen = 0; amtbuf[0] = '\0';
                    timer_start_or_reset();
                    timer_update_display(now_ms());

                    char l1[17], l2[17];
                    snprintf(l1, sizeof(l1), "Enter amount:%-3.3s", "");
                    snprintf(l2, sizeof(l2), "Stock: %d", items[chosen_slot].stock);
                    lcd_print2(l1, l2);
                    continue;
                }

                if (st == ST_AMOUNT) {
                    if (amtlen == 0) {
                        beep_error();
                        lcd_print2("No amount", "Type digits");
                        usleep(USLEEP_ERR_SHORT_US);
                        continue;
                    }

                    amount = atoi(amtbuf);
                    if (amount < 1 || amount > MAX_COUNT) {
                        beep_error();
                        lcd_print2("Amount must", "be 1-15");
                        usleep(USLEEP_ERR_SHORT_US);
                        amtlen = 0; amtbuf[0] = '\0';
                        continue;
                    }
                    if (amount > items[chosen_slot].stock) {
                        beep_error();
                        lcd_print2("Insufficient", "stock");
                        usleep(USLEEP_ERR_SHORT_US);
                        amtlen = 0; amtbuf[0] = '\0';
                        continue;
                    }

                    total = items[chosen_slot].price * (float)amount;
                    char total_s[12];
                    format_money(total_s, total);

                    char l1[17], l2[17];
                    snprintf(l1, sizeof(l1), "Total %s", total_s);
                    snprintf(l2, sizeof(l2), "Pay: enter 00");
                    lcd_print2(l1, l2);

                    st = ST_PAY;
                    pay_zero_count = 0;
                    timer_start_or_reset();
                    timer_update_display(now_ms());
                    continue;
                }

            } else {
                /* ===== SERVICE MODE ENTER ===== */

                /* From service menu -> enter an option / exit */
                if (st == ST_SVC_MENU) {
                    if (strcmp(selbuf, "1234") == 0) {
                        sellen = 0; selbuf[0] = '\0';

                        show_return_gate_prompt();
                        usleep(120000);

                        set_port_mapping(0);
                        st = ST_RETURN_GATE;
                        return_gate_deadline = now_ms() + RETURN_GATE_TIMEOUT_MS;
                        continue;
                    }

                    if (strcmp(selbuf, "1") == 0) {
                        svclen = 0; svcbuf[0] = '\0';
                        st = ST_SVC_DISPENSE_IDX;
                        show_image(IMG_MENU_SERVICE);
                        lcd_print2("Disp idx:", "B=OK  A=Back");
                    } else if (strcmp(selbuf, "2") == 0) {
                        svclen = 0; svcbuf[0] = '\0';
                        st = ST_SVC_RESTOCK_IDX;
                        show_image(IMG_RESTOCK);
                        lcd_print2("Restock idx:", "B=OK  A=Back");
                    } else if (strcmp(selbuf, "3") == 0) {
                        svclen = 0; svcbuf[0] = '\0';
                        st = ST_SVC_SOUND_SEL;
                        show_image(IMG_SOUND);
                        lcd_print2("Sound 1-8:", "B=Play A=Back");
                    } else if (strcmp(selbuf, "4") == 0) {
                        svclen = 0; svcbuf[0] = '\0';
                        st = ST_SVC_MOTOR_CYC;
                        show_image(IMG_MOTOR);
                        lcd_print2("Motor cyc 1-15", "B=Run A=Back");
                    } else {
                        beep_error();
                        lcd_print2("Invalid choice", "Use 1-4 or 1234");
                        usleep(USLEEP_ERR_SHORT_US);
                        service_menu_screen(selbuf);
                    }

                    sellen = 0; selbuf[0] = '\0';
                    continue;
                }

                /* ---- service dispense idx confirm ---- */
                if (st == ST_SVC_DISPENSE_IDX) {
                    if (svclen == 0) {
                        beep_error();
                        lcd_print2("No index", "Type digits");
                        usleep(USLEEP_ERR_SHORT_US);
                        continue;
                    }
                    int idx = atoi(svcbuf);
                    svc_disp_slot = find_slot_by_index(items, N, idx);
                    if (svc_disp_slot < 0) {
                        beep_error();
                        lcd_print2("Bad idx", "Try 3/8/11/22");
                        usleep(USLEEP_ERR_SHORT_US);
                        svclen = 0; svcbuf[0] = '\0';
                        show_image(IMG_MENU_SERVICE);
                        lcd_print2("Disp idx:", "B=OK  A=Back");
                        continue;
                    }

                    show_image(items[svc_disp_slot].img);
                    svclen = 0; svcbuf[0] = '\0';
                    st = ST_SVC_DISPENSE_AMT;
                    lcd_print2("Amount 1-15:", "B=Run A=Back");
                    continue;
                }

                /* ---- service dispense amount confirm ---- */
                if (st == ST_SVC_DISPENSE_AMT) {
                    if (svclen == 0) {
                        beep_error();
                        lcd_print2("No amount", "Type digits");
                        usleep(USLEEP_ERR_SHORT_US);
                        continue;
                    }
                    int a = atoi(svcbuf);
                    if (a < 1 || a > 15) {
                        beep_error();
                        lcd_print2("Amount 1-15", "Try again");
                        usleep(USLEEP_ERR_SHORT_US);
                        svclen = 0; svcbuf[0] = '\0';
                        show_image(IMG_MENU_SERVICE);
                        lcd_print2("Amount 1-15:", "B=Run A=Back");
                        continue;
                    }

                    lcd_print2("Service Disp", "Dispensing...");

                    gDispAnim.oneshot_done = 0;
                    gDispAnim.active = 0;
                    anim_start(&gDispAnim, disp_frames, DISP_N, +1, DISP_FRAME_MS);

                    for (int i = 0; i < a; i++) {
                        run_one_dispense_cycle_with_anim();
                        if (i != a - 1) usleep(150000);
                    }
                    while (!gDispAnim.oneshot_done) { anim_tick(&gDispAnim); usleep(20000); }

                    beep_success();
                    lcd_print2("Service Done", "A=Back");
                    usleep(USLEEP_SVC_DONE_US);

                    st = ST_SVC_MENU;
                    svclen = 0; svcbuf[0] = '\0';
                    svc_disp_slot = -1;
                    service_menu_screen(selbuf);
                    continue;
                }

                /* ---- restock index confirm ---- */
                if (st == ST_SVC_RESTOCK_IDX) {
                    if (svclen == 0) {
                        beep_error();
                        lcd_print2("No index", "Type digits");
                        usleep(USLEEP_ERR_SHORT_US);
                        continue;
                    }
                    int idx = atoi(svcbuf);
                    restock_slot = find_slot_by_index(items, N, idx);
                    if (restock_slot < 0) {
                        beep_error();
                        lcd_print2("Bad idx", "Try 3/8/11/22");
                        usleep(USLEEP_ERR_SHORT_US);
                        svclen = 0; svcbuf[0] = '\0';
                        show_image(IMG_RESTOCK);
                        lcd_print2("Restock idx:", "B=OK  A=Back");
                        continue;
                    }

                    /* prompt for NEW stock (1..15) */
                    show_image(items[restock_slot].img);
                    svclen = 0; svcbuf[0] = '\0';
                    st = ST_SVC_RESTOCK_QTY;
                    lcd_print2("New stock 1-15", "B=OK  A=Back");
                    continue;
                }

                /* ---- restock qty confirm ---- */
                if (st == ST_SVC_RESTOCK_QTY) {
                    if (svclen == 0) {
                        beep_error();
                        lcd_print2("No stock", "Type 1-15");
                        usleep(USLEEP_ERR_SHORT_US);
                        continue;
                    }
                    int newstock = atoi(svcbuf);
                    if (newstock < 1 || newstock > 15) {
                        beep_error();
                        lcd_print2("Stock must", "be 1-15");
                        usleep(USLEEP_ERR_SHORT_US);
                        svclen = 0; svcbuf[0] = '\0';
                        show_image(IMG_RESTOCK);
                        lcd_print2("New stock 1-15", "B=OK  A=Back");
                        continue;
                    }

                    items[restock_slot].stock = newstock;

                    beep_success();
                    char l2[17];
                    snprintf(l2, sizeof(l2), "Stock=%d", newstock);
                    lcd_print2("Restocked", l2);
                    usleep(USLEEP_SVC_DONE_US);

                    st = ST_SVC_MENU;
                    restock_slot = -1;
                    svclen = 0; svcbuf[0] = '\0';
                    service_menu_screen(selbuf);
                    continue;
                }

                /* ---- sound selection confirm (stay in sound select) ---- */
                if (st == ST_SVC_SOUND_SEL) {
                    if (svclen == 0) {
                        beep_error();
                        lcd_print2("Pick 1-8", "Type digit");
                        usleep(USLEEP_ERR_SHORT_US);
                        continue;
                    }
                    int s = atoi(svcbuf);
                    if (s < 1 || s > 8) {
                        beep_error();
                        lcd_print2("Sound must", "be 1-8");
                        usleep(USLEEP_ERR_SHORT_US);
                        svclen = 0; svcbuf[0] = '\0';
                        show_image(IMG_SOUND);
                        lcd_print2("Sound 1-8:", "B=Play A=Back");
                        continue;
                    }

                    lcd_print2("Playing...", "Please wait");
                    usleep(1000000);

                    /* Correct mapping per your image */
                    if (s == 1) beep_keypress();
                    else if (s == 2) beep_error();
                    else if (s == 3) beep_success();
                    else if (s == 4) beep_payment_ok();
                    else if (s == 5) beep_dispensing_slot(1);
                    else if (s == 6) beep_dispensing_slot(2);
                    else if (s == 7) beep_dispensing_slot(3);
                    else if (s == 8) beep_dispensing_slot(4);

                    /* stay in sound selection */
                    svclen = 0; svcbuf[0] = '\0';
                    show_image(IMG_SOUND);
                    lcd_print2("Sound 1-8:", "B=Play A=Back");
                    continue;
                }

                /* ---- motor cycles confirm (stay in motor screen) ---- */
                if (st == ST_SVC_MOTOR_CYC) {
                    if (svclen == 0) {
                        beep_error();
                        lcd_print2("No cycles", "Type 1-15");
                        usleep(USLEEP_ERR_SHORT_US);
                        continue;
                    }
                    int cycles = atoi(svcbuf);
                    if (cycles < 1 || cycles > 15) {
                        beep_error();
                        lcd_print2("Cycles must", "be 1-15");
                        usleep(USLEEP_ERR_SHORT_US);
                        svclen = 0; svcbuf[0] = '\0';
                        show_image(IMG_MOTOR);
                        lcd_print2("Motor cyc 1-15", "B=Run A=Back");
                        continue;
                    }

                    lcd_print2("Motor test", "Running...");
                    run_motor_test_cycles(cycles);
                    beep_success();

                    /* stay here for more tests */
                    svclen = 0; svcbuf[0] = '\0';
                    show_image(IMG_MOTOR);
                    lcd_print2("Motor cyc 1-15", "B=Run A=Back");
                    continue;
                }
            }
        }

        /* anything else */
        beep_error();
    }
}

/* ===== LCD low-level ===== */
static void initlcd(void)
{
    usleep(20000);
    lcd_writecmd(0x30);
    usleep(20000);
    lcd_writecmd(0x30);
    usleep(20000);
    lcd_writecmd(0x30);

    lcd_writecmd(0x02);
    lcd_writecmd(0x28);
    lcd_writecmd(0x01);
    lcd_writecmd(0x0c);
    lcd_writecmd(0x06);
    lcd_writecmd(0x80);
}

static void lcd_writecmd(char cmd)
{
    char data;
    data = (cmd & 0xf0);
    CM3_outport(gLcdPort, data | 0x04);
    usleep(10);
    CM3_outport(gLcdPort, data);
    usleep(200);

    data = (cmd & 0x0f) << 4;
    CM3_outport(gLcdPort, data | 0x04);
    usleep(10);
    CM3_outport(gLcdPort, data);
    usleep(2000);
}

static void LCDprint(char *sptr)
{
    while (*sptr) lcddata(*sptr++);
}

static void lcddata(unsigned char cmd)
{
    char data;
    data = (cmd & 0xf0);
    CM3_outport(gLcdPort, data | 0x05);
    usleep(10);
    CM3_outport(gLcdPort, data);
    usleep(200);

    data = (cmd & 0x0f) << 4;
    CM3_outport(gLcdPort, data | 0x05);
    usleep(10);
    CM3_outport(gLcdPort, data);
    usleep(2000);
}
