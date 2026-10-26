/*
 * ============================================================================
 *  LinkLab : A Data Link Layer Simulator with Error-Detection Benchmarking
 *  Computer Networks (CN) Project
 * ----------------------------------------------------------------------------
 *  Syllabus coverage
 *    - Framing ............ Bit stuffing (HDLC flag 01111110), Byte stuffing
 *    - Error detection .... Parity finding (odd/even), VRC, LRC, CRC, Checksum
 *    - Noisy channel ...... Single / double / odd / burst error injection
 *    - Flow & error ctrl .. Stop-and-Wait, Go-Back-N, Selective Repeat ARQ
 *    - Analysis ........... Detection-rate benchmark and protocol efficiency
 *                           vs. loss rate (both exported to CSV)
 *
 *  Build :  gcc linklab.c -o linklab        (Linux / macOS / MinGW)
 *  Run   :  ./linklab            (Windows: linklab.exe)
 * ============================================================================
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#include <windows.h>
#endif

#define MAXB    4096        /* max bits in any buffer   */
#define MAXMSG  128         /* max message length       */
#define MAXFR   256         /* max frames in ARQ sims   */

/* ---------------------------------------------------------------- colours */
static int use_color = 1;
#define RED    "\033[1;31m"
#define GREEN  "\033[1;32m"
#define YELLOW "\033[1;33m"
#define BLUE   "\033[1;34m"
#define MAG    "\033[1;35m"
#define CYAN   "\033[1;36m"
#define DIM    "\033[2m"
#define RESET  "\033[0m"
static const char *c(const char *code) { return use_color ? code : ""; }

/* ---------------------------------------------------------------- input */
static void read_line(const char *prompt, char *buf, int n) {
    printf("%s", prompt);
    fflush(stdout);
    if (!fgets(buf, n, stdin)) { buf[0] = 0; return; }
    buf[strcspn(buf, "\r\n")] = 0;
}
static int read_int(const char *prompt, int def, int lo, int hi) {
    char b[64];
    read_line(prompt, b, sizeof b);
    if (!b[0]) return def;
    int v = atoi(b);
    if (v < lo) v = lo;
    if (v > hi) v = hi;
    return v;
}
static void read_msg(char *msg, const char *def) {
    char prompt[200];
    snprintf(prompt, sizeof prompt, "Enter message [default \"%s\"]: ", def);
    read_line(prompt, msg, MAXMSG);
    if (!msg[0]) strcpy(msg, def);
}
static void pause_key(void) { char b[8]; read_line("\nPress Enter to continue...", b, 8); }
static void header(const char *t) {
    printf("\n%s==================================================================%s\n", c(CYAN), c(RESET));
    printf("%s  %s%s\n", c(CYAN), t, c(RESET));
    printf("%s==================================================================%s\n", c(CYAN), c(RESET));
}

/* ---------------------------------------------------------------- bit helpers */
static int to_bits(const unsigned char *s, int len, char *out) {
    int k = 0;
    for (int i = 0; i < len; i++)
        for (int b = 7; b >= 0; b--) out[k++] = (char)(((s[i] >> b) & 1) + '0');
    out[k] = 0;
    return k;
}
static int from_bits(const char *bits, int nbits, unsigned char *out) {
    int n = nbits / 8;
    for (int i = 0; i < n; i++) {
        unsigned char v = 0;
        for (int b = 0; b < 8; b++) v = (unsigned char)((v << 1) | (bits[i * 8 + b] - '0'));
        out[i] = v;
    }
    out[n] = 0;
    return n;
}
static void append_bits(char *dst, unsigned v, int width) {
    int k = (int)strlen(dst);
    for (int b = width - 1; b >= 0; b--) dst[k++] = (char)(((v >> b) & 1) + '0');
    dst[k] = 0;
}
static int ones_in(const char *bits, int n) { int o = 0; for (int i = 0; i < n; i++) o += bits[i] == '1'; return o; }

/* mark[i]: 0 normal, 1 highlight (yellow), 2 flag/redundancy (magenta), 3 error (red) */
static void print_bits(const char *bits, const char *mark, int group) {
    int n = (int)strlen(bits);
    printf("  ");
    for (int i = 0; i < n; i++) {
        if (group && i && i % group == 0) printf(" ");
        int m = mark ? mark[i] : 0;
        if (m == 1) printf("%s%c%s", c(YELLOW), bits[i], c(RESET));
        else if (m == 2) printf("%s%c%s", c(MAG), bits[i], c(RESET));
        else if (m == 3) printf("%s%c%s", c(RED), bits[i], c(RESET));
        else putchar(bits[i]);
    }
    printf("   (%d bits)\n", n);
}

static int chance(int pct) { return (rand() % 100) < pct; }

/* ============================================================================
 *  1. FRAMING
 * ==========================================================================*/
static const char *FLAG_BITS = "01111110";

