/**
 * Mesh Talk — Implementation
 *
 * ==[ CHAT ON A 320x240 ]== the scrollback pins to the newest line the way a
 * chat window does; scrolling walks backward through it and any new arrival
 * snaps back to the bottom only if the reader was already there.
 *
 * The wrap index is rebuilt on Mesh's revision counter rather than every
 * frame — the mesh speaks a few times a minute at most, and the compositor
 * shares its budget with a room renderer.
 */

#include "mesh_menu.h"

#include <stdio.h>
#include <string.h>

#include "display.h"
#include "soft_keyboard.h"
#include "ui_measurements.h"
#include "../audio/sfx.h"
#include "../core/config.h"
#include "../hamlet.h"
#include "../radio/meshtastic_uart.h"
#include "../util/psram_block.h"

namespace MeshMenu {

using namespace UIMeasurements;

// ==[ LAYOUT ]== 6x8 font, 4px side margin.
static constexpr int  PANE_X = 4;
static constexpr int  LINE_H = 9;
static constexpr int  STATUS_H = 10;
static constexpr int  PANE_Y = kTopBarH + STATUS_H;
static constexpr int  PANE_H = kMainAreaH - STATUS_H;

// ==[ THE RIGHT LANE ]== the scroll marks used to be a caret drawn at x=310,
// which is inside the text: a divider rule reaches x=314 and so does any body
// line that wraps at full width, so the mark landed on top of the content it
// was describing. Reserving a lane costs one column and buys a real position
// indicator — how far back you are AND how much there is — instead of a glyph
// that could only say "there is more".
static constexpr int SCROLL_W = 3;
static constexpr int SCROLL_X = kScreenWidth - PANE_X - SCROLL_W;   // 313
static constexpr int TEXT_RIGHT = SCROLL_X - 2;                     // 311
static constexpr uint8_t TOTAL_COLS = (TEXT_RIGHT - PANE_X) / kCharWSize1;  // 51

// The sender column was eight characters on the assumption that a mesh name
// could be long. It cannot: the firmware stamps user.short_name on the line and
// that field is `char short_name[5]`, four characters. The four columns this
// gives back are four more columns of message on every single row.
static constexpr uint8_t SENDER_COLS = 4;
// ==[ ROOM AROUND THE RAIL ]== the rail used to butt straight against both the
// name and the body, so a four-character name beside a hop count rendered as
// "0ct01oink from the mesh" — the mark read as a fifth letter of the name and
// the body had no left edge to find. Two columns of air is the whole fix.
static constexpr uint8_t RAIL_COL = SENDER_COLS + 1;
static constexpr uint8_t GUTTER_COLS = RAIL_COL + 2;
static constexpr uint8_t BODY_COLS = TOTAL_COLS - GUTTER_COLS;
static constexpr int  VISIBLE_LINES = PANE_H / LINE_H;

// ==[ PANE TABS ]== the strip was the only way between the scrollback and the
// roster and it looked exactly like the telemetry it sits in, so the control
// was invisible. Two pills at its right end read as tabs, which is a thing
// every reader already knows how to use.
static constexpr int TAB_W = 3 * kCharWSize1 + 2;                     // 20
static constexpr int TAB_GAP = 1;
static constexpr int TABS_X = kScreenWidth - PANE_X - TAB_W * 2 - TAB_GAP;
// Telemetry is clipped to the tabs rather than trusted to stay short: the
// strip grows with the session counters and a collision here would overwrite
// the only control on the screen.
static constexpr int STRIP_COLS = (TABS_X - 2 - PANE_X) / kCharWSize1;

// 64 messages can wrap to more rows than this — a 233-character payload over
// 44 columns is six lines, and six times sixty-four is 384 before a single
// divider. The index is therefore a window on the ring, not the whole of it,
// and it is filled from the newest message backward: the bottom of a chat is
// the part that must never be missing.
static constexpr uint16_t MAX_LINES = 320;

// Above this gap between consecutive messages the scrollback gets a rule
// carrying the age of what follows. Below it, two messages are one exchange
// and a rule between them is just noise.
static constexpr uint32_t DIVIDER_GAP_MS = 300000;   // 5 minutes

struct LineRef {
    uint8_t msgIdx;
    uint8_t start;    // offset into Message::body
    uint8_t len;
    bool    first;    // draws the sender gutter
    bool    last;     // closes the message rail on this row
    bool    divider;  // time rule above msgIdx; start/len/first unused
};

// ==[ VIEW STATE ]== one PSRAM block. As statics these two overflowed Core2's
// dram0_0_seg; neither is hot enough to justify the space. The index is walked
// once per frame for the ~22 visible rows, and the compose buffer only while a
// keyboard is open.
struct ViewState {
    LineRef lines[MAX_LINES];
    char    composeBuf[Mesh::BODY_LEN];
};
static ViewState* view = nullptr;

// ==[ TWO VIEWS ]== the scrollback answers "what was said"; the roster answers
// "who is out there", which is a question TEXTMSG could not even be asked.
// They share the pane rather than the screen because 320x240 has room for one
// dense thing at a time, and the strip at the top is the toggle.
enum class Pane : uint8_t { CHAT, NODES };
static Pane pane = Pane::CHAT;

// Roster cursor. Kept separate from the chat scroll so switching views twice
// puts you back where you were in both.
static uint8_t nodeSel = 0;
static uint8_t nodeTop = 0;

// Who the composer is addressing. BROADCAST unless a node was picked off the
// roster, and reset on every entry — a DM target that outlives the screen it
// was chosen on is how a private message ends up somewhere else.
static uint32_t dmTarget = Mesh::BROADCAST_ADDR;

static bool     active = false;
static bool     composing = false;
static uint16_t lineCount = 0;
static uint32_t indexedRevision = 0xFFFFFFFFu;
static uint16_t scrollFromBottom = 0;   // 0 = pinned to the newest line

// ==[ SCROLL ANCHOR ]== scrollFromBottom alone cannot hold a reader in place,
// because both ends of the index move: an arrival adds rows at the bottom and
// a full ring drops rows off the top. Naming the row the reader is actually
// looking at — by message sequence, which never shifts — is what makes
// "scrolling back parks you there" true.
struct Anchor {
    uint32_t seq;
    uint8_t  start;
    bool     divider;
    bool     valid;
};

// Said by the composer and by the roster's pick action, which are two different
// screens reaching the same refusal.
static constexpr const char* DM_NEEDS_PROTO = "T3XTMSG C4NN0T DM - US3 PR0T0";

static bool ensureView() {
    if (view) return true;
    view = PsramBlock::allocFor<ViewState>();
    return view != nullptr;
}

// ==[ WRAP INDEX ]==

// Break at an embedded newline first, then at the last space that still leaves
// a usefully full line. Falling back to a hard cut matters for the case this
// screen actually sees: a pasted URL or a coordinate blob with no spaces in it
// at all.
//
// The newline case is not hypothetical. A Meshtastic payload is arbitrary bytes
// and the firmware prints them straight through, so a message typed with line
// breaks on a phone arrives with those breaks intact — and Font0 would render
// each one as a glyph in the middle of a sentence.
static uint8_t wrapPoint(const char* body, size_t from, size_t len) {
    const size_t remaining = len - from;
    const size_t scan = remaining < BODY_COLS ? remaining : BODY_COLS;
    for (size_t i = 0; i < scan; ++i) {
        if (body[from + i] == '\n') return (uint8_t)i;
    }
    if (remaining <= BODY_COLS) return (uint8_t)remaining;

    for (size_t i = BODY_COLS; i > BODY_COLS / 2; --i) {
        if (body[from + i] == ' ') return (uint8_t)i;
    }
    return BODY_COLS;
}

// Step over the break itself: a wrap landing on a space would open the next
// line with it, and a hard newline must not be drawn at all. This is also what
// guarantees progress when wrapPoint returns 0 for a newline sitting at `pos`.
static size_t advancePastBreak(const char* body, size_t pos, size_t len) {
    if (pos < len && (body[pos] == ' ' || body[pos] == '\n')) pos++;
    return pos;
}

// How many rows this message will occupy, without writing any of them. The
// backward budget pass needs the height of a message before it can decide
// whether the message fits.
static uint16_t bodyLineCount(const Mesh::Message& msg) {
    const size_t len = strlen(msg.body);
    uint16_t rows = 0;
    size_t pos = 0;
    do {
        pos += wrapPoint(msg.body, pos, len);
        rows++;
        pos = advancePastBreak(msg.body, pos, len);
    } while (pos < len);
    return rows;
}

// A rule goes above the oldest message shown — it heads the scrollback with
// how far back the history reaches — and wherever the mesh went quiet.
static bool needsDivider(uint8_t m) {
    if (m == 0) return true;
    const uint32_t prev = Mesh::getMessage((uint8_t)(m - 1)).atMs;
    const uint32_t cur = Mesh::getMessage(m).atMs;
    return (uint32_t)(cur - prev) >= DIVIDER_GAP_MS;
}

static void rebuildIndex() {
    lineCount = 0;
    if (!ensureView()) return;
    const uint8_t count = Mesh::getMessageCount();
    if (count == 0) {
        indexedRevision = Mesh::getRingRevision();
        return;
    }

    // ==[ WHICH END TO CUT ]== walking forward from the oldest and stopping at
    // MAX_LINES drops the *newest* messages, so a long enough history could
    // leave the message that just chirped out of the index entirely. Count
    // backward from the newest instead and start at the oldest one that still
    // fits. Every message is budgeted a divider row it may not use, which
    // wastes a few slots and guarantees the forward pass cannot overrun.
    uint8_t firstMsg = (uint8_t)(count - 1);
    uint16_t budget = 0;
    for (uint8_t m = count; m > 0; --m) {
        const uint8_t idx = (uint8_t)(m - 1);
        const uint16_t need = (uint16_t)(bodyLineCount(Mesh::getMessage(idx)) + 1);
        if (budget + need > MAX_LINES) break;
        budget = (uint16_t)(budget + need);
        firstMsg = idx;
    }

    for (uint8_t m = firstMsg; m < count && lineCount < MAX_LINES; ++m) {
        if (m == firstMsg || needsDivider(m)) {
            view->lines[lineCount].msgIdx = m;
            view->lines[lineCount].start = 0;
            view->lines[lineCount].len = 0;
            view->lines[lineCount].first = false;
            view->lines[lineCount].last = false;
            view->lines[lineCount].divider = true;
            lineCount++;
            if (lineCount >= MAX_LINES) break;
        }

        const Mesh::Message& msg = Mesh::getMessage(m);
        const size_t len = strlen(msg.body);
        size_t pos = 0;
        bool first = true;

        do {
            const uint8_t take = wrapPoint(msg.body, pos, len);
            const size_t next = advancePastBreak(msg.body, pos + take, len);
            view->lines[lineCount].msgIdx = m;
            view->lines[lineCount].start = (uint8_t)pos;
            view->lines[lineCount].len = take;
            view->lines[lineCount].first = first;
            view->lines[lineCount].last = (next >= len);
            view->lines[lineCount].divider = false;
            lineCount++;
            first = false;
            pos = next;
        } while (pos < len && lineCount < MAX_LINES);
    }

    indexedRevision = Mesh::getRingRevision();
}

static uint16_t maxScroll() {
    return (lineCount > VISIBLE_LINES) ? (uint16_t)(lineCount - VISIBLE_LINES)
                                       : 0;
}

// The row currently painted at the top of the pane. Both draw() and the anchor
// go through here rather than deriving it twice — the sim models this one
// formula (sim/mesh_scroll_sim.py), so a second copy would silently stop being
// the thing that was verified.
static uint16_t topVisibleLine() {
    const uint16_t endLine = (uint16_t)(lineCount - scrollFromBottom);
    return (endLine > VISIBLE_LINES) ? (uint16_t)(endLine - VISIBLE_LINES) : 0;
}

static Anchor captureAnchor() {
    Anchor a = {0, 0, false, false};
    if (!view || lineCount == 0) return a;
    const LineRef& ref = view->lines[topVisibleLine()];
    a.seq = Mesh::getMessage(ref.msgIdx).seq;
    a.start = ref.start;
    a.divider = ref.divider;
    a.valid = true;
    return a;
}

// A divider and the first body row of the same message share a sequence and a
// start offset, so the flag has to be part of the identity or the view slides
// by exactly one row every time a rule appears above where the reader is.
static bool matchesAnchor(const LineRef& ref, const Anchor& a) {
    return ref.divider == a.divider && ref.start == a.start &&
           Mesh::getMessage(ref.msgIdx).seq == a.seq;
}

static void syncIndex() {
    // The ring counter, not the general one: the roster refreshes on every
    // decoded packet, and re-wrapping sixty-four messages because a node we
    // have never spoken to broadcast its position is the most expensive way to
    // redraw nothing.
    if (indexedRevision == Mesh::getRingRevision()) return;

    // A reader at the bottom rides the new arrival down; nothing to anchor.
    if (scrollFromBottom == 0) {
        rebuildIndex();
        return;
    }

    const Anchor anchor = captureAnchor();
    rebuildIndex();

    if (!anchor.valid) {
        if (scrollFromBottom > maxScroll()) scrollFromBottom = maxScroll();
        return;
    }

    for (uint16_t i = 0; i < lineCount; ++i) {
        if (!matchesAnchor(view->lines[i], anchor)) continue;
        // Put row i back on the top of the pane. scrollFromBottom counts up
        // from the newest line, so the offset that lands startLine on i is
        // simply how far i sits from the oldest reachable row.
        scrollFromBottom = (i < maxScroll()) ? (uint16_t)(maxScroll() - i) : 0;
        return;
    }

    // The anchored row fell off the front of the ring, or the scrollback was
    // cleared underneath the reader. The oldest line still held is the nearest
    // honest answer — snapping to the bottom would lose their place twice.
    scrollFromBottom = maxScroll();
}

// ==[ COMPOSE ]==

// The short name of whoever the composer is aimed at. Both the keyboard title
// and the bottom bar say this, and they have to say the same thing — a DM whose
// two labels disagree is worse than one with no label at all.
static const char* dmName() {
    const Mesh::Node* n = Mesh::findNode(dmTarget);
    return (n && n->shortName[0]) ? n->shortName : "N0D3";
}

static void startCompose() {
    if (!Mesh::isStarted()) {
        Display::showToast("M3SH BR1DG3 0FFL1N3", 1600);
        return;
    }
    if (!ensureView()) {
        Display::showToast("N0 M3M0RY F0R C0MP0S3", 1600);
        return;
    }
    if (!Mesh::canSend()) {
        // The radio merges anything arriving inside its one-second read window
        // into a single packet, so the queue is not a formality — refuse at the
        // door rather than take a message the wire has no room for.
        Display::showToast("S3ND QU3U3 FULL", 1400);
        return;
    }
    view->composeBuf[0] = '\0';
    // The title is the only place the composer can say who is about to
    // receive this. A DM that looks identical to a broadcast while you type it
    // is the one mistake worth spending a title bar to prevent.
    char title[24];
    if (dmTarget != Mesh::BROADCAST_ADDR) {
        snprintf(title, sizeof(title), "DM %s", dmName());
    } else {
        snprintf(title, sizeof(title), "M3SH MSG");
    }

    // meshtastic_Constants_DATA_PAYLOAD_LEN is the radio's own ceiling, and
    // going past it does not truncate — the overflow becomes a second mesh
    // packet. Refuse the characters here instead of broadcasting an orphan.
    SoftKeyboard::start(title, view->composeBuf,
                        sizeof(view->composeBuf), Mesh::MAX_PAYLOAD);
    composing = true;
}

static void finishCompose(bool accepted) {
    composing = false;
    if (!view) return;
    if (!accepted || !view->composeBuf[0]) return;

    if (Mesh::sendTo(view->composeBuf, dmTarget)) {
        SFX::click();
        scrollFromBottom = 0;   // your own message pulls you back to the bottom
        pane = Pane::CHAT;      // watch it go rather than sitting on the roster
    } else if (dmTarget != Mesh::BROADCAST_ADDR &&
               Mesh::getCodec() != Mesh::Codec::PROTO) {
        // The transport refuses rather than quietly broadcasting a private
        // message, so this is the one place that has to explain why.
        Display::showToast(DM_NEEDS_PROTO, 2000);
    } else {
        Display::showToast("M3SH S3ND F41L3D", 1600);
    }
    view->composeBuf[0] = '\0';
}

// ==[ PUBLIC API ]==

void enter() {
    active = true;
    composing = false;
    // Both reset every time: a target chosen last visit would silently address
    // this visit's first message, and the roster order has moved since.
    pane = Pane::CHAT;
    dmTarget = Mesh::BROADCAST_ADDR;
    nodeSel = 0;
    nodeTop = 0;
    scrollFromBottom = 0;
    indexedRevision = 0xFFFFFFFFu;
    Mesh::consumeUnread();

    // The settings toggle owns the bridge lifecycle, same as C5. Entering the
    // screen only starts a link that is configured but not yet up — which is
    // the normal case when the toggle was flipped while sitting in settings.
    if (Config::getMeshEnabled() && !Mesh::isStarted()) {
        Mesh::begin(Config::getMeshRxPin(), Config::getMeshTxPin(),
                    Config::getMeshBaud(), Config::getMeshCodec());
    }
}

void exit() {
    active = false;
    if (composing) {
        SoftKeyboard::stop();
        composing = false;
    }
}

void update() {
    if (!active) return;

    if (composing) {
        SoftKeyboard::update();
        bool accepted = false;
        if (SoftKeyboard::consumeDone(accepted)) finishCompose(accepted);
        return;
    }

    syncIndex();
    Mesh::consumeUnread();   // sitting on this screen is reading it
}

static const char* portLabel() {
    return MeshUartPolicy::portLabel(Config::getMeshRxPin());
}

// ==[ AGE ]== relative, and it has to be: Message::atMs is millis(), which does
// not survive a reboot, and a TEXTMSG line carries no timestamp of its own to
// fall back on. Unsigned subtraction gives the right elapsed time across the
// 49-day millis() wrap, so the arithmetic needs no special case for it.
static void formatAge(char* out, size_t cap, uint32_t deltaMs) {
    const uint32_t sec = deltaMs / 1000u;
    if (sec < 60u) {
        snprintf(out, cap, "N0W");
    } else if (sec < 3600u) {
        snprintf(out, cap, "%luM", (unsigned long)(sec / 60u));
    } else if (sec < 172800u) {
        snprintf(out, cap, "%luH", (unsigned long)(sec / 3600u));
    } else {
        snprintf(out, cap, "%luD", (unsigned long)(sec / 86400u));
    }
}

// "-- 12M 4G0 ------------------..." across the full pane width. "N0W" is
// already an elapsed time, so pinning "4G0" to it produced "N0W 4G0".
static void buildDivider(char* out, size_t cap, const char* age) {
    char label[16];
    const bool relative = (strcmp(age, "N0W") != 0);
    const int labelLen = relative ? snprintf(label, sizeof(label), " %s 4G0 ", age)
                                  : snprintf(label, sizeof(label), " %s ", age);
    const size_t cols = (TOTAL_COLS < cap - 1) ? (size_t)TOTAL_COLS : cap - 1;
    memset(out, '-', cols);
    out[cols] = '\0';
    if (labelLen > 0 && (size_t)labelLen + 2 <= cols) {
        memcpy(out + 2, label, (size_t)labelLen);
    }
}

static void drawStatusStrip(M5Canvas& canvas, uint16_t bg, uint16_t dim,
                            uint16_t fg) {
    const uint32_t now = millis();
    char strip[64];
    bool alarm = false;

    if (!Config::getMeshEnabled()) {
        snprintf(strip, sizeof(strip), "M3SH 0FF - 3N4BL3 1N TUN3 P1G");
    } else if (!Mesh::isStarted()) {
        snprintf(strip, sizeof(strip), "BR1DG3 D0WN  %s", portLabel());
    } else if (Mesh::isUnparsed()) {
        // Bytes are arriving and not one of them has ever formed a line this
        // codec can read. Without saying so, this is indistinguishable from a
        // quiet mesh — the same W41T, the same empty scrollback — and the
        // setting that causes it is one screen away.
        alarm = true;
        snprintf(strip, sizeof(strip), "N0 P4RS3  %s  %lu  %luB", portLabel(),
                 (unsigned long)Mesh::getBaud(),
                 (unsigned long)Mesh::getRxBytes());
    } else {
        const char* state = "W41T";
        switch (Mesh::getLinkState(now)) {
            case Mesh::LinkState::LIVE: state = "L1V3"; break;
            case Mesh::LinkState::WAITING: state = "W41T"; break;
            case Mesh::LinkState::OFF: state = "0FF"; break;
        }
        // How long ago the cable last carried anything. LIVE/W41T is a
        // two-minute threshold and nothing more; on a quiet mesh the only
        // question worth answering is "how quiet, exactly".
        char heard[10] = "";
        const uint32_t lastRx = Mesh::getLastRxMs();
        if (lastRx != 0) {
            char age[8];
            formatAge(age, sizeof(age), (uint32_t)(now - lastRx));
            snprintf(heard, sizeof(heard), " %s", age);
        }
        // Q is the one number here that is about to change on its own: a
        // message cannot leave until the wire has been quiet for longer than
        // the radio's read window, so a queue is a wait, not a fault.
        char queued[8] = "";
        const uint8_t pending = Mesh::getTxPending();
        if (pending > 0) snprintf(queued, sizeof(queued), " Q%u", pending);

        // Under PROTO the port and baud matter far less than whether the
        // session ever opened, so the strip spends its width on that instead.
        char codecInfo[28];
        if (Mesh::getCodec() == Mesh::Codec::PROTO) {
            const char* ps = "?";
            switch (Mesh::getProtoState()) {
                case Mesh::ProtoState::OPENING: ps = "0P3N"; break;
                case Mesh::ProtoState::SYNCING: ps = "SYNC"; break;
                case Mesh::ProtoState::READY:   ps = "RDY";  break;
                default: break;
            }
            snprintf(codecInfo, sizeof(codecInfo), "PR0T0 %s N%u", ps,
                     (unsigned)Mesh::getNodeCount());
        } else {
            snprintf(codecInfo, sizeof(codecInfo), "T3XT %s", portLabel());
        }
        snprintf(strip, sizeof(strip), "%s%s  %s  %lu  R%u T%u%s", state, heard,
                 codecInfo, (unsigned long)Mesh::getBaud(),
                 (unsigned)Mesh::getRxMessages(),
                 (unsigned)Mesh::getTxMessages(), queued);
    }

    // Clipped rather than trusted: R/T counters and a node count all grow with
    // the session, and the tabs to the right are the only pane control there is.
    if (strlen(strip) > (size_t)STRIP_COLS) strip[STRIP_COLS] = '\0';
    canvas.setTextColor(alarm ? fg : dim, bg);
    canvas.drawString(strip, PANE_X, kTopBarH + 1);
}

// ==[ TABS ]== the active pane is the inverted pill, which is the one thing a
// reader does not have to be told how to read. STATUS_H is even and the glyph
// is 8 rows of ink, so the fill sits one row above and one below it.
static void drawPaneTabs(M5Canvas& canvas, uint16_t bg, uint16_t dim,
                         uint16_t fg) {
    struct Tab { const char* label; Pane pane; };
    static const Tab kTabs[2] = {{"MSG", Pane::CHAT}, {"N0D", Pane::NODES}};

    for (uint8_t i = 0; i < 2; ++i) {
        const int x = TABS_X + i * (TAB_W + TAB_GAP);
        const bool on = (pane == kTabs[i].pane);
        if (on) canvas.fillRect(x, kTopBarH, TAB_W, STATUS_H, fg);
        canvas.setTextColor(on ? bg : dim, on ? fg : bg);
        canvas.drawString(kTabs[i].label, x + 1, kTopBarH + 1);
    }
}

// ==[ POSITION ]== drawn only when the content actually overflows, because a
// full-height thumb says nothing and a scrollbar on a screen that cannot
// scroll is a control that lies. Thumb length is the fraction on screen;
// thumb position is how far through the rest you are.
static void drawScrollbar(M5Canvas& canvas, uint16_t bg, uint16_t dim,
                          uint16_t fg, uint16_t total, uint16_t visible,
                          uint16_t top, int trackY, int trackH) {
    if (total <= visible) return;

    canvas.fillRect(SCROLL_X, trackY, SCROLL_W, trackH,
                    Display::lerpColor565(dim, bg, 0.6f));

    int thumbH = (int)((long)trackH * visible / total);
    if (thumbH < 6) thumbH = 6;          // still grabbable on a long history
    if (thumbH > trackH) thumbH = trackH;
    const uint16_t span = (uint16_t)(total - visible);
    const int thumbY = trackY + (int)((long)(trackH - thumbH) * top / span);
    canvas.fillRect(SCROLL_X, thumbY, SCROLL_W, thumbH, fg);
}

// ==[ SIGNAL ]== the roster's SNR column. SNR in dB is the number the mesh
// actually turns on, and it is not a percentage: LongFast demodulates down to
// about -20dB and anything past +10 is a node in the same room. Five buckets is
// all a 6px cell can carry honestly, and inventing a sixth would be inventing
// precision.
static const char* snrGlyph(float snr) {
    if (snr >= 8.0f)  return "####";
    if (snr >= 2.0f)  return "###.";
    if (snr >= -5.0f) return "##..";
    if (snr >= -12.0f) return "#...";
    return "....";
}

// ==[ THE RAIL ]== the column between the name and the body. On an inbound line
// it carries how far the message travelled, which costs no width because the
// column was already there. A digit is hops; the plain rail means the codec
// never said — which is always the case under TEXTMSG.
static const char* inboundRail(const Mesh::Message& msg) {
    if (!msg.hasHops) return "|";
    switch (msg.hops) {
        case 0: return "|";   // direct neighbour — the common, quiet case
        case 1: return "1";
        case 2: return "2";
        case 3: return "3";
        default: return "+";
    }
}

static const char* railGlyph(const Mesh::Message& msg) {
    if (!msg.outgoing) return inboundRail(msg);
    switch (msg.txState) {
        case Mesh::TxState::QUEUED:    return ">";  // waiting out the pacing gap
        case Mesh::TxState::CONFIRMED: return "+";  // the radio echoed it back
        case Mesh::TxState::NO_ECHO:   return "?";  // and this one did not
        default:
            // SENT, and that is the whole of what TEXTMSG can tell us: the
            // bytes left this device. Nothing will ever confirm unless
            // serial.echo is set on the C6L, so the plain rail is the honest
            // mark rather than a fault invented out of a config default. The
            // transport decides when SENT becomes NO_ECHO — it is the only
            // thing that knows the echo window closed on a message that had
            // actually been written, rather than one still in the queue.
            return "|";
    }
}

// ==[ MESSAGE GROUPING ]== the first row names the speaker, but a long payload
// can run for six rows. A quiet one-pixel stem keeps those continuation rows
// attached to their sender without spending another text column. The last
// continuation row gets a short foot, so two adjacent wrapped messages never
// read as one tall rail; a one-line message keeps its hop/delivery glyph clean.
static void drawMessageRail(M5Canvas& canvas, const LineRef& ref,
                            const Mesh::Message& msg, int y, uint16_t bg,
                            uint16_t dim, uint16_t fg) {
    const int x = PANE_X + RAIL_COL * kCharWSize1;
    const uint16_t color = (msg.outgoing || msg.direct) ? fg : dim;

    if (ref.first) {
        canvas.setTextColor(color, bg);
        canvas.drawString(railGlyph(msg), x, y);
    } else {
        canvas.fillRect(x + 2, y, 1, LINE_H, color);
    }
    if (ref.last && !ref.first) {
        canvas.fillRect(x + 2, y + LINE_H - 2, 4, 1, color);
    }
}

// Our own name and an inbound DM are the two sender labels that carry state,
// so they get real chips. Ordinary arrivals remain quiet in the gutter; the
// contrast belongs to the words, not to sixty-four identical decorations.
static void drawSenderTag(M5Canvas& canvas, const Mesh::Message& msg, int y,
                          uint16_t bg, uint16_t dim, uint16_t fg) {
    char sender[SENDER_COLS + 1];
    strncpy(sender, msg.sender, SENDER_COLS);
    sender[SENDER_COLS] = '\0';

    if (msg.direct || msg.outgoing) {
        const uint16_t fill = msg.direct ? fg : dim;
        canvas.fillRect(PANE_X - 1, y, SENDER_COLS * kCharWSize1 + 2,
                        LINE_H - 1, fill);
        canvas.setTextColor(bg, fill);
    } else {
        canvas.setTextColor(dim, bg);
    }
    canvas.drawString(sender, PANE_X, y);
}

// ==[ EXPLANATION SCREENS ]== every empty or degraded state on this screen is
// the same shape — one line saying what is happening, then the lines saying
// what to do about it. They were each hand-placed and had drifted apart: one
// put a blank row under the headline and one did not, and all of them drew the
// headline in the same dim as the advice, so nothing led. One helper, one
// rhythm, and the headline carries the contrast it earns.
static void drawNotice(M5Canvas& canvas, uint16_t bg, uint16_t dim, uint16_t fg,
                       const char* headline, const char* l1 = nullptr,
                       const char* l2 = nullptr, const char* l3 = nullptr) {
    int y = PANE_Y + 4;
    canvas.setTextColor(fg, bg);
    canvas.drawString(headline, PANE_X, y);
    y += LINE_H * 2;   // one blank row, always

    const char* lines[3] = {l1, l2, l3};
    canvas.setTextColor(dim, bg);
    for (uint8_t i = 0; i < 3; ++i) {
        if (!lines[i]) continue;
        canvas.drawString(lines[i], PANE_X, y);
        y += LINE_H;
    }
}

// ==[ ROSTER ]== one row per node, densest-first: the name you would say out
// loud, then the three numbers that decide whether you can reach them.
static void drawNodes(M5Canvas& canvas, uint16_t bg, uint16_t dim,
                      uint16_t fg) {
    const uint32_t now = millis();
    const uint8_t  count = Mesh::getNodeCount();

    if (Mesh::getCodec() != Mesh::Codec::PROTO) {
        // Not a fault and not an empty mesh — the codec on the cable has no
        // field for any of this. Say which, and where the switch is.
        drawNotice(canvas, bg, dim, fg, "T3XTMSG C4RR13S N0 N0D3 L1ST",
                   "SW1TCH T0 PR0T0 1N TUN3 P1G",
                   "C6L N33DS S3R14L.M0D3=PR0T0 T00");
        return;
    }

    if (count == 0) {
        if (Mesh::getProtoState() == Mesh::ProtoState::OPENING) {
            // The handshake is retried, so this is a wait rather than a
            // failure — but it is also exactly what a radio whose serial.mode
            // is still TEXTMSG looks like forever.
            drawNotice(canvas, bg, dim, fg, "0P3N1NG S3SS10N...",
                       "N0 R3PLY M34NS C6L 1SNT 1N PR0T0");
        } else {
            drawNotice(canvas, bg, dim, fg, "N0 N0D3S H34RD Y3T",
                       "TH3 R0ST3R F1LLS 4S N0D3S SP34K");
        }
        return;
    }

    // ==[ COLUMN HEADER ]== four unlabelled columns of "##.. 0H N0W" is a
    // cipher on first sight, and this is the pane a reader arrives at looking
    // for exactly those numbers. One row is a cheap price for reading them.
    canvas.setTextColor(dim, bg);
    canvas.drawString(" N4M3 SNR  H0P S33N", PANE_X, PANE_Y);
    const int listY = PANE_Y + LINE_H + 1;

    // Keep the cursor on screen without moving it: the roster reorders itself
    // as nodes are heard, and a cursor that chased the list would wander.
    if (nodeSel >= count) nodeSel = (uint8_t)(count - 1);
    const uint8_t rows = (uint8_t)((VISIBLE_LINES - 1) / 2);
    if (nodeSel < nodeTop) nodeTop = nodeSel;
    if (nodeSel >= nodeTop + rows) nodeTop = (uint8_t)(nodeSel - rows + 1);
    if (nodeTop + rows > count) {
        nodeTop = (count > rows) ? (uint8_t)(count - rows) : 0;
    }

    int y = listY;
    for (uint8_t i = nodeTop; i < count && i < nodeTop + rows; ++i) {
        const Mesh::Node& n = Mesh::getNode(i);
        const bool sel = (i == nodeSel);
        const uint32_t age = (uint32_t)(now - n.lastSeenMs);
        const bool stale = age >= MeshUartPolicy::NODE_STALE_MS;

        if (sel) {
            // Even height, symmetric above and below the two text rows.
            canvas.fillRect(0, y - 1, canvas.width(), LINE_H * 2, dim);
        }
        canvas.setTextColor(sel ? bg : (stale ? dim : fg), sel ? dim : bg);

        // Row 1: name and how it is addressed.
        char line[64];
        char hops[6] = "--";
        if (n.hasHops) snprintf(hops, sizeof(hops), "%uH", (unsigned)n.hopsAway);
        char ageBuf[8];
        formatAge(ageBuf, sizeof(ageBuf), age);
        // The star is who the composer is aimed at, which is a different fact
        // from which row the cursor is on — you can scroll away from your DM
        // target without losing it.
        snprintf(line, sizeof(line), "%c%-4s %-4s %-3s %s",
                 (dmTarget == n.num) ? '*' : ' ',
                 n.shortName[0] ? n.shortName : "????",
                 snrGlyph(n.snr), hops, ageBuf);
        canvas.drawString(line, PANE_X, y);

        // Row 2: the long name, which is the one a human recognises, plus the
        // numbers behind the bar for anyone who wants them. One leading space,
        // not two: the short name above starts in column 1 because column 0 is
        // the DM star, and two spaces here left the two names a column apart.
        canvas.setTextColor(sel ? bg : dim, sel ? dim : bg);
        char sub[64];
        if (n.battery > 0 && n.battery <= 100) {
            snprintf(sub, sizeof(sub), " %.28s  %d%%", n.longName,
                     (int)n.battery);
        } else if (n.battery > 100) {
            // The radio reports >100 for a node running on USB rather than a
            // battery it can measure. Saying "101%" would be reporting a
            // sensor artefact as a fact.
            snprintf(sub, sizeof(sub), " %.28s  USB", n.longName);
        } else {
            snprintf(sub, sizeof(sub), " %.32s", n.longName);
        }
        canvas.drawString(sub, PANE_X, y + LINE_H);
        y += LINE_H * 2;
    }

    // After the rows, so the track owns its lane rather than being painted
    // over by a selected row that spans the full width.
    drawScrollbar(canvas, bg, dim, fg, count, rows, nodeTop, listY,
                  rows * LINE_H * 2);
}

// ==[ CONTROLS ]== drawn for both panes. The roster used to return early and
// take the bottom bar with it, so the one pane whose controls are not obvious
// — where the cursor picks a node and OK aims the composer at it — was also
// the one pane that offered no hints and no bottom edge.
static void drawControlBar(M5Canvas& canvas) {
    if (Display::drawHintBottomBar(&canvas)) return;

    // The composer's target is the one thing worth spending the middle slot on:
    // a DM that reads like a broadcast until you have already sent it is the
    // mistake this screen most needs to prevent.
    char act[20];
    if (dmTarget != Mesh::BROADCAST_ADDR) {
        snprintf(act, sizeof(act), "[B]DM %s", dmName());
    } else {
        snprintf(act, sizeof(act), "[B]WR1T3");
    }

    // [B] is OK and [C] is back everywhere else on this device, and long OK
    // wipes the scrollback — so a bar that says [B+]3X1T is an invitation to
    // lose the history while reaching for the door.
    Display::drawBottomBar3To(&canvas,
                              (pane == Pane::NODES) ? "[A/C]P1CK" : "[A/C]SCR0LL",
                              act, "[C+]3X1T");
}

// ==[ THIS FUNCTION OWNS THE CLEAR ]== Display::drawMeshScreen deliberately
// does not pre-fill the sprite, so every path out of here has to leave it
// painted — including the ones that draw nothing.
void draw(M5Canvas& canvas) {
    if (!active) {
        canvas.fillSprite(Display::getColorBG());
        return;
    }

    if (composing) {
        SoftKeyboard::draw(canvas);   // fills the sprite itself
        Display::drawUiOverlaysTo(&canvas);
        return;
    }

    const uint16_t fg = Display::getColorFG();
    const uint16_t bg = Display::getColorBG();
    const uint16_t dim = Display::lerpColor565(fg, bg, 0.48f);

    canvas.fillSprite(bg);
    Display::drawStatusBarTo(&canvas, "M3SH T4LK");

    // After the status bar, not before: it draws at its own size, and every
    // column measurement below assumes the 6x8 cell.
    canvas.setFont(&fonts::Font0);
    canvas.setTextSize(1);
    canvas.setTextDatum(TL_DATUM);

    drawStatusStrip(canvas, bg, dim, fg);
    drawPaneTabs(canvas, bg, dim, fg);

    if (pane == Pane::NODES) {
        drawNodes(canvas, bg, dim, fg);
        drawControlBar(canvas);
        Display::drawUiOverlaysTo(&canvas);
        return;
    }

    syncIndex();

    if (lineCount == 0) {
        if (Mesh::isUnparsed()) {
            // Reachable precisely because the transport refuses to file lines
            // it cannot parse: on a mismatched link the ring stays empty
            // instead of filling with mojibake, so this is the screen you get
            // rather than sixty-four rows of noise. Both causes live on the
            // same screen of the same config, so name the place rather than
            // guess which of the two it is.
            //
            // Which mode to name depends on which one we are speaking, and
            // naming the wrong one sends the reader to undo a correct setting.
            drawNotice(canvas, bg, dim, fg, "BYT3S 4RR1V3, N0N3 P4RS3",
                       "C6L S3R14L.B4UD MUST M4TCH M3SH B4UD",
                       Mesh::getCodec() == Mesh::Codec::PROTO
                           ? "C6L S3R14L.M0D3 MUST B3 PR0T0"
                           : "C6L S3R14L.M0D3 MUST B3 T3XTMSG");
        } else {
            drawNotice(canvas, bg, dim, fg, "N0 TR4FF1C Y3T",
                       Mesh::getCodec() == Mesh::Codec::PROTO
                           ? "0K C0MP0S3S. N0D T4B L1STS TH3 M3SH."
                           : "0K C0MP0S3S. BR04DC4ST 0NLY.");
        }
    } else {
        const uint16_t endLine = (uint16_t)(lineCount - scrollFromBottom);
        const uint16_t startLine = topVisibleLine();

        // ==[ BOTTOM ANCHORED ]== the newest line sits on the last row of the
        // pane whether there are two lines or two hundred. Filling from the top
        // instead left a short conversation stranded under the status strip
        // with two thirds of the screen empty below it, and moved the newest
        // line every time one arrived — a chat reads from a fixed bottom edge.
        const uint16_t drawn = (uint16_t)(endLine - startLine);
        int y = PANE_Y + 2 + (VISIBLE_LINES - drawn) * LINE_H;
        char cell[BODY_COLS + 1];
        const uint32_t now = millis();

        for (uint16_t i = startLine; i < endLine; ++i) {
            const LineRef& ref = view->lines[i];
            const Mesh::Message& msg = Mesh::getMessage(ref.msgIdx);

            if (ref.divider) {
                char age[8];
                char rule[TOTAL_COLS + 1];
                formatAge(age, sizeof(age), (uint32_t)(now - msg.atMs));
                buildDivider(rule, sizeof(rule), age);
                canvas.setTextColor(dim, bg);
                canvas.drawString(rule, PANE_X, y);
                y += LINE_H;
                continue;
            }

            if (ref.first) {
                // ==[ DIRECT ]== a message addressed to this node and not to
                // the whole mesh. TEXTMSG delivered these too and had no way
                // to say so, which meant a private message read exactly like a
                // broadcast. The sender chip is the loudest mark available in
                // four columns, and it costs no message width at all.
                drawSenderTag(canvas, msg, y, bg, dim, fg);
            }
            drawMessageRail(canvas, ref, msg, y, bg, dim, fg);

            memcpy(cell, msg.body + ref.start, ref.len);
            cell[ref.len] = '\0';
            canvas.setTextColor(fg, bg);
            canvas.drawString(cell, PANE_X + GUTTER_COLS * kCharWSize1, y);

            y += LINE_H;
        }

        drawScrollbar(canvas, bg, dim, fg, lineCount, VISIBLE_LINES, startLine,
                      PANE_Y + 2, VISIBLE_LINES * LINE_H);
    }

    drawControlBar(canvas);
    Display::drawUiOverlaysTo(&canvas);
}

// ==[ INPUT HELPERS ]== both of these are reached from a button and from a
// fingertip, so they sit ahead of every handler rather than beside one.

// Switching views is the one control that has to exist in both of them, so it
// lives on the strip that is drawn in both. Cheap to hit, impossible to hit by
// accident while reading.
void togglePane() {
    pane = (pane == Pane::CHAT) ? Pane::NODES : Pane::CHAT;
    SFX::click();
}

// Addressing the composer is a roster action, not a composer one: you pick who
// you are talking to by pointing at them.
static void aimAtSelectedNode() {
    if (Mesh::getCodec() != Mesh::Codec::PROTO) {
        Display::showToast(DM_NEEDS_PROTO, 2000);
        return;
    }
    if (nodeSel >= Mesh::getNodeCount()) return;
    const Mesh::Node& n = Mesh::getNode(nodeSel);
    // Tapping the node you are already aimed at aims back at everyone, so the
    // way out of a DM is the same gesture as the way in.
    dmTarget = (dmTarget == n.num) ? Mesh::BROADCAST_ADDR : n.num;
    SFX::click();
}

void handleBtnOK(bool longPress) {
    if (!active || composing) return;

    if (longPress) {
        // Clearing is a chat action. On the roster it would silently wipe a
        // scrollback the reader is not even looking at.
        if (pane == Pane::NODES) {
            togglePane();
            return;
        }
        Mesh::clear();
        scrollFromBottom = 0;
        indexedRevision = 0xFFFFFFFFu;
        Display::showToast("SCR0LLB4CK CL34R3D", 1200);
        SFX::click();
        return;
    }
    if (pane == Pane::NODES) {
        aimAtSelectedNode();
        startCompose();
        return;
    }
    startCompose();
}

void handleBtnBack(bool longPress) {
    if (!active || composing) return;

    if (longPress) {
        Hamlet::enterMode(HamletMode::MENU);
        return;
    }
    scrollUp();
}

// Both clamp against maxScroll(), which is only as current as the index. Input
// runs before the frame is drawn, so without this the first press after an
// arrival scrolls against last frame's line count. syncIndex() is revision
// gated, so on the common path this costs a comparison.
void scrollUp() {
    if (!active || composing) return;
    if (pane == Pane::NODES) {
        if (nodeSel > 0) nodeSel--;
        return;
    }
    syncIndex();
    if (scrollFromBottom < maxScroll()) scrollFromBottom++;
}

void scrollDown() {
    if (!active || composing) return;
    if (pane == Pane::NODES) {
        const uint8_t count = Mesh::getNodeCount();
        if (count && nodeSel + 1 < count) nodeSel++;
        return;
    }
    syncIndex();
    if (scrollFromBottom > 0) scrollFromBottom--;
}

void handleTouch(int x, int y) {
    if (!active || composing) return;

    // The strip is the view toggle in both views. Landing on a tab selects
    // that pane outright rather than flipping — pressing the tab you are
    // already on should keep you there, which is what a tab does everywhere
    // else. Anywhere else on the strip stays a toggle, because the telemetry
    // is a large target and a reader aiming roughly at it means "switch".
    if (y >= kTopBarH && y < PANE_Y) {
        if (x >= TABS_X) {
            const Pane want =
                (x < TABS_X + TAB_W + TAB_GAP) ? Pane::CHAT : Pane::NODES;
            if (want != pane) togglePane();
            return;
        }
        togglePane();
        return;
    }
    if (y < PANE_Y || y >= PANE_Y + PANE_H) return;

    if (pane == Pane::NODES) {
        // Two text rows per node below the column header, so the row under the
        // finger is half the offset from the list top — the same arithmetic
        // drawNodes lays them out with. A tap on the header itself is not a row.
        const int listY = PANE_Y + LINE_H + 1;
        if (y < listY) return;
        const uint8_t row = (uint8_t)((y - listY) / (LINE_H * 2));
        const uint8_t idx = (uint8_t)(nodeTop + row);
        if (idx < Mesh::getNodeCount()) {
            if (idx == nodeSel) {
                aimAtSelectedNode();
            } else {
                nodeSel = idx;
                SFX::click();
            }
        }
        return;
    }

    // The pane scrolls by swipe, so a tap has exactly one job. Anywhere in the
    // message area opens the composer; the bars keep their own meanings.
    startCompose();
}

bool isActive() { return active; }
bool isComposing() { return composing; }

}  // namespace MeshMenu
