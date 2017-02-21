/*   Virtual Nascom, a Nascom II emulator.

     Copyright (C) 2000,2009  Tommy Thorn

     Z80 emulator portition Copyright (C) 1995,1998 Frank D. Cringle.

     NasEmu is free software; you can redistribute it and/or modify it
     under the terms of the GNU General Public License as published by
     the Free Software Foundation; either version 2 of the License, or
     (at your option) any later version.

     This program is distributed in the hope that it will be useful,
     but WITHOUT ANY WARRANTY; without even the implied warranty of
     MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
     General Public License for more details.

     You should have received a copy of the GNU General Public License
     along with this program; if not, write to the Free Software
     Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA
     02111-1307, USA.


   A Nascom consists of:

    - a Z80 CPU,
    - an UART,
    - a bitmapped keyboard,
    - memory:
        0000 - 07ff  2 KB ROM monitor,
        0800 - 0bff  1 KB screen memory,
        0c00 - 0fff  1 KB workspace
        1000 - dfff       memory
        e000 - ffff  8 KB of MS Basic

  With the Z80 emulator in place the first thing to get working is the
  screen memory.  The "correct" way to simulate screen memory is to
  trap upon writes, but that would be slow.  We do it any just to get
  started.
*/

#include <stdio.h>
#include <stdlib.h>
#include <getopt.h>
#include <ctype.h>
#include "simz80.h"
#include "nascom.h"
#include "ihex.h"
#include <SDL.h>


static bool go_fast = false;
static int t_sim_delay = 900;


#define FONT_H_PITCH 16
#define FONT_H       15
#define FONT_W        8

extern uint8_t nascom_font_raw[];

static SDL_Surface *screen;
static struct font {
    SDL_Surface *surf;
    int w, h, h_pitch;
} nascom_font;

static FILE *serial_out, *serial_in;
static int tape_led = 0;
static int serial_input_available = 0;

static void RenderItem(struct font *font, int idx, int x, int y)
{
    auto SDL_Rect dest = { x, y, font->w, font->h };
    SDL_Rect clip = { 0, idx * font->h_pitch, font->w, font->h };
    SDL_BlitSurface(font->surf, &clip, screen, &dest);
}

/* The keyboard holds the state state of every depressed key and a
   current scanning pointer. */

static struct {
    unsigned char mask[9];
    unsigned char index;
} keyboard = {
    {
        0,  /*     ? Sh ?  ?  ?  ?  */
        0,  /* Up T  X  F  5  B  H  */
        0,  /* Lt Y  Z  D  6  N  J  */
        0,  /* Dn U  S  E  7  M  K  */
        0,  /* Rt I  A  W  8  ,  L  */
        0,  /* Gr O  Q  3  9  .  ;  */
        0,  /* [  P  1  2  0  /  :  */
        0,  /* ]  R  Sp C  4  V  G  */
        0   /* Ch @  Sh Ct -  Nl Bs */
    },
    0};

/*
 * To avoid race conditions and behaving in a manner that couldn't
 * happen realistically, we allow only a single bit transition per
 * scan.  Thus we queue up all the transactions and apply one per full
 * keyboard scan.
 */

#define MAX_KEYBOARD_EVENTS 16

static struct {
    struct {
        unsigned char *addr, value;
    } e[MAX_KEYBOARD_EVENTS], *rp, *wp;
} keyboard_events;

unsigned char ui_mask[9];

static char * kbd_translation[] = {
/* 0 */  "________",
/* 1 */  "__TXF5BH",
/* 2 */  "__YZD6NJ",
/* 3 */  "__USE7MK",
/* 4 */  "__IAW8,L",
/* 5 */  "__OQ39.;",
/* 6 */  "_[P120/:",
/* 7 */  "_]R C4VG",
/* 8 */  "_\t@__-\r\010"
};