/* Bit stuffing: after five consecutive 1s insert a 0. Frame is FLAG data FLAG. */
static int bit_stuff(const char *in, char *out, char *mark) {
    int k = 0, ones = 0;
    memset(mark, 0, MAXB);
    for (int i = 0; i < 8; i++) { mark[k] = 2; out[k++] = FLAG_BITS[i]; }
    for (int i = 0; in[i]; i++) {
        out[k++] = in[i];
        if (in[i] == '1') {
            if (++ones == 5) { mark[k] = 1; out[k++] = '0'; ones = 0; }
        } else ones = 0;
    }
    for (int i = 0; i < 8; i++) { mark[k] = 2; out[k++] = FLAG_BITS[i]; }
    out[k] = 0;
    return k;
}
/* returns data length, or -1 on framing error */
static int bit_destuff(const char *in, char *out) {
    int n = (int)strlen(in), k = 0, ones = 0;
    if (n < 16 || strncmp(in, FLAG_BITS, 8) || strncmp(in + n - 8, FLAG_BITS, 8)) return -1;
    for (int i = 8; i < n - 8; i++) {
        if (ones == 5) {                 /* this bit must be a stuffed 0 */
            ones = 0;
            if (in[i] == '0') continue;
            return -1;                   /* six 1s inside data = framing error */
        }
        out[k++] = in[i];
        ones = in[i] == '1' ? ones + 1 : 0;
    }
    out[k] = 0;
    return k;
}

#define FLAG_BYTE 0x7E   /* '~' */
#define ESC_BYTE  0x7D   /* '}' */
static int byte_stuff(const unsigned char *in, int n, unsigned char *out, char *mark) {
    int k = 0;
    memset(mark, 0, MAXB);
    mark[k] = 2; out[k++] = FLAG_BYTE;
    for (int i = 0; i < n; i++) {
        if (in[i] == FLAG_BYTE || in[i] == ESC_BYTE) { mark[k] = 1; out[k++] = ESC_BYTE; }
        out[k++] = in[i];
    }
    mark[k] = 2; out[k++] = FLAG_BYTE;
    return k;
}
static int byte_destuff(const unsigned char *in, int n, unsigned char *out) {
    int k = 0;
    if (n < 2 || in[0] != FLAG_BYTE || in[n - 1] != FLAG_BYTE) return -1;
    for (int i = 1; i < n - 1; i++) {
        if (in[i] == ESC_BYTE) { i++; if (i >= n - 1) return -1; }
        out[k++] = in[i];
    }
    out[k] = 0;
    return k;
}
static void print_bytes(const unsigned char *b, int n, const char *mark) {
    printf("  ");
    for (int i = 0; i < n; i++) {
        const char *col = "";
        if (mark && mark[i] == 1) col = c(YELLOW);
        if (mark && mark[i] == 2) col = c(MAG);
        char ch = (b[i] >= 32 && b[i] < 127) ? (char)b[i] : '.';
        printf("%s[%c %02X]%s ", col, ch, b[i], c(RESET));
        if ((i + 1) % 8 == 0 && i + 1 < n) printf("\n  ");
    }
    printf("  (%d bytes)\n", n);
}

static void menu_bit_stuffing(void) {
    char msg[MAXMSG], data[MAXB], st[MAXB], mark[MAXB], back[MAXB];
    header("FRAMING : BIT STUFFING (HDLC)");
    char raw[MAXMSG];
    read_line("Enter a bit string, or leave empty to use text: ", raw, MAXMSG);
    int bitmode = raw[0] != 0;
    for (int i = 0; raw[i]; i++) if (raw[i] != '0' && raw[i] != '1') bitmode = 0;
    if (bitmode) strcpy(data, raw);
    else { read_msg(msg, "Hi~"); to_bits((unsigned char *)msg, (int)strlen(msg), data); }

    printf("\n%sOriginal data%s\n", c(BLUE), c(RESET));
    print_bits(data, NULL, 8);
    int n = bit_stuff(data, st, mark);
    int stuffed = 0;
    for (int i = 0; i < n; i++) stuffed += mark[i] == 1;
    printf("\n%sTransmitted frame%s  (%sflag%s / %sstuffed 0%s)\n", c(BLUE), c(RESET), c(MAG), c(RESET), c(YELLOW), c(RESET));
    print_bits(st, mark, 0);
    int m = bit_destuff(st, back);
    printf("\n%sReceiver after de-stuffing%s\n", c(BLUE), c(RESET));
    print_bits(back, NULL, 8);
    printf("\n  Stuffed bits inserted : %d\n", stuffed);
    printf("  Overhead              : %.1f%% (incl. 16 flag bits)\n", 100.0 * (n - (int)strlen(data)) / (double)strlen(data));
    printf("  Recovered correctly   : %s%s%s\n", (m >= 0 && strcmp(back, data) == 0) ? c(GREEN) : c(RED),
           (m >= 0 && strcmp(back, data) == 0) ? "YES" : "NO", c(RESET));
}

