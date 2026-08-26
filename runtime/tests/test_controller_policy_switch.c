/* Behavioural test for controller presentation policy switching.
 * Drives a sequence of samples through the pure helpers for both SIO ports,
 * with and without a plugin policy, D-pad-only and stick-only input, and a
 * policy change between two consecutive samples — asserting that no physical
 * sample ever carries a D-pad-derived stick deflection and that a mode change
 * takes effect on the next sample with no mixed sample in between. */
#include "controller_policy.h"
#include <stdio.h>
#include <string.h>

static int g_fail = 0;
#define CHECK(cond, ...) do { if (!(cond)) { printf("FAIL: " __VA_ARGS__); printf("\n"); g_fail++; } } while (0)

/* PSX pad word is active-low. */
#define BTN_NONE   0xFFFFu
#define DPAD_UP    0x0010u
#define DPAD_RIGHT 0x0020u
#define DPAD_DOWN  0x0040u
#define DPAD_LEFT  0x0080u
#define PRESS(mask) ((uint16_t)(BTN_NONE & ~(mask)))

typedef struct Sample {
    int      port;           /* 0 or 1 */
    int      configured;     /* seat mode */
    int      policy_present;
    uint32_t policy_request; /* what the plugin answers this sample */
    uint16_t buttons;
    uint8_t  raw[4];
} Sample;

typedef struct Presented {
    int     mode;
    uint8_t st[4];
} Presented;

static Presented present_physical(const Sample* s)
{
    Presented p;
    p.mode = controller_policy_effective_mode(s->configured, s->policy_present,
                                              s->policy_request);
    controller_pad_compose_physical(p.mode, s->raw, p.st);
    return p;
}

static const uint8_t CENTRE[4] = { 0x80, 0x80, 0x80, 0x80 };
static const uint8_t STICK_LEFT[4] = { 0x10, 0x80, 0x80, 0x80 };

static void run_sequence(const char* name, const Sample* seq, int n)
{
    int i;
    int prev_mode = -1;
    for (i = 0; i < n; i++) {
        Presented p = present_physical(&seq[i]);
        /* invariant 1: never a D-pad-derived deflection on a physical sample */
        CHECK(!controller_pad_sample_has_dpad_fold(p.st, seq[i].raw),
              "%s[%d]: port %d presented a D-pad fold (lx=%02X ly=%02X)",
              name, i, seq[i].port, p.st[0], p.st[1]);
        /* invariant 2: DIGITAL -> centred; ANALOG -> raw exactly */
        if (p.mode == PSX_CTRL_MODE_DIGITAL)
            CHECK(memcmp(p.st, CENTRE, 4) == 0, "%s[%d]: digital sample not centred", name, i);
        else
            CHECK(memcmp(p.st, seq[i].raw, 4) == 0, "%s[%d]: analog sample not raw", name, i);
        /* invariant 3: a mode change is clean — the sample is fully in the new
         * mode (already covered by invariant 2), and no latch survives: the
         * same inputs with the previous request must reproduce the previous mode */
        if (prev_mode >= 0 && p.mode != prev_mode) {
            Sample back = seq[i]; back.policy_request = seq[i - 1].policy_request;
            back.configured = seq[i - 1].configured; back.policy_present = seq[i - 1].policy_present;
            Presented q = present_physical(&back);
            CHECK(q.mode == prev_mode, "%s[%d]: mode change left a latch", name, i);
        }
        prev_mode = p.mode;
    }
}