#define ___ " " // Dummy
static const char
kbd_us_shift[] = ";:'\"[{]}\\|-_=+`~1!2@3#4$5%6^7&8*9(0),<.>/?";
static const char
kbd_spec          [] = ";" ":" "["    "]" "-" "," "." "/"    "0" "1" "2"  "3" "4" "5" "6" "7" "8" "9" " ";
static const char
kbd_spec_w_shift  [] = "+" "*" "\\"   "_" "=" "<" ">" "?"    "^" "!" "\"" "#" "$" "%" "&" "'" "(" ")" ___;
static const char
kbd_spec_w_ctrl   [] = "{" ___ "\033" ___ ___ ___ ___ ___    ___ ___ ___  ___ ___ ___ ___ ___ ___ ___ "`";
static const char
kbd_spec_w_shctrl [] = ___ ___ ___    ___ "}" "|" "~" "\177" ___ ___ ___  ___ ___ ___ ___ ___ ___ ___ ___;

static int reset = 0;

// Ctr-Shift-Meta 0 -> the REAL # (instead of the pound symbol)
// Ctrl-Space -> `

static void handle_key_event_dwim(SDL_keysym keysym, bool keydown);
static void handle_key_event_raw(SDL_keysym keysym, bool keydown);

static void (*handle_key_event)(SDL_keysym, bool) = handle_key_event_dwim;

static void handle_app_control(SDL_keysym keysym, bool keydown)
{
    if (keydown)
        switch (keysym.sym) {
        case SDLK_END: {
            FILE *f;
            f = fopen("screendump", "a+");
            fwrite((const void *) (ram+0x800), 1, 1024, f);
            fclose(f);
            if (verbose) printf("Screen dumped\n");
            break;
        }

        case SDLK_F4:
            exit(0);

        case SDLK_F5:
            go_fast = !go_fast;
            printf("Switch to %s\n", go_fast ? "fast" : "slow");

            t_sim_delay = go_fast ? 900000 : 900;
            break;

        case SDLK_F9:
            reset = 1;
            break;

        case SDLK_F10:
            if (handle_key_event == handle_key_event_raw)
                handle_key_event = handle_key_event_dwim;
            else
                handle_key_event = handle_key_event_raw;

            printf("Switch to %s keyboard\n",
                   handle_key_event == handle_key_event_raw ? "raw" : "dwim");
            break;

        default:
            ;
        }
}