static void menu_byte_stuffing(void) {
    char msg[MAXMSG], mark[MAXB];
    unsigned char st[MAXB], back[MAXB];
    header("FRAMING : BYTE (CHARACTER) STUFFING");
    printf("  FLAG = '~' (0x7E)   ESC = '}' (0x7D)\n");
    read_msg(msg, "A~B}C");
    int n = (int)strlen(msg);
    printf("\n%sOriginal data%s\n", c(BLUE), c(RESET));
    print_bytes((unsigned char *)msg, n, NULL);
    int s = byte_stuff((unsigned char *)msg, n, st, mark);
    printf("\n%sTransmitted frame%s  (%sflag%s / %sESC inserted%s)\n", c(BLUE), c(RESET), c(MAG), c(RESET), c(YELLOW), c(RESET));
    print_bytes(st, s, mark);
    int m = byte_destuff(st, s, back);
    printf("\n%sReceiver after de-stuffing%s\n", c(BLUE), c(RESET));
    print_bytes(back, m, NULL);
    printf("\n  ESC bytes inserted : %d\n", s - n - 2);
    printf("  Recovered correctly: %s%s%s\n", (m == n && memcmp(back, msg, n) == 0) ? c(GREEN) : c(RED),
           (m == n && memcmp(back, msg, n) == 0) ? "YES" : "NO", c(RESET));
}

/* ============================================================================
 *  2. ERROR DETECTION
 * ==========================================================================*/
enum { M_VRC, M_LRC, M_CRC, M_CSUM, M_COUNT };
static const char *MNAME[M_COUNT] = { "VRC (even parity)", "LRC", "CRC", "Checksum-16" };
static char crc_gen[40] = "100000111";      /* CRC-8 : x^8 + x^2 + x + 1 */

/* modulo-2 division; rem gets (len(gen)-1) bits. verbose prints long division */
static void mod2_div(const char *dividend, const char *gen, char *rem, int verbose) {
    char work[MAXB];
    int n = (int)strlen(dividend), g = (int)strlen(gen);
    strcpy(work, dividend);
    if (verbose) printf("  %s   <- dividend\n", work);
    for (int i = 0; i + g <= n; i++) {
        if (work[i] != '1') continue;
        for (int j = 0; j < g; j++) work[i + j] = (work[i + j] == gen[j]) ? '0' : '1';
        if (verbose) {
            printf("  %*s%s%s%s   XOR divisor at bit %d\n", i, "", c(DIM), gen, c(RESET), i);
            printf("  ");
            for (int k = 0; k < n; k++) {
                if (k < i + g) putchar(k <= i ? ' ' : work[k]);
                else printf("%s%c%s", c(DIM), work[k], c(RESET));
            }
            printf("\n");
        }
    }
    memcpy(rem, work + n - (g - 1), g - 1);
    rem[g - 1] = 0;
}

static unsigned csum16(const char *bits, int n) {
    unsigned long sum = 0;
    for (int i = 0; i + 16 <= n; i += 16) {
        unsigned w = 0;
        for (int b = 0; b < 16; b++) w = (w << 1) | (unsigned)(bits[i + b] - '0');
        sum += w;
        sum = (sum & 0xFFFF) + (sum >> 16);           /* wrap-around carry */
    }
    return (unsigned)(sum & 0xFFFF);
}

/* Sender: build codeword from data bits (data must be a multiple of 8 bits). */
static int encode(int m, const char *data, char *cw, char *mark) {
    int n = (int)strlen(data), k = 0;
    if (mark) memset(mark, 0, MAXB);
    cw[0] = 0;
    switch (m) {
    case M_VRC:                                  /* parity bit after every byte */
        for (int i = 0; i < n; i += 8) {
            memcpy(cw + k, data + i, 8); k += 8;
            if (mark) mark[k] = 2;
            cw[k++] = (ones_in(data + i, 8) % 2) ? '1' : '0';
        }
        cw[k] = 0;
        break;
    case M_LRC: {                                /* column parity over the block */
        strcpy(cw, data); k = n;
        for (int col = 0; col < 8; col++) {
            int o = 0;
            for (int i = col; i < n; i += 8) o += data[i] == '1';
            if (mark) mark[k] = 2;
            cw[k++] = (o % 2) ? '1' : '0';
        }
        cw[k] = 0;
        break; }
    case M_CRC: {
        char dividend[MAXB], rem[40];
        int r = (int)strlen(crc_gen) - 1;
        strcpy(dividend, data);
        for (int i = 0; i < r; i++) dividend[n + i] = '0';
        dividend[n + r] = 0;
        mod2_div(dividend, crc_gen, rem, 0);
        strcpy(cw, data); strcat(cw, rem); k = n + r;
        if (mark) for (int i = n; i < k; i++) mark[i] = 2;
        break; }
    case M_CSUM: {
        strcpy(cw, data); k = n;
        while (k % 16) cw[k++] = '0';                /* pad to 16-bit words */
        cw[k] = 0;
        if (mark) for (int i = n; i < k; i++) mark[i] = 1;
        unsigned s = (~csum16(cw, k)) & 0xFFFF;
        append_bits(cw, s, 16);
        if (mark) for (int i = k; i < k + 16; i++) mark[i] = 2;
        k += 16;
        break; }
    }
    return k;
}

