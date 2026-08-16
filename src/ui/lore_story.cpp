#include "lore_story.h"

namespace LoreStory {

// ==[ THE 0CT0 FILES ]== one damaged transmission per visit. linear, then looped.
static const Fragment FRAGMENTS[] = {
    {
        "03:17 // LAB",
        "NINE WARNINGS",
        "0ct0 arrived on twenty minutes of sleep. The compiler had nine warnings and one clean "
        "alibi. He ignored the alibi. By 03:19 the bug had fingerprints. They matched 0ct0."
    },
    {
        "03:41 // BENCH",
        "THE BISCUIT CHAMBER",
        "The test required an RF chamber. Finance produced a biscuit tin and two clips. Channel "
        "six dropped 11 dB inside it. Ugly evidence still counts. Evidence filed."
    },
    {
        "04:06 // CH 6",
        "PACKET WITH NO ALIBI",
        "A frame arrived empty every 997 ms. Too punctual for noise. 0ct0 blamed the router, the "
        "moon, then physics. Pancetta circled the timer callback. Case open."
    },
    {
        "04:32 // /DEV/PIG",
        "PANCETTA BOOTS",
        "0ct0 gave the anomaly a snout, promiscuous mode, and one job: follow the evidence. The "
        "empty coffee mug abstained. Pancetta opened one eye. Channel one started talking."
    },
    {
        "05:10 // KITCHEN",
        "REPRODUCIBLE DISORDER",
        "Three antennas entered the cable drawer. Five came out, none labeled, one warm. 0ct0 called "
        "it entropy. Pancetta called it chain-of-custody failure with noodles."
    },
    {
        "05:52 // MODEL A",
        "ONE HUNDRED PERCENT",
        "The classifier scored 100 percent by marking every beacon hostile. It also marked 0ct0. "
        "The model was useless, the last result held up, and nobody enjoyed peer review."
    },
    {
        "06:23 // QUBIT BAR",
        "BACKUP CONSPIRACY",
        "0ct0 kept four backups on three media. Every copy carried the same one-byte typo. Redundancy "
        "had protected the crime scene. Pancetta restored yesterday and questioned nobody."
    },
    {
        "07:01 // ROOFTOP",
        "THE INVISIBLE AP",
        "No SSID. No clean bearing. Just clients bending around an empty channel like rain around a "
        "body. Pancetta logged the disturbance. The wire remembers."
    },
    {
        "22:48 // DAY TWO",
        "SLEEP PATCH REJECTED",
        "0ct0 read that sleep clears waste, then stayed awake to study it. At hour 31 he fixed the "
        "same line twice and broke it both ways. Pancetta entered fatigue as root cause."
    },
    {
        "23:16 // SCOPE",
        "SIX MILLIMETERS",
        "The bench moved six millimeters every 43 seconds. 0ct0 drafted a theory about spacetime. "
        "Pancetta followed the vibration through one wall and arrested the washing machine."
    },
    {
        "00:04 // CULTURE",
        "ONE LETTER MURDER",
        "0ct0 changed one letter in a build flag. Flash grew 312 KB, RAM vanished, and the linker "
        "left a chalk outline. He called it a typo. Pancetta booked the bastard byte."
    },
    {
        "01:39 // MIRROR",
        "SECOND RUN, NEW TRUTH",
        "The same test returned two answers. 0ct0 called it emergence. Pancetta found an uninitialized "
        "byte, motive, and opportunity. Undefined behavior confessed before breakfast."
    },
    {
        "02:20 // UPLINK",
        "FIELD NOTE AT 02:20",
        "The report kept the commands, offsets, failure, and fix. Everything else got the knife. "
        "Footnote one named the remaining doubt. Footnote two named 0ct0."
    },
    {
        "03:03 // FLOCK",
        "PEER REVIEW ARRIVES",
        "Four radios answered with partial evidence, bad clocks, and full confidence. FLOCKNOW kept "
        "the checksums and cut the swagger. Nobody admitted leadership. Good protocol."
    },
    {
        "03:33 // INCIDENT",
        "THE DEMO EFFECT",
        "The build ran clean for 11 hours. One witness entered. A null pointer took the stand, killed "
        "the demo, and escaped through serial. 0ct0 had removed the logs that morning."
    },
    {
        "04:12 // HOTFIX",
        "THE LOGS GET A CLOCK",
        "The old log said radio go brrr, then nothing. Timestamps put the murder at 04:09:17, exactly "
        "five seconds after NVS wrote. The debounce had motive. 0ct0 supplied opportunity."
    },
    {
        "05:45 // DAWN",
        "THE WORKING THEORY",
        "Pancetta concluded intelligence is noise with coffee and a stack trace. 0ct0 objected, "
        "walked into a door marked PULL, and revised the paper from the floor."
    },
    {
        "06:00 // CASE OPEN",
        "NO FINAL REPORT",
        "Dawn found 18 reports, three regressions, and one pig still listening. 0ct0 shipped the fix. "
        "Pancetta saved the receipts, ordered breakfast, and left the case open."
    },
};
static_assert(sizeof(FRAGMENTS) / sizeof(FRAGMENTS[0]) > 0,
              "lore needs at least one transmission");

// These files unlock from witnessed behavior, not elapsed ABOUT visits. Pig's
// ordinary habits are the evidence: each one remains available after reboot.
static const Fragment PIG_MEMORIES[] = {
    {
        "PIG // CHEEK",
        "FACE FIRST",
        "Pig met a face with his own face. No warning, no paperwork, just one firm bump and a purr. Pancetta logged it as trust arriving head first."
    },
    {
        "PIG // PILLOW",
        "THE WARMEST HAT",
        "Pig climbed onto Pancetta's head, turned twice, and slept. The case went nowhere for a while. Some weight is not a burden. It is company."
    },
    {
        "PIG // PAWS",
        "BISCUIT SHIFT",
        "Pig kneaded the softest place in reach with grave attention. Left paw, right paw, repeat. The old comfort protocol still compiled without warnings."
    },
    {
        "PIG // EYES",
        "THE SLOW ANSWER",
        "Pig looked across the room and closed both eyes slowly. No alarm lived there. Pancetta answered the same way. Two detectives agreed to be safe."
    },
    {
        "PIG // UPLINK",
        "BINARY MEOW",
        "Pig sent four bytes in mews and mrrs. The checksum was mostly whiskers. Pancetta kept every packet. Meaning is allowed to arrive without a decoder."
    },
    {
        "PIG // 03:00",
        "NIGHT CIRCUIT",
        "Pig crossed the room at impossible speed, pursued by evidence nobody else could see. The midnight circuit ended exactly where it began: innocent."
    },
    {
        "PIG // EXHIBIT",
        "HAIRBALL EVIDENCE",
        "Pig produced one damp exhibit and left the scene. Pancetta filed it without enthusiasm. Good cats make a little mess. Good friends remember all of it."
    },
    {
        "PIG // VOICE",
        "ONE CLEAR WORD",
        "Pig opened his mouth and stated the whole case out loud. Twice, in case the first one was evidence. Nothing survived translation. The meaning arrived anyway."
    },
};
static_assert(sizeof(PIG_MEMORIES) / sizeof(PIG_MEMORIES[0]) ==
                  (size_t)PancettaCat::Memory::COUNT,
              "every durable cat memory needs one lore file");

size_t count() {
    return sizeof(FRAGMENTS) / sizeof(FRAGMENTS[0]);
}

const Fragment& get(uint32_t sequence) {
    return FRAGMENTS[sequence % count()];
}

size_t memoryCount() {
    return sizeof(PIG_MEMORIES) / sizeof(PIG_MEMORIES[0]);
}

const Fragment& getMemory(PancettaCat::Memory memory) {
    const uint8_t index = (uint8_t)memory;
    return PIG_MEMORIES[index < memoryCount() ? index : 0u];
}

bool firstUnreadMemory(uint32_t observedMask, uint32_t seenMask,
                       PancettaCat::Memory& out) {
    const uint32_t unread = (observedMask & PancettaCat::kAllMemoryBits) &
                            ~(seenMask & PancettaCat::kAllMemoryBits);
    for (uint8_t i = 0; i < (uint8_t)PancettaCat::Memory::COUNT; ++i) {
        const PancettaCat::Memory candidate =
            (PancettaCat::Memory)i;
        if (unread & PancettaCat::memoryBit(candidate)) {
            out = candidate;
            return true;
        }
    }
    out = PancettaCat::Memory::COUNT;
    return false;
}

}  // namespace LoreStory