static void handle_key_event_dwim(SDL_keysym keysym, bool keydown)
{
    int i = -1, bit = 0;
    static bool ui_shift = false;
    static bool ui_ctrl  = false;
    static bool ui_graph = false;
    bool emu_shift = false;
    bool emu_ctrl  = false;
    bool emu_graph = false;
    int ch = toupper(keysym.sym);

    /* We are getting raw key code events, so first we need to handle the UI a bit */

    switch (keysym.sym) {
    case SDLK_LSHIFT:
    case SDLK_RSHIFT:
        ui_shift = keydown;
        return;
    case SDLK_LCTRL:
    case SDLK_RCTRL:
        ui_ctrl = keydown;
        return;
    case SDLK_RMETA:
    case SDLK_LMETA:
    case SDLK_RALT:
    case SDLK_LALT:
        ui_graph = keydown;
        return;
    default:
        break;
    }

    emu_shift = !ui_shift & isalpha(keysym.sym);
    emu_ctrl  = ui_ctrl;
    emu_graph = ui_graph;

    if (ui_shift)
        for (int i = 0; kbd_us_shift[i]; i += 2) {
            if (kbd_us_shift[i] == ch) {
                ch = kbd_us_shift[i+1];
                break;
            }
        }

    /* Now translate the ASCII to Nascom keyboard events */

    // Quick hack to enable LF (which is shift-CH)
    if (ch == '\t' && ui_shift) {
        emu_shift = true;
    }

#if 0
    // HACKS THAT TECHNICALLY BREAK FAITHFUL EMULATION
    // The gylphs for '#' is £ but there exist a gylph that looks right
    if (ch == '#') {
        ch = '0';
        emu_shift = true;
        emu_ctrl  = true;
        emu_graph = true;
        goto search;
    }

    // The glyph for '^' is ↑ but there exist a gylph that looks right
    if (ch == '^') {
        emu_shift = false;
        emu_ctrl  = false;
        emu_graph = true;
        ch = '3';
        goto search;
    }

    // Other things: | and ~ doesn't look quite like their modern versions,
    // but there doesn't appear to be a good alternative
#endif

    // ' ' has to be special cased because it's the NOT_ITEM above
    if (ch == ' ')
        goto search;


    // '@' is also special
    if (ch == '@') {
        emu_shift = true;
        goto search;
    }

    for (int i = 0; kbd_spec_w_shift[i]; ++i)
        if (kbd_spec_w_shift[i] == ch) {
            emu_shift = true;
            ch = kbd_spec[i];
            goto search;
        }

    for (int i = 0; kbd_spec_w_ctrl[i]; ++i)
        if (kbd_spec_w_ctrl[i] == ch) {
            emu_shift = false;
            emu_ctrl = true;
            ch = kbd_spec[i];
            goto search;
        }

    for (int i = 0; kbd_spec_w_shctrl[i]; ++i)
        if (kbd_spec_w_shctrl[i] == ch) {
            emu_shift = true;
            emu_ctrl = true;
            ch = kbd_spec[i];
            goto search;
        }

    search:
    if (keysym.sym < 128) {
        for (i = 1; i < 9; ++i)
            for (bit = 0; bit < 7; ++bit)
                if (kbd_translation[i][7-bit] == ch) {
                    goto translate;
                }

        i = -1;
    } else {
        emu_shift = ui_shift;

        switch (keysym.sym) {
        case SDLK_UP:      i = 1, bit = 6; break;
        case SDLK_LEFT:    i = 2, bit = 6; break;
        case SDLK_DOWN:    i = 3, bit = 6; break;
        case SDLK_RIGHT:   i = 4, bit = 6; break;
        default:
            handle_app_control(keysym, keydown);
        }
    }

translate:
    if (emu_shift)
        ui_mask[0] |= 1 << 4;
    else
        ui_mask[0] &= ~(1 << 4);

    if (emu_ctrl)
        ui_mask[0] |= 1 << 3;
    else
        ui_mask[0] &= ~(1 << 3);

    if (emu_graph)
        ui_mask[5] |= 1 << 6;
    else
        ui_mask[5] &= ~(1 << 6);

    if (i != -1) {
        if (keydown)
            ui_mask[i] |= 1 << bit;
        else
            ui_mask[i] &= ~(1 << bit);
    }
}

static void handle_key_event_raw(SDL_keysym keysym, bool keydown)
{
    int i = -1, bit = 0;

    // We need this to be able to share kbd_translation between the
    // RAW and the DWIM variant
    if (keysym.sym == '\'')
        keysym.sym = ':';

    if (keysym.sym < 128) {
        int ch = toupper(keysym.sym);
        for (i = 1; i < 9; ++i)
            for (bit = 0; bit < 7; ++bit)
                if (kbd_translation[i][7-bit] == ch) {
                    goto translate;
                }
        i = -1;
        translate:;
    } else {
        switch (keysym.sym) {
        // case Newline  i = 0, bit = 5; break;
        // case '@':     i = 8, bit = 5; break;
        case SDLK_LCTRL:
        case SDLK_RCTRL:   i = 0, bit = 3; break;

        case SDLK_LSHIFT:
        case SDLK_RSHIFT:  i = 0, bit = 4; break;

        case SDLK_UP:      i = 1, bit = 6; break;
        case SDLK_LEFT:    i = 2, bit = 6; break;
        case SDLK_DOWN:    i = 3, bit = 6; break;
        case SDLK_RIGHT:   i = 4, bit = 6; break;

        case SDLK_LMETA:
        case SDLK_RMETA:
        case SDLK_LALT:
        case SDLK_RALT:    i = 5, bit = 6; break;

        case SDLK_KP_ENTER:i = 8, bit = 6; break;
        default:
            handle_app_control(keysym, keydown);
        }
    }

    if (i != -1) {
        if (keydown)
            ui_mask[i] |= 1 << bit;
        else
            ui_mask[i] &= ~(1 << bit);
    }
}