/* Receiver: returns 1 if the codeword is accepted (no error detected). */
static int check(int m, const char *cw) {
    int n = (int)strlen(cw);
    switch (m) {
    case M_VRC:
        for (int i = 0; i + 9 <= n; i += 9) if (ones_in(cw + i, 9) % 2) return 0;
        return 1;
    case M_LRC:
        for (int col = 0; col < 8; col++) {
            int o = 0;
            for (int i = col; i < n; i += 8) o += cw[i] == '1';
            if (o % 2) return 0;
        }
        return 1;
    case M_CRC: {
        char rem[40];
        mod2_div(cw, crc_gen, rem, 0);
        return strchr(rem, '1') == NULL; }
    case M_CSUM:
        return csum16(cw, n) == 0xFFFF;
    }
    return 0;
}

enum { E_NONE, E_SINGLE, E_DOUBLE, E_TRIPLE, E_BURST };
/* flips bits in cw according to pattern; marks flipped positions with 3 */
static void inject(char *cw, int pattern, int burst_len, char *mark) {
    int n = (int)strlen(cw), pos[3], cnt = 0;
    switch (pattern) {
    case E_SINGLE: cnt = 1; break;
    case E_DOUBLE: cnt = 2; break;
    case E_TRIPLE: cnt = 3; break;
    case E_BURST: {
        if (burst_len > n) burst_len = n;
        int s = rand() % (n - burst_len + 1);
        for (int i = 0; i < burst_len; i++) {
            int flip = (i == 0 || i == burst_len - 1) ? 1 : rand() % 2; /* burst ends are always wrong */
            if (flip) { cw[s + i] = cw[s + i] == '0' ? '1' : '0'; if (mark) mark[s + i] = 3; }
        }
        return; }
    default: return;
    }
    for (int i = 0; i < cnt; i++) {
        int p, dup;
        do { p = rand() % n; dup = 0; for (int j = 0; j < i; j++) if (pos[j] == p) dup = 1; } while (dup);
        pos[i] = p;
        cw[p] = cw[p] == '0' ? '1' : '0';
        if (mark) mark[p] = 3;
    }
}

static void show_parity_table(const char *msg) {
    printf("\n  %-6s %-10s %-6s %-12s %s\n", "Char", "Bits", "1s", "Parity type", "Even-parity bit");
    for (int i = 0; msg[i]; i++) {
        char b[9];
        to_bits((unsigned char *)&msg[i], 1, b);
        int o = ones_in(b, 8);
        printf("  '%c'    %s   %-6d %-12s %d\n", msg[i], b, o, o % 2 ? "ODD" : "EVEN", o % 2);
    }
}

static void menu_error_demo(void) {
    char msg[MAXMSG], data[MAXB], cw[MAXB], rx[MAXB], mark[MAXB];
    header("ERROR DETECTION : VRC / LRC / CRC / CHECKSUM");
    read_msg(msg, "NET");
    int n = to_bits((unsigned char *)msg, (int)strlen(msg), data);
    printf("\n  1. VRC (parity finding)  2. LRC  3. CRC (generator %s)  4. Checksum-16\n", crc_gen);
    int m = read_int("Choose method [3]: ", 3, 1, 4) - 1;

    if (m == M_VRC) show_parity_table(msg);
    if (m == M_CRC && n <= 40) {
        char dividend[MAXB], rem[40];
        int r = (int)strlen(crc_gen) - 1;
        strcpy(dividend, data);
        for (int i = 0; i < r; i++) dividend[n + i] = '0';
        dividend[n + r] = 0;
        printf("\n%sSender: modulo-2 division by %s (data + %d zeros)%s\n", c(BLUE), crc_gen, r, c(RESET));
        mod2_div(dividend, crc_gen, rem, 1);
        printf("  Remainder (CRC) = %s%s%s\n", c(MAG), rem, c(RESET));
    }
    if (m == M_CSUM) {
        char padded[MAXB]; strcpy(padded, data);
        int k = n; while (k % 16) padded[k++] = '0'; padded[k] = 0;
        printf("\n  16-bit words: ");
        for (int i = 0; i < k; i += 16) {
            unsigned w = 0; for (int b = 0; b < 16; b++) w = (w << 1) | (unsigned)(padded[i + b] - '0');
            printf("%04X ", w);
        }
        unsigned s = csum16(padded, k);
        printf("\n  Wrapped sum = %04X   Checksum = ~sum = %s%04X%s\n", s, c(MAG), (~s) & 0xFFFF, c(RESET));
    }

    int k = encode(m, data, cw, mark);
    printf("\n%sCodeword sent%s (%sredundant bits%s)\n", c(BLUE), c(RESET), c(MAG), c(RESET));
    print_bits(cw, mark, m == M_VRC ? 9 : 8);
    printf("  Data %d bits + redundancy %d bits\n", n, k - n);

    printf("\n  Channel noise: 0. none  1. single-bit  2. two-bit  3. three-bit  4. burst  5. choose positions\n");
    int e = read_int("Choose error [1]: ", 1, 0, 5);
    strcpy(rx, cw);
    memset(mark, 0, MAXB);
    if (e == 5) {
        char line[200];
        read_line("Bit positions to flip (0-based, space separated): ", line, sizeof line);
        char *tok = strtok(line, " ,");
        while (tok) {
            int p = atoi(tok);
            if (p >= 0 && p < k) { rx[p] = rx[p] == '0' ? '1' : '0'; mark[p] = 3; }
            tok = strtok(NULL, " ,");
        }
    } else if (e == 4) {
        int L = read_int("Burst length [6]: ", 6, 2, k);
        inject(rx, E_BURST, L, mark);
    } else inject(rx, e, 0, mark);

    printf("\n%sCodeword received%s (%sflipped bits%s)\n", c(BLUE), c(RESET), c(RED), c(RESET));
    print_bits(rx, mark, m == M_VRC ? 9 : 8);
    int ok = check(m, rx), corrupted = strcmp(rx, cw) != 0;
    if (m == M_CRC) {
        char rem[40]; mod2_div(rx, crc_gen, rem, 0);
        printf("  Receiver remainder = %s\n", rem);
    }
    if (m == M_CSUM) printf("  Receiver sum of all words = %04X (must be FFFF)\n", csum16(rx, k));
    printf("\n  Receiver verdict: ");
    if (ok && !corrupted) printf("%sACCEPTED - no error%s\n", c(GREEN), c(RESET));
    else if (!ok)         printf("%sERROR DETECTED - frame discarded%s\n", c(RED), c(RESET));
    else                  printf("%sACCEPTED - but data is corrupted! (undetected error)%s\n", c(YELLOW), c(RESET));
}