int main(void)
{
    /* Non-plugin seats: configured mode rules, on both ports. */
    {
        Sample seq[] = {
            { 0, PSX_CTRL_MODE_ANALOG,  0, PSX_CTRL_POLICY_NONE, PRESS(DPAD_LEFT), {0x80,0x80,0x80,0x80} },
            { 0, PSX_CTRL_MODE_ANALOG,  0, PSX_CTRL_POLICY_NONE, BTN_NONE,         {0x10,0x80,0x80,0x80} },
            { 1, PSX_CTRL_MODE_DIGITAL, 0, PSX_CTRL_POLICY_NONE, PRESS(DPAD_UP),   {0x80,0x80,0x80,0x80} },
            { 1, PSX_CTRL_MODE_DIGITAL, 0, PSX_CTRL_POLICY_NONE, BTN_NONE,         {0x10,0x80,0x80,0x80} },
        };
        run_sequence("non-plugin", seq, 4);
        CHECK(present_physical(&seq[0]).mode == PSX_CTRL_MODE_ANALOG, "P1 analog seat stays analog on D-pad");
        CHECK(memcmp(present_physical(&seq[0]).st, CENTRE, 4) == 0, "analog seat + D-pad only: sticks centred (no fold)");
        CHECK(present_physical(&seq[3]).mode == PSX_CTRL_MODE_DIGITAL, "P2 digital seat stays digital on stick");
        CHECK(memcmp(present_physical(&seq[3]).st, CENTRE, 4) == 0, "digital seat + stick: sticks centred");
    }

    /* Plugin policy: D-pad-only sample -> plugin asks DIGITAL; stick-only -> ANALOG;
     * then a policy change between two consecutive samples, on both ports. */
    {
        Sample seq[] = {
            { 0, PSX_CTRL_MODE_ANALOG, 1, PSX_CTRL_MODE_DIGITAL, PRESS(DPAD_LEFT), {0x80,0x80,0x80,0x80} },
            { 0, PSX_CTRL_MODE_ANALOG, 1, PSX_CTRL_MODE_ANALOG,  BTN_NONE,         {0x10,0x80,0x80,0x80} },
            { 0, PSX_CTRL_MODE_ANALOG, 1, PSX_CTRL_MODE_DIGITAL, PRESS(DPAD_DOWN), {0x10,0x80,0x80,0x80} }, /* both live: plugin chose digital */
            { 0, PSX_CTRL_MODE_ANALOG, 1, PSX_CTRL_MODE_ANALOG,  PRESS(DPAD_DOWN), {0x10,0x80,0x80,0x80} }, /* both live: plugin chose analog */
            { 1, PSX_CTRL_MODE_DIGITAL,1, PSX_CTRL_MODE_ANALOG,  BTN_NONE,         {0x80,0x30,0x80,0x80} },
            { 1, PSX_CTRL_MODE_DIGITAL,1, PSX_CTRL_MODE_DIGITAL, PRESS(DPAD_RIGHT),{0x80,0x30,0x80,0x80} },
        };
        run_sequence("plugin", seq, 6);
        Presented p2 = present_physical(&seq[2]);
        CHECK(p2.mode == PSX_CTRL_MODE_DIGITAL && memcmp(p2.st, CENTRE, 4) == 0,
              "plugin DIGITAL with both sources live: digital, centred (no both-at-once)");
        Presented p3 = present_physical(&seq[3]);
        CHECK(p3.mode == PSX_CTRL_MODE_ANALOG && memcmp(p3.st, seq[3].raw, 4) == 0,
              "plugin ANALOG with both sources live: raw stick, D-pad bits stay in the button word only");
    }

    /* Invalid plugin answer falls back to the configured seat mode. */
    CHECK(controller_policy_effective_mode(PSX_CTRL_MODE_DIGITAL, 1, 7u) == PSX_CTRL_MODE_DIGITAL,
          "invalid policy request -> configured (digital)");
    CHECK(controller_policy_effective_mode(PSX_CTRL_MODE_ANALOG, 1, PSX_CTRL_POLICY_NONE) == PSX_CTRL_MODE_ANALOG,
          "no policy request -> configured (analog)");

    /* Injected samples: the fold exists only there, only in ANALOG, only without a live stick. */
    {
        uint8_t st[4];
        memcpy(st, CENTRE, 4);
        controller_pad_compose_injected(PSX_CTRL_MODE_ANALOG, 0, PRESS(DPAD_LEFT), st);
        CHECK(st[0] == 0x00, "injected analog D-pad folds left onto lx");
        memcpy(st, STICK_LEFT, 4);
        controller_pad_compose_injected(PSX_CTRL_MODE_ANALOG, 1, PRESS(DPAD_RIGHT), st);
        CHECK(st[0] == 0x10, "injected: live stick override wins over the fold");
        memcpy(st, STICK_LEFT, 4);
        controller_pad_compose_injected(PSX_CTRL_MODE_DIGITAL, 1, PRESS(DPAD_RIGHT), st);
        CHECK(memcmp(st, CENTRE, 4) == 0, "injected digital: centred");
    }

    if (g_fail) { printf("%d failure(s)\n", g_fail); return 1; }
    printf("controller policy switch: all checks pass\n");
    return 0;
}