static void load_nascom(const char *file)
{
    FILE *f = fopen(file, "r");
    int a, b1, b2, b3, b4, b5, b6, b7, b8;
    int count = 0;
    int ch;

    if (!f) {
        perror(file);
        exit(1);
    }

    if (verbose)
        printf("Loading %s\n", file);

    ch = fgetc(f);
    rewind(f);

    if (ch == ':') {
        load_ihex(file, &RAM(0));
        return;
    }

    for (; !feof(f) ;) {
        if (fscanf(f, "%x %x %x %x %x %x %x %x %x",
                   &a, &b1, &b2, &b3, &b4, &b5, &b6, &b7, &b8) == 9) {
            RAM(a)   = b1;
            RAM(a+1) = b2;
            RAM(a+2) = b3;
            RAM(a+3) = b4;
            RAM(a+4) = b5;
            RAM(a+5) = b6;
            RAM(a+6) = b7;
            RAM(a+7) = b8;
            count += 8;
        }

        do
            ch = fgetc(f);
        while (ch != -1 && ch != '\n');

        if (ch == -1)
            break;
    }

    fclose(f);
    if (verbose)
        printf(". Successfully loaded %d bytes\n", count);
}

static void uiloop(void)
{
    static uint8_t screencache[1024];

    memset(screencache, ' ', sizeof screencache);

    for (;;) {
        SDL_Event event;

        while (SDL_PollEvent(&event)) {
            switch (event.type) {
            case SDL_MOUSEMOTION:
                /*printf("Mouse moved by %d,%d to (%d,%d)\n",
                  event.motion.xrel, event.motion.yrel,
                  event.motion.x, event.motion.y);*/
                break;
            case SDL_MOUSEBUTTONDOWN:
                /*printf("Mouse button %d pressed at (%d,%d)\n",
                  event.button.button, event.button.x, event.button.y);*/
                break;
            case SDL_KEYDOWN:
            case SDL_KEYUP:
                handle_key_event(event.key.keysym, event.type == SDL_KEYDOWN);
                break;
            case SDL_QUIT:
                //printf("Quit\n");
                return;
            default:
                //printf("Unknown event: %d\n", event.type);
                break;
            }
        }

        bool dirty = false;

        for (uint8_t *p0 = ram + 0x80A, *q0 = screencache + 0xA;
             p0 < ram + 0xC00; p0 += 64, q0 += 64)
            for (unsigned char *p = p0, *q = q0; p < p0 + 48; ++p, ++q)
                if (*q != *p) {
                    *q = *p;
                    unsigned index = p - ram - 0x800;
                    unsigned x     = index % 64 - 10;
                    unsigned y     = index / 64;
                    y = (y + 1) % 16; // The last line is the first line

                    RenderItem(&nascom_font, *p, x * FONT_W, y * FONT_H);
                    dirty = true;
                }

        if (dirty)
            // SDL_UpdateRect(screen, 0, 0, screen->w, screen->h);
            SDL_Flip(screen); // either seem to work

        if (go_fast)
            SDL_Delay(1000 / 30); // 30 fps
        else
            SDL_Delay(1000 / 10); // 10 fps
    }
}

static int sim_delay()
{
    if (reset) {
        reset = 0;
        return 1;
    }

    if (!go_fast)
        SDL_Delay(1);

    return 0;
}

static void simulate(void *dummy)
{
    simz80(pc, t_sim_delay, sim_delay);
}