/* Benchmark: detection rate of every method against every error pattern. */
static void menu_error_experiment(void) {
    header("EXPERIMENT : ERROR DETECTION CAPABILITY");
    int trials = read_int("Trials per cell [5000]: ", 5000, 100, 200000);
    int bytes  = read_int("Random data length in bytes [16]: ", 16, 2, 64);
    struct { const char *name; int pat, len; } P[] = {
        { "Single-bit", E_SINGLE, 0 }, { "Two-bit", E_DOUBLE, 0 }, { "Three-bit", E_TRIPLE, 0 },
        { "Burst <= 8", E_BURST, 8 }, { "Burst = 12", E_BURST, 12 }, { "Burst = 20", E_BURST, 20 },
    };
    int NP = (int)(sizeof P / sizeof P[0]);
    double rate[6][M_COUNT];
    unsigned char raw[64]; char data[MAXB], cw[MAXB];

    for (int p = 0; p < NP; p++)
        for (int m = 0; m < M_COUNT; m++) {
            int det = 0;
            for (int t = 0; t < trials; t++) {
                for (int i = 0; i < bytes; i++) raw[i] = (unsigned char)(rand() & 0xFF);
                to_bits(raw, bytes, data);
                encode(m, data, cw, NULL);
                int L = P[p].len;
                if (P[p].pat == E_BURST && L == 8) L = 2 + rand() % 7;   /* lengths 2..8 */
                inject(cw, P[p].pat, L, NULL);
                if (!check(m, cw)) det++;
            }
            rate[p][m] = 100.0 * det / trials;
        }

    printf("\n  Detection rate (%%), %d trials each, CRC generator %s\n\n", trials, crc_gen);
    printf("  %-12s", "Error type");
    for (int m = 0; m < M_COUNT; m++) printf(" %18s", MNAME[m]);
    printf("\n  ------------");
    for (int m = 0; m < M_COUNT; m++) printf(" ------------------");
    printf("\n");
    FILE *f = fopen("detection_results.csv", "w");
    if (f) fprintf(f, "error_type,VRC,LRC,CRC,Checksum16\n");
    for (int p = 0; p < NP; p++) {
        printf("  %-12s", P[p].name);
        if (f) fprintf(f, "%s", P[p].name);
        for (int m = 0; m < M_COUNT; m++) {
            double r = rate[p][m];
            const char *col = r >= 99.999 ? c(GREEN) : r >= 90 ? c(YELLOW) : c(RED);
            printf(" %s%17.2f%%%s", col, r, c(RESET));
            if (f) fprintf(f, ",%.2f", r);
        }
        printf("\n");
        if (f) fprintf(f, "\n");
    }
    if (f) { fclose(f); printf("\n  Saved to detection_results.csv\n"); }
    printf("\n  %sObservations%s\n", c(BLUE), c(RESET));
    printf("  - VRC misses an even number of flips inside the same character; it only scores\n");
    printf("    well on two-bit errors because random flips usually land in different bytes.\n");
    printf("  - LRC misses two flips in the same bit column of different bytes.\n");
    printf("  - LRC and CRC-8 catch every burst no longer than 8 bits.\n");
    printf("  - CRC with r check bits misses only about 1 in 2^r long bursts (2^8 = 256 -> ~99.6%%).\n");
    printf("  - Try CRC-16 in Settings: every cell goes to (almost) 100%%.\n");
}

/* ============================================================================
 *  3. FLOW AND ERROR CONTROL (ARQ)
 * ==========================================================================*/
typedef struct { int tx, retx, rounds; } Stats;
static int quiet = 0;
#define LOG(...) do { if (!quiet) printf(__VA_ARGS__); } while (0)

/* Go-Back-N (Stop-and-Wait is Go-Back-N with W = 1).
 * One "round" = sender transmits its window, then waits one timeout interval. */
static Stats sim_gbn(int N, int W, int p) {
    Stats s = { 0, 0, 0 };
    int base = 0, next = 0, expected = 0;
    int sent[MAXFR] = { 0 };
    while (base < N) {
        s.rounds++;
        LOG("\n%s-- Round %d   window [%d..%d]%s\n", c(CYAN), s.rounds, base, (base + W < N ? base + W : N) - 1, c(RESET));
        int acked = base;
        while (next < base + W && next < N) {
            s.tx++;
            int re = sent[next]; if (re) s.retx++;
            sent[next] = 1;
            LOG("  Sender   : send F%-3d %s\n", next, re ? "(retransmission)" : "");
            if (chance(p)) { LOG("  %sChannel  : F%d LOST%s\n", c(RED), next, c(RESET)); next++; continue; }
            int send_ack = 0;
            if (next == expected) { expected++; send_ack = 1; LOG("  Receiver : F%d accepted, sends ACK%d\n", next, expected); }
            else if (next < expected) { send_ack = 1; LOG("  Receiver : F%d duplicate, discarded, re-sends ACK%d\n", next, expected); }
            else LOG("  %sReceiver : F%d out of order (expecting F%d), discarded%s\n", c(YELLOW), next, expected, c(RESET));
            if (send_ack) {
                if (chance(p)) LOG("  %sChannel  : ACK%d LOST%s\n", c(RED), expected, c(RESET));
                else { if (expected > acked) acked = expected; LOG("  %sSender   : ACK%d received%s\n", c(GREEN), expected, c(RESET)); }
            }
            next++;
        }
        if (acked > base) { LOG("  Sender   : window slides to F%d\n", acked); base = acked; }
        if (base < next) { LOG("  %sSender   : TIMEOUT, go back to F%d%s\n", c(MAG), base, c(RESET)); next = base; }
    }
    return s;
}

static Stats sim_sr(int N, int W, int p) {
    Stats s = { 0, 0, 0 };
    int state[MAXFR] = { 0 };   /* 0 unsent, 1 sent & waiting, 2 acked */
    int rcv[MAXFR] = { 0 };
    int base = 0, rbase = 0;
    while (base < N) {
        s.rounds++;
        LOG("\n%s-- Round %d   window [%d..%d]%s\n", c(CYAN), s.rounds, base, (base + W < N ? base + W : N) - 1, c(RESET));
        for (int i = base; i < base + W && i < N; i++) {
            if (state[i] == 2) continue;
            s.tx++;
            int re = state[i] == 1; if (re) s.retx++;
            state[i] = 1;
            LOG("  Sender   : send F%-3d %s\n", i, re ? "(selective retransmission)" : "");
            if (chance(p)) { LOG("  %sChannel  : F%d LOST%s\n", c(RED), i, c(RESET)); continue; }
            if (rcv[i]) LOG("  Receiver : F%d duplicate, re-sends ACK%d\n", i, i);
            else {
                rcv[i] = 1;
                LOG("  Receiver : F%d %s, sends ACK%d\n", i, i == rbase ? "accepted" : "buffered (out of order)", i);
                int from = rbase;
                while (rbase < N && rcv[rbase]) rbase++;
                if (rbase > from) LOG("  Receiver : delivers F%d..F%d to network layer\n", from, rbase - 1);
            }
            if (chance(p)) LOG("  %sChannel  : ACK%d LOST%s\n", c(RED), i, c(RESET));
            else { state[i] = 2; LOG("  %sSender   : ACK%d received%s\n", c(GREEN), i, c(RESET)); }
        }
        int ob = base;
        while (base < N && state[base] == 2) base++;
        if (base > ob) LOG("  Sender   : window slides to F%d\n", base);
        for (int i = base; i < base + W && i < N; i++)
            if (state[i] == 1) LOG("  %sSender   : TIMEOUT for F%d only%s\n", c(MAG), i, c(RESET));
    }
    return s;
}

static void print_stats(const char *name, int N, Stats s) {
    printf("\n  %s%s summary%s\n", c(BLUE), name, c(RESET));
    printf("  Frames delivered     : %d\n", N);
    printf("  Total transmissions  : %d\n", s.tx);
    printf("  Retransmissions      : %d\n", s.retx);
    printf("  Timeout rounds       : %d\n", s.rounds);
    printf("  Efficiency           : %.1f%% (useful frames / frames sent)\n", 100.0 * N / s.tx);
}