static void
usage(void)
{
    fprintf(stderr,
 "This is Virtual Nascom.  Usage: %s {flags} files\n"
 "           -i <file>       take serial port input from file (if tape led is on)\n"
 "           -m <file>       use <file> as monitor (default is nassys3.nal)\n"
 "           -v              be verbose\n"
            ,progname);
    exit (1);
}

static int mysetup(int argc, char **argv);

int main(int argc, char **argv)
{
    int c;

    serial_out = fopen("serialout.txt", "a+");

    if (!serial_out)
        exit(3);

    if (mysetup(argc, argv))
        return 1;

    monitor = "nassys3.nal";
    progname = argv[0];


#ifdef MMU
    for (c=0; c<MEMSIZE/4; ++c) pagetable[c]=ram+(c<<12);
#endif

    while ((c = getopt(argc, argv, "i:m:v")) != EOF)
        switch (c) {
        case 'i':
            serial_in = fopen(optarg, "r");
            //printf("serial input %s -> %p\n", optarg, serial_in);
            serial_input_available = !feof(serial_in);
            break;
        case 'm':
            monitor = optarg;
            break;
        case 'v':
            verbose = 1;
            break;
        case '?':
            usage();
        }

    if (verbose)
        puts("Virtual Nascom, a Nascom 2 emulator version " VERSION "\n"
             "Copyright (C) 2000-2017 Tommy Thorn.\n"
             "Uses software from \n"
             "Yet Another Z80 Emulator version " YAZEVERSION
             ", Copyright (C) 1995,1998 Frank D. Cringle.\n"
             "VirtualNascom comes with ABSOLUTELY NO WARRANTY; for details\n"
             "see the file \"COPYING\" in the distribution directory.\n");

    load_nascom(monitor);
    load_nascom("basic.nal");

    for (; optind < argc; optind++)
        load_nascom(argv[optind]);

    ram[0x10000] = ram[0]; // Make GetWord[0xFFFF) work correctly

    SDL_CreateThread((int (*)(void *))simulate, NULL);
    uiloop();

    exit(0);
}

/*
 * 1.7 Input/output port addressing
 *
 *     Output Bit
 * P0  7 Not available          7 Unused
 *     6 Not used               6 Keyboard S6
 *     5 Unused                 5 Keyboard S3
 *     4 Tape drive LED         4 Keyboard S5
 *     3 Single step            3 Keyboard S4
 *     2 Unused                 2 Keyboard S0
 *     1 Reset keyb'd count     1 Keyboard S2
 *     0 Clock keyb'd count     0 Keyboard S1
 */

#define P0_OUT_TAPE_DRIVE_LED 16
#define P0_OUT_SINGLE_STEP     8
#define P0_OUT_KEYBOARD_RESET  2
#define P0_OUT_KEYBOARD_CLOCK  1

/*
 * P1  0 - 7 Data to UART       0 - 7 Data from UART
 *     (Serial port)            (Serial port)
 *
 * P2  0 - 7 Not assigned       7 Data received from UART
 *                              6 UART TBR empty
 *                              5 Not assigned
 *                              4 Not assigned
 *                              3 F error on UART
 *                              2 P error on UART
 *                              1 O error on UART
 *                              0 Not assigned
 */

#define UART_DATA_READY 128
#define UART_TBR_EMPTY   64
#define UART_F_ERROR      8
#define UART_P_ERROR      4
#define UART_O_ERROR      2

/*
 * P3  Not assigned             Not assigned
 *
 * P4  PIO port A data input and output
 *
 * P5  PIO port B data input and output
 *
 * P6  PIO port A control
 *
 * P7  PIO port B control
 */