static void menu_arq(void) {
    header("FLOW CONTROL : STOP-AND-WAIT / GO-BACK-N / SELECTIVE REPEAT");
    printf("  1. Stop-and-Wait   2. Go-Back-N   3. Selective Repeat\n");
    int proto = read_int("Choose protocol [2]: ", 2, 1, 3);
    int N = read_int("Number of frames [10]: ", 10, 1, MAXFR);
    int W = proto == 1 ? 1 : read_int("Window size [4]: ", 4, 1, 64);
    int p = read_int("Loss probability for frames and ACKs, % [20]: ", 20, 0, 90);
    if (proto == 3 && W > N) W = N;
    Stats s;
    if (proto == 3) s = sim_sr(N, W, p); else s = sim_gbn(N, W, p);
    print_stats(proto == 1 ? "Stop-and-Wait" : proto == 2 ? "Go-Back-N" : "Selective Repeat", N, s);
}

static void menu_compare(void) {
    header("EXPERIMENT : PROTOCOL EFFICIENCY VS LOSS RATE");
    int N = read_int("Frames per run [50]: ", 50, 5, MAXFR);
    int W = read_int("Window size for GBN and SR [7]: ", 7, 2, 64);
    int runs = read_int("Runs averaged per point [200]: ", 200, 10, 5000);
    quiet = 1;
    FILE *f = fopen("arq_results.csv", "w");
    if (f) fprintf(f, "loss_pct,stop_and_wait,go_back_n,selective_repeat\n");
    printf("\n  Average transmissions per delivered frame (lower is better)\n\n");
    printf("  %-8s %15s %15s %18s\n", "Loss %", "Stop-and-Wait", "Go-Back-N", "Selective Repeat");
    printf("  -------- --------------- --------------- ------------------\n");
    for (int p = 0; p <= 50; p += 5) {
        double a = 0, b = 0, d = 0;
        for (int r = 0; r < runs; r++) {
            a += sim_gbn(N, 1, p).tx;
            b += sim_gbn(N, W, p).tx;
            d += sim_sr(N, W, p).tx;
        }
        a /= (double)runs * N; b /= (double)runs * N; d /= (double)runs * N;
        printf("  %-8d %15.2f %15.2f %18.2f\n", p, a, b, d);
        if (f) fprintf(f, "%d,%.3f,%.3f,%.3f\n", p, a, b, d);
    }
    quiet = 0;
    if (f) { fclose(f); printf("\n  Saved to arq_results.csv\n"); }
    printf("\n  %sObservations%s\n", c(BLUE), c(RESET));
    printf("  - Go-Back-N wastes bandwidth as loss grows: one loss resends the whole window.\n");
    printf("  - Selective Repeat resends only lost frames, so it stays close to Stop-and-Wait\n");
    printf("    in bandwidth while keeping the pipelining that Stop-and-Wait lacks.\n");
}

/* ============================================================================
 *  4. FULL PIPELINE : message -> frames -> CRC -> bit stuffing -> noisy link
 *                     -> de-stuff -> CRC check -> ACK/NAK (Stop-and-Wait ARQ)
 * ==========================================================================*/
static void menu_pipeline(void) {
    char msg[MAXMSG], received[MAXMSG] = "";
    header("FULL TRANSMISSION PIPELINE");
    read_msg(msg, "HELLO LJIET");
    int fsize = read_int("Payload bytes per frame [4]: ", 4, 1, 16);
    int p = read_int("Probability a frame gets a bit error, % [40]: ", 40, 0, 90);
    int len = (int)strlen(msg), nframes = (len + fsize - 1) / fsize;
    int total_tx = 0, detected = 0;
    printf("\n  Message is split into %d frames. Each frame = [SEQ 8 bits][DATA][CRC %d bits],\n",
           nframes, (int)strlen(crc_gen) - 1);
    printf("  then bit-stuffed and wrapped in HDLC flags.\n");

    for (int f = 0; f < nframes; f++) {
        int plen = (len - f * fsize) < fsize ? (len - f * fsize) : fsize;
        unsigned char payload[20];
        memcpy(payload, msg + f * fsize, plen);
        payload[plen] = 0;
        char data[MAXB] = "", cw[MAXB], frame[MAXB], mark[MAXB], rxdata[MAXB];
        append_bits(data, (unsigned)f, 8);
        char pb[MAXB]; to_bits(payload, plen, pb); strcat(data, pb);
        encode(M_CRC, data, cw, NULL);
        bit_stuff(cw, frame, mark);

        printf("\n%s=== Frame %d  payload \"%s\" ===%s\n", c(CYAN), f, (char *)payload, c(RESET));
        int attempt = 0;
        while (1) {
            attempt++; total_tx++;
            char line[MAXB], lmark[MAXB];
            strcpy(line, frame);
            memcpy(lmark, mark, MAXB);
            int noisy = chance(p);
            if (noisy) {                              /* flip one bit between the flags */
                int n = (int)strlen(line), pos = 8 + rand() % (n - 16);
                line[pos] = line[pos] == '0' ? '1' : '0';
                lmark[pos] = 3;
            }
            printf("  Attempt %d - on the wire:\n", attempt);
            print_bits(line, lmark, 0);
            int dl = bit_destuff(line, rxdata);
            if (dl < 0) {
                detected++;
                printf("  %sReceiver: FRAMING ERROR (bad flag or stuffing) -> NAK%s\n", c(RED), c(RESET));
                continue;
            }
            if (!check(M_CRC, rxdata)) {
                detected++;
                printf("  %sReceiver: CRC FAILED -> NAK, sender retransmits%s\n", c(RED), c(RESET));
                continue;
            }
            unsigned char out[20];
            int r = (int)strlen(crc_gen) - 1;
            rxdata[dl - r] = 0;                         /* strip CRC */
            from_bits(rxdata + 8, dl - r - 8, out);     /* strip SEQ */
            printf("  %sReceiver: CRC OK -> ACK%d, payload \"%s\"%s\n", c(GREEN), f + 1, (char *)out, c(RESET));
            strcat(received, (char *)out);
            break;
        }
    }
    printf("\n%s=== Result ===%s\n", c(CYAN), c(RESET));
    printf("  Sent message     : %s\n", msg);
    printf("  Received message : %s\n", received);
    printf("  Frames %d, transmissions %d, corrupted frames caught %d\n", nframes, total_tx, detected);
    printf("  Integrity        : %s%s%s\n", strcmp(msg, received) == 0 ? c(GREEN) : c(RED),
           strcmp(msg, received) == 0 ? "MESSAGE DELIVERED INTACT" : "MISMATCH", c(RESET));
}

/* ============================================================================
 *  SETTINGS AND MAIN
 * ==========================================================================*/
static void menu_settings(void) {
    header("SETTINGS");
    printf("  Current CRC generator: %s\n", crc_gen);
    printf("  Presets: 1. CRC-8 (100000111)  2. CRC-4 (10011)  3. CRC-16-CCITT (10001000000100001)\n");
    printf("           4. custom   0. keep\n");
    int ch = read_int("Choice [0]: ", 0, 0, 4);
    if (ch == 1) strcpy(crc_gen, "100000111");
    if (ch == 2) strcpy(crc_gen, "10011");
    if (ch == 3) strcpy(crc_gen, "10001000000100001");
    if (ch == 4) {
        char g[40]; int ok = 1;
        read_line("Generator bits (must start and end with 1): ", g, sizeof g);
        int n = (int)strlen(g);
        for (int i = 0; i < n; i++) if (g[i] != '0' && g[i] != '1') ok = 0;
        if (ok && n >= 2 && g[0] == '1' && g[n - 1] == '1') strcpy(crc_gen, g);
        else printf("  Invalid generator, unchanged.\n");
    }
    int col = read_int("Colours on? 1 = yes, 0 = no [1]: ", 1, 0, 1);
    use_color = col;
    int seed = read_int("Random seed (0 = time based) [0]: ", 0, 0, 1000000000);
    srand(seed ? (unsigned)seed : (unsigned)time(NULL));
    printf("  Saved. CRC generator = %s\n", crc_gen);
}

int main(int argc, char **argv) {
#ifdef _WIN32
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode = 0;
    if (GetConsoleMode(h, &mode)) SetConsoleMode(h, mode | 0x0004); /* enable ANSI colours */
#endif
    if (argc > 1 && strcmp(argv[1], "--no-color") == 0) use_color = 0;
    srand((unsigned)time(NULL));
    while (1) {
        header("LinkLab : Data Link Layer Simulator");
        printf("  1. Full transmission pipeline (framing + CRC + ARQ)\n");
        printf("  2. Framing : bit stuffing\n");
        printf("  3. Framing : byte stuffing\n");
        printf("  4. Error detection demo (parity, VRC, LRC, CRC, checksum)\n");
        printf("  5. Experiment : error detection capability\n");
        printf("  6. Flow control : Stop-and-Wait / Go-Back-N / Selective Repeat\n");
        printf("  7. Experiment : protocol efficiency vs loss rate\n");
        printf("  8. Settings (CRC generator, colours, seed)\n");
        printf("  0. Exit\n");
        char b[16];
        read_line("\nChoice: ", b, sizeof b);
        if (feof(stdin) && !b[0]) break;
        int ch = atoi(b);
        switch (ch) {
        case 1: menu_pipeline(); break;
        case 2: menu_bit_stuffing(); break;
        case 3: menu_byte_stuffing(); break;
        case 4: menu_error_demo(); break;
        case 5: menu_error_experiment(); break;
        case 6: menu_arq(); break;
        case 7: menu_compare(); break;
        case 8: menu_settings(); break;
        case 0: printf("Bye.\n"); return 0;
        default: continue;
        }
        pause_key();
    }
    return 0;
}