void out(unsigned int port, unsigned char value)
{
    static unsigned char port0;

    unsigned int down_trans;

    if (0) fprintf(stdout, "[%02x] <- %02x\n", port, value);

    switch (port) {
    case 0:
        /* KBD */
        down_trans = port0 & ~value;
        port0 = value;

        if ((down_trans & P0_OUT_KEYBOARD_CLOCK) && keyboard.index < 9)
            keyboard.index++;
        if (down_trans & P0_OUT_KEYBOARD_RESET) {
            keyboard.index = 0;

            memcpy(keyboard.mask, ui_mask, sizeof ui_mask); // XXX still not enough to avoid races

            /* Issue one change from the queue */
            if (keyboard_events.rp != keyboard_events.wp) {
                *keyboard_events.rp->addr = keyboard_events.rp->value;
                ++keyboard_events.rp;
                if (keyboard_events.rp == keyboard_events.e + MAX_KEYBOARD_EVENTS)
                    keyboard_events.rp = keyboard_events.e;
            }

            // Since we simulate Z-80 at max speed, we need to slow down here for keyboard
            // input to be even possible
            SDL_Delay(go_fast ? 0 : 2);
        }
#if 0
        if (tape_led != !!(value & P0_OUT_TAPE_DRIVE_LED))
            fprintf(stderr, "Tape LED = %d\n", !!(value & P0_OUT_TAPE_DRIVE_LED));
#endif
        tape_led = !!(value & P0_OUT_TAPE_DRIVE_LED);
        break;

    case 1:
        fputc(value, serial_out);
        break;

    default:
        if (verbose)
            fprintf(stdout, "OUT [%02x] <- %02x\n", port, value);
    }
}

int in(unsigned int port)
{
    if (0) fprintf(stdout, "<- [%02x]\n", port);

    switch (port) {
    case 0:
        /* KBD */
        /* printf("[%d]", keyboard.index); */
        return ~keyboard.mask[keyboard.index];
    case 1:
        if (serial_input_available & tape_led) {
            char ch = fgetc(serial_in);
            serial_input_available = !feof(serial_in);
            return ch;
        }
        else
            return 0;
    case 2:
        /* Status port on the UART */
        return UART_TBR_EMPTY |
            (serial_input_available & tape_led ? UART_DATA_READY : 0);
    default:
        if (verbose)
            fprintf(stdout, "IN <- [%02x]\n", port);
        return 0;
    }
}

static int mysetup(int argc, char **argv)
{
    if (SDL_Init(SDL_INIT_VIDEO) < 0) {
        fprintf(stderr, "Unable to init SDL: %s\n", SDL_GetError());
        return 1;
    }

    keyboard_events.rp = keyboard_events.wp = keyboard_events.e;

    atexit(SDL_Quit);


    screen = SDL_SetVideoMode(48 * FONT_W, 16 * FONT_H, 8, SDL_SWSURFACE);
    if (screen == NULL) {
        fprintf(stderr, "Unable to set video: %s\n", SDL_GetError());
        return 1;
    }

    /* Set the window caption */
    SDL_WM_SetCaption("Nascom II", "Nascom II");

#if 0
    /* Populate the palette */
    SDL_Color colors[256];

    colors[0].r = colors[0].g = colors[0].b = 0;
    colors[255].r = colors[255].b = 0;
    colors[255].g = 255;

    /* Set palette */
    if (!SDL_SetColors(screen, colors, 0, 256)) {
        fprintf(stderr, "Unable to create framebuffer palette: %s\n",
                SDL_GetError());
        screen = 0; //XXX should free it
        return 1;
    }
#endif

    /* Load font */
    nascom_font.surf =
        SDL_CreateRGBSurfaceFrom(
                nascom_font_raw,
                8 /* width */,
           256*16 /* height */,
                1 /* depth */,
                1 /* pitch */,
                0 /* Rmask */,
                1 /* Gmask */,
                0 /* Bmask */,
                0 /* Amask */);
    nascom_font.w = FONT_W;
    nascom_font.h = FONT_H;
    nascom_font.h_pitch = FONT_H_PITCH;

    if (!nascom_font.surf) {
        perror("Couldn't load the font\n");
        return 1;
    }

    nascom_font.surf = SDL_DisplayFormat(nascom_font.surf);

    return 0;
}
