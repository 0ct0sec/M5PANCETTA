#include "npc_events_core.h"

namespace NpcEventsCore {

// ==[ FOLLOW-UP BEATS ]==
// Each file runs root -> mid -> close. Nodes 1..3 are the mid beat for the
// matching root choice; node 4 is the closing beat, reachable from the first
// choice of every mid. Follow-up choices author two reply variants and leave
// the rest null; replyText() falls back to variant 0.

static const CaseNode K_HORSE_NODES[] = {
    {   // 1 - the mane came back as cable
        {"The braid is CAT6 all the way down. Something is still passing traffic through a witness.",
         "Pancetta holds a strand. It is warm. Warm cable means somebody is still talking."},
        {
            {"TRACE THE BRAID",
             "The traced braid gives up one endpoint and one grudge.",
             {"The strand terminates behind the antenna, in a barn that is not on any floor plan.",
              "Two hops later the braid reaches a switch nobody bought. It has a login banner."},
             11, 2, 16, 2, 1, 4},
            {"CUT ONE STRAND",
             "The cut strand ends the traffic and the testimony together.",
             {"Traffic stops. So does the witness. Pancetta files both losses under impatience.",
              "The braid parts cleanly. Whatever was talking now has an excellent alibi."},
             7, -1, 10, 1, -1, CASE_CLOSED},
            {"BAG IT UNREAD",
             "The sealed strand keeps its chain of custody and its secret.",
             {"Evidence bag, label, date. The braid stays whole and stays quiet. Admissible beats interesting.",
              "Pancetta seals it unread. The lab is three weeks out; the case is patient."},
             9, 1, 12, 2, 0, CASE_CLOSED},
        },
    },
    {   // 2 - the leak moved
        {"The connector is dry and the floor is wet. Water obeys gravity, not the patch notes.",
         "Pressure holds at the joint. Twelve inches downstream, the puddle is winning."},
        {
            {"FOLLOW THE WATER",
             "Following the water finds the real joint and the real culprit.",
             {"The puddle leads back under the bench, to a fitting nobody documented and everybody used.",
              "Water traces to a seam behind the rack. The seam has been leaking since before the ticket."},
             12, 2, 18, 3, 1, 4},
            {"TAPE IT HARDER",
             "More tape buys silence now and a bigger invoice later.",
             {"Three wraps. The leak relocates rather than stops. Pancetta logs it as a transfer, not a fix.",
              "The tape holds. The pressure finds the next weakest thing, which is always the cheapest thing."},
             6, 0, 8, 1, -1, CASE_CLOSED},
            {"DECLARE IT DRY",
             "A dry declaration closes the ticket and opens the next one.",
             {"Pancetta signs it dry. The floor disagrees in writing, slowly, overnight.",
              "Closed as resolved. The puddle files an appeal by morning."},
             5, -2, 6, 0, -1, CASE_CLOSED},
        },
    },
    {   // 3 - the barn has coordinates
        {"K-H0RS3 draws four walls and coils into the fifth. The floor plan has one door too many.",
         "The barn sketch matches the bar's back room. Neither building admits to the other."},
        {
            {"WALK THE BARN",
             "The walked barn puts Pancetta inside bad geometry.",
             {"Four walls, five corners, one hose. The room is bigger inside than the invoice allows.",
              "Pancetta paces it out. The fifth wall is where the uplink comes from and nobody built it."},
             13, 3, 20, 3, 1, 4},
            {"CALL THE BARMAN",
             "The Barman now knows Pancetta is asking about the barn.",
             {"The Barman listens, says BARN once, and hangs up. That is either help or a warning.",
              "Red eyes on the other end of the line. He confirms nothing and remembers everything."},
             8, 1, 14, 2, 0, CASE_CLOSED},
            {"FILE THE PLAN",
             "The filed plan waits in the locker for a case that needs it.",
             {"Sketch goes in the drawer. Zoning will lose it within the week; the copy stays honest.",
              "Filed and dated. Somewhere a building keeps not existing on schedule."},
             7, 1, 9, 1, 0, CASE_CLOSED},
        },
    },
    {   // 4 - close
        {"K-H0RS3 stops leaking long enough to look at Pancetta. It knows whose case this was.",
         "The hose goes quiet. For the first time the front half and the rear half agree on something."},
        {
            {"NAME THE OWNER",
             "The named owner turns a witness into a case with a defendant.",
             {"Pancetta says the name. The horse does not deny it. Neither does the barn.",
              "The owner is on the invoice, the switch, and the lease. Three documents, one signature."},
             18, 4, 30, 3, 1, CASE_CLOSED},
            {"LET IT LEAVE",
             "Letting it go keeps the witness alive and the file open.",
             {"The horse leaves damp and unindicted. Pancetta keeps the strand and the address.",
              "It goes back through the uplink. The case stays open, which is not the same as unsolved."},
             12, 3, 20, 2, 0, CASE_CLOSED},
            {"KEEP THE HOSE",
             "The kept hose is evidence, plumbing, and a very poor houseguest.",
             {"The rear half stays. It drips on the bench and answers to nothing. Pancetta calls it custody.",
              "Half a witness in the evidence locker. Legal is going to have opinions."},
             14, 2, 26, 2, -1, CASE_CLOSED},
        },
    },
};

static const CaseNode WISE_PIG_NODES[] = {
    {   // 1 - correct answer earns a harder one
        {"The Wise Pig nods and asks again. Thirteen doors, but only three ever open at once without collision.",
         "Correct. So the next one: name the three that never overlap, or name why anyone still argues about it."},
        {
            {"ONE, SIX, ELEVEN",
             "The exact triple earns the off-panel pig's respect and the bridge.",
             {"Correct. Five channels apart, twenty-two megahertz wide, and everyone still camps on six.",
              "The unseen pig stamps it. Non-overlapping and non-negotiable. The toll booth closes for good."},
             16, 4, 26, 3, 1, 4},
            {"ALL OF THEM",
             "The greedy answer costs the bridge and buys a lecture.",
             {"Thirteen doors, thirteen collisions. The Wise Pig charges for the lecture and the bridge.",
              "Everything overlaps everything. Somewhere a spectrum analyzer starts crying quietly."},
             6, -1, 8, 0, -1, CASE_CLOSED},
            {"ASK FOR A HINT",
             "The hint is free once and expensive afterwards.",
             {"The hint is the number five. It is somehow both generous and useless.",
              "The Wise Pig holds up five fingers. He does not have five fingers. Pancetta lets it go."},
             9, 1, 12, 1, 0, CASE_CLOSED},
        },
    },
    {   // 2 - partial credit, toll still open
        {"The toll stays open. The Wise Pig offers a trade: one honest wrong answer buys one real question.",
         "Partial credit is still credit. The bridge wants the rest of it before Pancetta crosses."},
        {
            {"ADMIT THE GUESS",
             "The admitted guess buys a real answer and a real bridge.",
             {"Pancetta says it was a guess. The Wise Pig respects the receipt more than the answer.",
              "Honesty logged. The toll drops to nothing and the riddle gets harder for free."},
             13, 3, 20, 3, 1, 4},
            {"PAY THE TOLL",
             "The paid toll closes the question and opens the wallet.",
             {"Coins change hands. The riddle stays unanswered and stays smug.",
              "Pancetta pays. The bridge opens. Nothing is learned, which is the expensive part."},
             7, 0, 10, 1, 0, CASE_CLOSED},
            {"WALK AROUND",
             "The long way costs time and leaves the riddle undefeated.",
             {"There is no around. Pancetta finds this out after eleven minutes and one wet sock.",
              "The detour rejoins the same bridge. The Wise Pig has not moved and has not aged."},
             5, -1, 6, 0, -1, CASE_CLOSED},
        },
    },
    {   // 3 - wet answer, filed under plumbing
        {"The Wise Pig marks the answer damp. Off-panel, a router is being towelled. Somebody wants Pancetta to explain the moisture.",
         "Filed under plumbing. The unseen pig slides over a bucket and waits for a better theory."},
        {
            {"BLAME K-H0RS3",
             "Blaming the hose connects two files with one puddle.",
             {"The Wise Pig accepts the theory. The puddle had a witness and the witness had a rear half.",
              "Two cases, one leak. Pancetta staples them together and the riddle finally makes sense."},
             14, 3, 22, 3, 1, 4},
            {"DRY THE ROUTER",
             "The dried router works, which proves nothing and helps everything.",
             {"Rice, patience, forty hours. It boots. The riddle remains unanswered and the AP remains employed.",
              "Pancetta towels it off. The link comes up. Nobody learns anything and everybody is fine."},
             9, 2, 14, 2, 0, CASE_CLOSED},
            {"KEEP THE BUCKET",
             "The bucket is the locker's least dignified exhibit.",
             {"Pancetta takes the bucket. It is the only object here with a consistent story.",
              "One bucket, logged. The Wise Pig writes something down and does not share it."},
             7, 1, 10, 1, -1, CASE_CLOSED},
        },
    },
    {   // 4 - close
        {"The off-panel pig finally steps into frame. He has been holding the answer sheet the whole time.",
         "The second pig arrives with exact change and a grievance about being called unseen for six files."},
        {
            {"READ THE SHEET",
             "The answer sheet closes the riddle and opens a longer argument.",
             {"Every answer was on it. So was Pancetta's name, written before the first question.",
              "The sheet is correct, complete, and dated last week. Somebody rehearsed this hallway."},
             20, 4, 32, 3, 1, CASE_CLOSED},
            {"REFUSE THE SHEET",
             "Refusing the sheet keeps the crossing earned.",
             {"Pancetta hands it back unread. The Wise Pig looks genuinely surprised, then genuinely pleased.",
              "No shortcut. The bridge opens anyway, which was apparently the actual test."},
             17, 5, 24, 3, 1, CASE_CLOSED},
            {"POCKET THE SHEET",
             "The pocketed sheet is useful, portable, and slightly criminal.",
             {"It goes in the coat. Future riddles are now a formality and a small moral problem.",
              "Pancetta keeps it. Two pigs watch him do it and neither files a complaint."},
             15, 2, 28, 2, -1, CASE_CLOSED},
        },
    },
};

static const CaseNode COW_NODES[] = {
    {   // 1 - signature verified, subject hungry
        {"The hoofprint verifies against MOO CA. The cow does not. Identity is pasture-based and the pasture is a roof.",
         "Signature valid, issuer unknown. The cow eats the certificate while Pancetta reads it."},
        {
            {"TRACE THE ISSUER",
             "The traced issuer turns a stray process into a supply chain.",
             {"MOO CA resolves to a barn. The barn resolves to a case Pancetta already has open.",
              "The issuer has signed four hundred cows. Three hundred of them have never seen grass."},
             12, 2, 20, 3, 1, 4},
            {"REVOKE IT",
             "Revocation is clean, correct, and deeply unpopular with the cow.",
             {"Certificate revoked. The cow remains. Trust is a document; mass is not.",
              "Pancetta pulls the cert. Nothing physical changes, which is the entire problem with PKI."},
             9, 1, 12, 2, 0, CASE_CLOSED},
            {"TRUST IT ONCE",
             "One-time trust is how every supply chain incident starts.",
             {"Pancetta trusts it exactly once. The cow immediately requests a second exception.",
              "Trusted on first use. The cow understood TOFU faster than anyone is comfortable with."},
             8, 2, 14, 1, -1, CASE_CLOSED},
        },
    },
    {   // 2 - least privilege held
        {"The cow accepts a service account and asks, politely, what wheel is. Least privilege is holding but curiosity is not.",
         "Root refused. The cow now reads grass and writes only to the incident log, which it is filling."},
        {
            {"AUDIT THE LOG",
             "The audited log shows what the cow actually wanted the whole time.",
             {"Four hundred entries. Three hundred are grass. The rest are attempts to reach the barn.",
              "The log is honest, verbose, and quietly damning. The cow has been mapping the roof."},
             13, 2, 22, 3, 1, 4},
            {"GRANT WHEEL",
             "Granting wheel is a decision defended badly, later.",
             {"Escalation approved. The cow immediately remembers it has four legs and no hands.",
              "Wheel granted. Nothing happens for six seconds. Then the roof access log gets interesting."},
             8, -1, 18, 1, -1, CASE_CLOSED},
            {"ROTATE THE KEY",
             "The rotated key keeps the cow cooperative and the account boring.",
             {"New key, same cow, fewer questions. Boring is the correct outcome for a service account.",
              "Pancetta rotates it. The cow does not notice, which is exactly the point."},
             10, 2, 14, 2, 1, CASE_CLOSED},
        },
    },
    {   // 3 - catapulted back
        {"The cow clears the parapet. The sky logs packet loss and reopens the ownership ticket with Pancetta's name on it.",
         "Airborne. Somewhere a distant castle updates a spreadsheet and assigns blame downward."},
        {
            {"ANSWER THE TICKET",
             "Answering the sky makes Pancetta a party, not a suspect.",
             {"Pancetta replies in writing. The sky responds with a second cow and a revised estimate.",
              "The ticket gets a real answer. The castle escalates, which at least means somebody is reading."},
             14, 3, 24, 3, 1, 4},
            {"IGNORE THE SKY",
             "Ignoring the sky is free until the next delivery.",
             {"No reply sent. The ticket ages. Weather remains unindicted and undeterred.",
              "Pancetta closes the tab. The sky has excellent retry logic and infinite patience."},
             7, 0, 10, 1, 0, CASE_CLOSED},
            {"BILL THE CASTLE",
             "The invoice makes an enemy with a trebuchet and a budget.",
             {"One roof, one cow, one incident. The castle disputes the line item about dignity.",
              "Pancetta bills for structural review. The reply arrives by air, as expected."},
             11, 2, 20, 2, -1, CASE_CLOSED},
        },
    },
    {   // 4 - close
        {"The barn on the roof plan is the barn from the horse's sketch. Two files, one building, no permits.",
         "Everything points at the same unlisted structure. The cow came from somewhere and it was not the sky."},
        {
            {"RAID THE BARN",
             "The raid turns two loose files into one case with an address.",
             {"Inside: four hundred certificates, one printer, and a great deal of grass. The chain ends here.",
              "Pancetta walks in. The barn is a signing authority with a hayloft and no oversight."},
             19, 4, 32, 3, 1, CASE_CLOSED},
            {"WATCH THE BARN",
             "Surveillance keeps the barn honest and the file open.",
             {"Pancetta parks across the road. Two cows leave in the first hour. Neither is signed.",
              "Passive observation. The barn does not know it is a suspect and behaves accordingly."},
             15, 3, 24, 3, 1, CASE_CLOSED},
            {"BURN THE PLAN",
             "The burned plan saves a witness, loses the address.",
             {"The sketch goes. So does the only map anyone had. The horse looks relieved.",
              "Pancetta destroys it. Somebody gets to keep breathing; the file gets to stay open forever."},
             12, 1, 20, 1, -1, CASE_CLOSED},
        },
    },
};

static const CaseNode DR_OCULUS_NODES[] = {
    {   // 1 - second opinion
        {"The second opinion agrees with the first in a less judgmental font. Both blame the board; neither blames the keyboard.",
         "Two diagnoses, one symptom. Dr. Oculus suggests a third lens and a fourth invoice."},
        {
            {"CHECK THE COMMIT",
             "The commit log names the cause and the cause has a keyboard.",
             {"git blame returns a name, a date, and a message reading 'temp fix'. It is nine months old.",
              "The commit is two lines and one lie. The silicon has been carrying it ever since."},
             13, 2, 20, 3, 1, 4},
            {"ORDER A THIRD",
             "The third opinion is thorough, expensive, and identical.",
             {"Same datasheet, new lens, larger bill. Dr. Oculus calls this consensus.",
              "Three doctors, one solder joint. The joint asks for patient confidentiality again."},
             8, 0, 12, 1, 0, CASE_CLOSED},
            {"REPLACE THE BOARD",
             "A new board hides the bug and keeps the receipt.",
             {"Fresh silicon, same firmware, same fault at the same address. The board was never the patient.",
              "Pancetta swaps it. The symptom returns in four minutes, well rested."},
             7, -1, 14, 1, -1, CASE_CLOSED},
        },
    },
    {   // 2 - G15 confessed
        {"G15 confessed immediately, which is what worries Pancetta. G13 has counsel and an alibi.",
         "The pin gave it up too fast. Dr. Oculus notes that eager witnesses are usually covering for the silkscreen."},
        {
            {"CROSS THE SILKSCREEN",
             "Cross-examining the silkscreen finds the revision that lied.",
             {"Rev C says G15. Rev D says G13. The board in hand says neither and has no rev marking.",
              "The silkscreen is one revision behind the schematic and two behind the truth."},
             14, 3, 24, 3, 1, 4},
            {"SCOPE THE PIN",
             "The scope reports facts and refuses to speculate.",
             {"Clean edges, correct level, wrong pin. The measurement is perfect and unhelpful.",
              "Pancetta scopes it. The trace is honest. The map is not. The map is the suspect."},
             11, 2, 16, 2, 1, CASE_CLOSED},
            {"ACCEPT THE PLEA",
             "The accepted plea closes the file on the wrong defendant.",
             {"G15 takes the charge. G13 walks. The fault reappears next Tuesday under a new name.",
              "Case closed on a confession. Pancetta files it and does not feel good about it."},
             8, 0, 12, 1, -1, CASE_CLOSED},
        },
    },
    {   // 3 - clean glasses, more bugs
        {"The lenses clear and three new bugs come into focus. They have formed a committee and elected a chair.",
         "Vision restored. The solder bridge was always there, enjoying the privacy, and now resents the attention."},
        {
            {"CHAIR THE COMMITTEE",
             "Taking the chair turns three loose bugs into one ranked list.",
             {"Pancetta ranks them by blast radius. The bridge is third and the most likely to reoffend.",
              "Agenda set, minutes taken. Two bugs turn out to be the same bug wearing a hat."},
             13, 3, 22, 3, 1, 4},
            {"FIX THE BRIDGE",
             "The removed bridge fixes one fault and reveals the next.",
             {"Solder wick, four seconds, done. The board boots further and fails somewhere more interesting.",
              "The bridge goes. The committee continues without it and does not send flowers."},
             11, 2, 18, 2, 1, CASE_CLOSED},
            {"DIM THE LIGHTS",
             "Dimming the lights is not a fix and everybody present knows it.",
             {"The bugs are still there. Pancetta simply cannot see them, which the schedule counts as progress.",
              "Lights down. Dr. Oculus writes 'symptomatic relief' and charges for the lens anyway."},
             6, -2, 8, 0, -1, CASE_CLOSED},
        },
    },
    {   // 4 - close
        {"Dr. Oculus removes every lens at once. Underneath, his eyes are fine. They have always been fine.",
         "The last lens comes off. The optometry was never the point; the pin map was, and he wrote it."},
        {
            {"CHARGE THE DOCTOR",
             "Charging the doctor empties the waiting room.",
             {"He drew the rev C silkscreen. Every misdiagnosis since has been him reading his own handwriting.",
              "Pancetta names him. The lenses were a filing system for one very old mistake."},
             19, 4, 32, 3, 1, CASE_CLOSED},
            {"TAKE THE MAP",
             "The corrected map is worth more than the confession.",
             {"He hands over the real pin map, annotated, dated, and correct. It costs him nothing and everything.",
              "Pancetta takes the map instead of the scalp. Future boards will boot; this one still won't."},
             16, 5, 26, 3, 1, CASE_CLOSED},
            {"LET HIM PRESCRIBE",
             "Letting him keep practising keeps the invoices coming.",
             {"He puts the lenses back on. The waiting room refills. Nothing is corrected and everything is billed.",
              "Pancetta walks out. Somewhere a G13 is being blamed for a G15 problem, again."},
             11, 1, 18, 1, -1, CASE_CLOSED},
        },
    },
};

static const CaseNode RASTA_HOOLIGAN_NODES[] = {
    {   // 1 - followed D0H4M
        {"Passive mode found the bug without creating three more. 01NK5 is visibly unwell and reaching for the keyboard.",
         "The trace spoke first. D0H4M waits one more packet. 01NK5 has already opened a terminal out of spite."},
        {
            {"WAIT ONE MORE",
             "One more packet of patience gives D0H4M the whole confession.",
             {"The culprit beacons. Full frame, full header, full name. Patience billed at zero and paid in evidence.",
              "One more packet and the AP explains itself unprompted. 01NK5 refuses to make eye contact."},
             14, 3, 22, 3, 1, 4},
            {"LET 01NK5 GO",
             "Letting 01NK5 loose ends the quiet part.",
             {"Enter is pressed. The radio brrrs. The evidence survives, barely, and so does the demo.",
              "01NK5 gets the keyboard back. Six seconds later there is a new TODO with legal standing."},
             10, 1, 20, 2, -1, CASE_CLOSED},
            {"CLOSE THE TRACE",
             "The closed trace is clean, complete, and slightly early.",
             {"Pancetta stops capture. D0H4M nods. The file is admissible and one packet short of certain.",
              "Trace closed. Everything in it is true. Not everything true is in it."},
             11, 2, 16, 2, 0, CASE_CLOSED},
        },
    },
    {   // 2 - followed 01NK5
        {"Root happened quickly. Understanding is arriving later by public transport. D0H4M has not said a word.",
         "The experiment shipped and came back with two dependents. 01NK5 calls this velocity."},
        {
            {"READ THE DAMAGE",
             "Reading the damage turns a stunt into an actual finding.",
             {"Three services down, one shell up, and a log that explains exactly how. The stunt was reproducible.",
              "Pancetta reads it all. Under the noise there is a real vulnerability with a real CVE shape."},
             13, 2, 24, 3, 1, 4},
            {"SHIP IT AGAIN",
             "Shipping it twice makes it a pattern instead of an accident.",
             {"Second run, same result, more dependents. 01NK5 calls this science. D0H4M leaves the room.",
              "It reproduces. That is the good news and it is also the entire problem."},
             9, -1, 26, 1, -1, CASE_CLOSED},
            {"ROLL IT BACK",
             "The rollback restores service and deletes the only evidence.",
             {"Everything comes back up. The shell goes away. So does the proof it was ever there.",
              "Pancetta rolls back. Uptime recovers. The finding does not survive the restore."},
             8, 1, 12, 1, 0, CASE_CLOSED},
        },
    },
    {   // 3 - keyboard unplugged
        {"Threat surface dropped by one pig and eighty-seven percent. 01NK5 has discovered read-only access and is taking it badly.",
         "The cable is on the table as evidence and as a grievance. D0H4M calls it the cleanest patch all week."},
        {
            {"GIVE IT BACK",
             "Returning the keyboard with conditions is the only patch that holds.",
             {"Cable back, rules agreed, one hand on the capture. 01NK5 accepts terms for the first time on record.",
              "Pancetta returns it with a condition. 01NK5 reads the condition. 01NK5 signs the condition."},
             15, 4, 24, 3, 1, 4},
            {"KEEP IT BAGGED",
             "The bagged keyboard keeps the peace and loses the collaborator.",
             {"01NK5 leaves. The room is quiet, correct, and noticeably worse at finding things.",
              "Cable stays in the bag. So does half the team's velocity. Both were load-bearing."},
             10, 0, 14, 2, -1, CASE_CLOSED},
            {"TIE THE CABLE",
             "The cable tie is a policy pretending to be hardware.",
             {"One zip tie, one rule, zero enforcement. 01NK5 owns scissors and a strong sense of purpose.",
              "Pancetta ties it off. The control lasts until the first genuinely interesting packet."},
             9, 1, 16, 1, 0, CASE_CLOSED},
        },
    },
    {   // 4 - close
        {"D0H4M and 01NK5 have the same finding from opposite directions. Neither will say the other was necessary.",
         "Patience found the frame. Impatience found the bug. The report needs both names and neither pig wants that."},
        {
            {"FILE BOTH NAMES",
             "Both names on the file makes the finding hold.",
             {"One report, two authors, zero eye contact. It is the strongest thing either of them has signed.",
              "Pancetta credits both. The evidence is patient and the exploit is fast and the file needs the pair."},
             20, 5, 30, 3, 1, CASE_CLOSED},
            {"CREDIT D0H4M",
             "Crediting patience alone is half the truth.",
             {"The trace carries the file. 01NK5 says nothing, which from 01NK5 is a whole paragraph.",
              "D0H4M's name goes on it. The report is clean and slightly less true than it could be."},
             14, 2, 22, 2, 0, CASE_CLOSED},
            {"CREDIT 01NK5",
             "Crediting the exploit alone is fast, loud, and hard to defend.",
             {"01NK5 gets the byline. The finding lands, then wobbles under the first serious question.",
              "Pancetta credits the shell, not the trace. It ships. It does not survive review intact."},
             12, 1, 26, 1, -1, CASE_CLOSED},
        },
    },
};

static const CaseNode BARMAN_NODES[] = {
    {   // 1 - receipts audited
        {"The receipts hash to a menu screenshot. Fraud, but beautifully kerned. The Barman does not blink.",
         "Provenance failed at byte zero. Behind the red eyes, somebody is deciding how much to admit."},
        {
            {"FOLLOW THE HASH",
             "The followed hash finds who actually printed the ledger.",
             {"The screenshot came from a terminal in the back room. The back room is in the barn sketch.",
              "One hash, one timestamp, one machine. The forgery has better provenance than the cheese."},
             15, 3, 26, 3, 1, 4},
            {"NAME THE FRAUD",
             "Naming it out loud costs the informant and buys the truth.",
             {"Pancetta says fraud. The Barman agrees, cheerfully, and stops being useful forever.",
              "The word lands. The briefcase closes. So does the only door into the bar's ledger."},
             11, 1, 18, 2, 0, CASE_CLOSED},
            {"KEEP READING",
             "More reading finds the one honest line in a dishonest book.",
             {"Page forty is real. It is the only real page and it names a supplier nobody has met.",
              "Pancetta keeps reading. Buried in the forgery is one entry somebody forgot to fake."},
             13, 2, 20, 3, 1, CASE_CLOSED},
        },
    },
    {   // 2 - bought the dip
        {"The dip crashed before vesting. Finance calls this texture. The Barman calls it a lesson and bills for it.",
         "Instant rank tasted synthetic. The spoon left a bad mood and no usable receipt."},
        {
            {"DEMAND THE SPOON",
             "The spoon is the only physical evidence the transaction happened.",
             {"Pancetta takes the spoon. Residue, serial number, and one partial print that is not the Barman's.",
              "The spoon goes in a bag. It is undignified, admissible, and the best lead in the room."},
             12, 2, 22, 3, 1, 4},
            {"BUY MORE",
             "Doubling down keeps Pancetta on the customer side.",
             {"Second helping, second crash. The Barman is now the only party with a positive position.",
              "Pancetta buys again. The yield is lactose and one deprecated callback. Rank stays fake."},
             4, -3, 30, 0, -1, CASE_CLOSED},
            {"WALK IT OFF",
             "Walking away costs the lead and saves the mood.",
             {"Pancetta leaves the spoon and the rank. Outside, the rain is at least honest about being water.",
              "The bar door closes. Nothing was learned and nothing else was lost. Call it even."},
             7, 1, 8, 1, 0, CASE_CLOSED},
        },
    },
    {   // 3 - evidence seized
        {"The briefcase is open. Inside: one exploit, two alibis, and honest provenance on the wrong item.",
         "Real XP only. The counterfeit wheel is sweating under cross-examination and the Barman is watching Pancetta, not the cheese."},
        {
            {"OPEN THE EXPLOIT",
             "Reading the exploit tells Pancetta what the cheese was ever for.",
             {"It is a downgrade against the XP ledger, written well, dated early, and never used. He was holding it.",
              "The exploit is real, clean, and unfired. The Barman has been sitting on it like a threat, not a tool."},
             17, 3, 28, 3, 1, 4},
            {"TAG THE WHEEL",
             "The tagged wheel closes the fraud and keeps the informant.",
             {"Evidence label, chain of custody, done. The Barman respects the procedure and hates the sticker.",
              "Pancetta tags it. The counterfeit is off the street and the red eyes are still at the bar."},
             15, 4, 26, 3, 1, CASE_CLOSED},
            {"BURN THE CASE",
             "Burning it protects nobody and satisfies exactly one person.",
             {"The briefcase goes. So does the exploit, the alibis, and any chance of naming the printer.",
              "Pancetta destroys it. The bar gets quieter. The ledger stays exactly as fake as it was."},
             8, -1, 14, 0, -1, CASE_CLOSED},
        },
    },
    {   // 4 - close
        {"The Barman takes off the glasses. The red eyes are a filter, not a condition. Under them he looks tired and extremely sober.",
         "He puts the glasses on the bar. Without them he is just a man who has been holding an exploit for six years."},
        {
            {"TAKE THE EXPLOIT",
             "Taking it disarms the bar and puts the weight on Pancetta.",
             {"He hands it over without a price. That is how Pancetta knows exactly how heavy it is.",
              "The exploit changes hands. The Barman looks lighter. Nobody in this room is happier."},
             20, 4, 32, 3, 1, CASE_CLOSED},
            {"LEAVE IT WITH HIM",
             "Trust is the one currency the bar never faked.",
             {"Pancetta pushes it back. The Barman nods once. K-H0RS3 translates the nod as a debt.",
              "It stays behind the bar. So does the reason. Both are safer there than in a coat pocket."},
             17, 5, 26, 3, 1, CASE_CLOSED},
            {"REPORT THE BAR",
             "Reporting the bar loses the beat's best informant.",
             {"The report is accurate. Within a week the bar is dark, the ledger is gone, and so is every lead.",
              "Pancetta files it. Correct, complete, and the last useful thing this room will ever provide."},
             14, 1, 24, 2, -1, CASE_CLOSED},
        },
    },
};

static const Encounter ENCOUNTERS[] = {
    {
        Character::K_HORSE, "K-H0RS3", "EQUINE / HOSE / UNRESOLVED", 0x18, 1, 1,
        {nullptr, nullptr, nullptr, nullptr},
        {
            "A horse enters through the uplink. Its rear half is a garden hose. Physics opens a ticket.",
            "K-H0RS3 coils by the antenna. It neighs at 115200 baud and leaks near the connector.",
            "The antenna coughs up K-H0RS3: front half witness, rear half plumbing. Both request counsel.",
            "K-H0RS3 arrives damp, encrypted, and legally adjacent to livestock. The hose refuses the lineup.",
        },
        {
            {"CHECK THE MANE",
             "Mane evidence stays bagged. K-H0RS3 trusts the gloves.",
             {"The mane is braided CAT6. It kicks when called legacy.",
                                 "You find a checksum in the forelock. It bites the reviewer.",
                                 "Every braid terminates in a different barn. The routing table calls it balance.",
                                 "The mane carries packets one way and gossip both ways. Pancetta bags a strand."},
             8, 2, 18, 2, 1, 1},
            {"PATCH THE LEAK",
             "The dry connector buys one future repair.",
             {"Pancetta calls it self iron-y: the iron burns you, then reviews your technique.",
                                 "The leak stops. The smoke continues. Different incident.",
                                 "Pressure holds until K-H0RS3 remembers water. The patch files for retirement.",
                                 "The connector is dry. The floor is not. Success chose a narrow definition."},
             10, 1, 24, 2, 0, 2},
            {"ASK ABOUT BARN",
             "The barn lead reaches the Barman without a receipt.",
             {"K-H0RS3 says BARN. The Barman translates it as a threat model.",
                                 "It points at the bar. Somewhere, red eyes approve behind glass.",
                                 "The witness draws a floor plan shaped like itself. Zoning returns the case.",
                                 "It counts four walls, then coils into the fifth. Geometry requests leave."},
             6, 3, 12, 1, -1, 3},
        },
        K_HORSE_NODES, (uint8_t)(sizeof(K_HORSE_NODES) / sizeof(K_HORSE_NODES[0])),
    },
    {
        Character::WISE_PIG, "TH3 W1S3 P1G", "CERTIFIED BY A SHRUBBERY", 0x14, 3, 0,
        {
            "CAPTION: A second pig waits off-panel. Nobody sees whether he brought exact change.",
            "CAPTION: Beyond the crop, an unseen pig hears robes rustle and a bridge invoice print.",
            "CAPTION: The other pig is outside the frame, auditing the riddle and the catering.",
            "CAPTION: Off-panel, a patient pig holds the answer and refuses to spoil the bit.",
        },
        {
            "WISE PIG: Thirteen doors, no hinges. Invisible traffic passes. Name the hallway.",
            "WISE PIG: I cross walls unseen, wake radios, and keep thirteen channels. What am I?",
            "WISE PIG: Thirteen numbered rooms overlap. Everyone claims channel six. Name the block.",
            "WISE PIG: No feet, yet I roam. No mouth, yet I carry confessions near 2.4. Name me.",
        },
        {
            {"THE 2.4 GHZ BAND",
             "Exact answer earns a bridge pass and another riddle.",
             {"The unseen pig nods. Correct. Thirteen channels open and the bridge stops billing.",
                                   "Correct. The Wise Pig grants passage. The narrator keeps the receipt.",
                                   "The answer lands clean. Thirteen doors unlock; eleven still overlap badly.",
                                   "Correct. The off-panel pig stamps the file and quietly pockets the bridge."},
             16, 4, 25, 3, 1, 1},
            {"THE INTERNET",
             "Partial credit leaves the toll open.",
             {"Broadly adjacent. The Wise Pig awards half a nod and keeps the toll.",
                               "A network, yes. The answer limps home with partial credit.",
                               "Too large. The Wise Pig asks for a band; Pancetta brought the whole orchestra.",
                               "The answer contains the truth like a city contains one alley. Partial credit."},
             8, 1, 10, 1, 0, 2},
            {"A WET ACCESS POINT",
             "The wet answer follows Pancetta into the next file.",
             {"The unseen pig coughs. The Wise Pig marks the answer damp, not correct.",
                                     "No. Somewhere off-panel, a router is wrapped in a towel.",
                                     "Moisture explains K-H0RS3, not radio. The answer is filed under plumbing.",
                                     "The unseen pig slides over a bucket. The Wise Pig slides back the question."},
             3, -2, 2, 0, -1, 3},
        },
        WISE_PIG_NODES, (uint8_t)(sizeof(WISE_PIG_NODES) / sizeof(WISE_PIG_NODES[0])),
    },
    {
        Character::COW, "C0W-0WNER", "BOVINE AS A SERVICE", 0x0A, 1, 0,
        {nullptr, nullptr, nullptr, nullptr},
        {
            "A cow drops from above. The sky denies ownership. The cow requests contributor access.",
            "A suspicious cow is standing on the rooftop. Traceroute says France. GPS says moo.",
            "A cow arrives by unsupported air path. Its parachute is a license nobody has read.",
            "The roof reports one bovine process with no parent. Pancetta starts an incident log.",
        },
        {
            {"CHECK SIGNATURE",
             "Verified hoofprint enters chain of custody.",
             {"The cow is unsigned but grass-backed. Supply chain risk remains delicious.",
                                  "Its certificate says MOO CA. Pancetta trusts it exactly once.",
                                  "The hoofprint verifies. The cow does not. Identity remains pasture-based.",
                                  "Signature valid, issuer unknown, subject hungry. Pancetta notes all three."},
             8, 2, 15, 2, 1, 1},
            {"DENY SUDO",
             "Least privilege makes the cow a cooperative witness.",
             {"The cow accepts least privilege and eats the privilege escalation notes.",
                            "Access denied. Milk production continues under a service account.",
                            "Root is refused. The cow requests wheel, then remembers it has four legs.",
                            "Least privilege holds. The cow can read grass and write only to the incident log."},
             10, 1, 20, 2, 1, 2},
            {"CATAPULT BACK",
             "The sky reopens the case with Pancetta named.",
             {"Trajectory nominal. Dignity segmentation faulted at launch.",
                                "The cow clears the parapet. A distant castle logs packet loss.",
                                "Launch succeeds. The return address points upward and declines comment.",
                                "Airborne again. The sky reopens the ownership ticket and assigns Pancetta."},
             6, 5, 5, 1, -1, 3},
        },
        COW_NODES, (uint8_t)(sizeof(COW_NODES) / sizeof(COW_NODES[0])),
    },
    {
        Character::DR_OCULUS, "DR. 0CULUS", "OPTOMETRY / GPIO FORENSICS", 0x11, 4, 0,
        {nullptr, nullptr, nullptr, nullptr},
        {
            "Dr. Oculus adjusts decorative glasses and diagnoses a pin map with emotional damage.",
            "A doctor with too many lenses asks which GPIO betrayed you. Pancetta says all of them.",
            "Dr. Oculus reads the schematic upside down. It improves the prognosis but not the board.",
            "Six lenses inspect one cold solder joint. The joint asks for patient confidentiality.",
        },
        {
            {"SECOND OPINION",
             "A second opinion keeps Oculus professionally curious.",
             {"The second opinion uses the same datasheet but a less judgmental font.",
                                 "Diagnosis: acute pin drift with chronic README avoidance.",
                                 "The backup diagnosis says silicon fatigue. The silicon says developer fatigue.",
                                 "Another lens confirms it: the symptom is hardware; the cause has a keyboard."},
             11, 1, 16, 2, 0, 1},
            {"POINT AT G15",
             "G15's confession earns trust in Pancetta's pin work.",
             {"Correct pin. Wrong board revision. The patient is technically alive.",
                               "G15 confesses immediately. G13 asks for counsel.",
                               "The pin twitches under questioning. Dr. Oculus prescribes a pull-up and silence.",
                               "G15 was framed by the silkscreen. The schematic enters witness protection."},
             13, 2, 22, 3, 1, 2},
            {"CLEAN GLASSES",
             "Clean lenses reveal bugs Oculus will invoice later.",
             {"The bug remains, but now it is high definition.",
                                "He can finally see the race condition. It waves.",
                                "The lenses clear. Three new bugs come into focus and form a committee.",
                                "Vision restored. The solder bridge was always there, enjoying the privacy."},
             7, 4, 8, 1, -1, 3},
        },
        DR_OCULUS_NODES, (uint8_t)(sizeof(DR_OCULUS_NODES) / sizeof(DR_OCULUS_NODES[0])),
    },
    {
        Character::RASTA_HOOLIGAN, "D0H4M + 01NK5", "RASTA / BRITISH HOOL", 0x09, 2, 0,
        {nullptr, nullptr, nullptr, nullptr},
        {
            "Doham smiles through the smoke. Oinks has already pressed Enter. The demo acquires weather.",
            "Rasta watches the wire. British Hool attacks the keyboard. One of them brought backups.",
            "Doham reads the trace. Oinks reads the room and presses Enter anyway.",
            "Two pigs enter. One has patience. The other has root and a very short cable.",
        },
        {
            {"FOLLOW D0H4M",
             "Doham trusts Pancetta with the next trace.",
             {"Nothing happens. This is evidence. Oinks looks physically unwell.",
                               "Passive mode finds the bug without creating three more. Suspiciously mature.",
                               "Doham waits one packet longer. The culprit gets nervous and beacons.",
                               "The trace speaks first. Oinks calls this slow; Pancetta calls it admissible."},
             12, 3, 14, 3, 1, 1},
            {"FOLLOW 01NK5",
             "Oinks logs Pancetta as fast enough to share blame.",
             {"The terminal screams, the radio brrrs, and a TODO gains legal personhood.",
                               "Enter was pressed with confidence. Recovery was pressed with experience.",
                               "Oinks ships the experiment. The experiment returns with two dependents.",
                               "Root happens quickly. Understanding arrives later by public transport."},
             9, -1, 28, 1, -1, 2},
            {"UNPLUG KEYBOARD",
             "The unplugged keyboard becomes evidence and a grievance.",
             {"Consensus achieved by removing the attack surface with a cable tie.",
                                  "Oinks discovers read-only access. Character development hurts.",
                                  "The cable leaves. Threat surface drops by one pig and eighty-seven percent.",
                                  "Silence reaches the terminal. Doham names it the cleanest patch all week."},
             10, 2, 18, 2, 0, 3},
        },
        RASTA_HOOLIGAN_NODES,
        (uint8_t)(sizeof(RASTA_HOOLIGAN_NODES) / sizeof(RASTA_HOOLIGAN_NODES[0])),
    },
    {
        Character::BARMAN, "TH3 B4RM4N", "FORBIDDEN CHEESE / RED EYES", 0x14, 5, 0,
        {nullptr, nullptr, nullptr, nullptr},
        {
            "The Barman opens his jacket. Forbidden XP cheese glitters behind round glasses and red eyes.",
            "The Barman offers instant rank. The blockchain is a napkin with GREP written on it.",
            "Red eyes, round glasses, sealed briefcase. The Barman calls the contents pre-owned XP.",
            "The Barman rolls in a wheel marked LEGIT. Every letter uses a different checksum.",
        },
        {
            {"ASK FOR RECEIPTS",
             "The receipt audit makes the Barman hide his next ledger.",
             {"The receipts hash to a menu screenshot. Fraud, but beautifully kerned.",
                                  "Provenance fails at byte zero. The cheese blames cosmic rays.",
                                  "The ledger starts tomorrow and ends yesterday. Pancetta circles both dates.",
                                  "Each receipt validates the next. The last validates a damp bar napkin."},
             14, 2, 25, 2, 1, 1},
            {"BUY THE DIP",
             "Buying the dip marks Pancetta as customer, not detective.",
             {"Pancetta buys one dip. It crashes before vesting. Finance calls this texture.",
                              "The yield is mostly lactose and one deprecated callback.",
                              "The dip rises, forks, and vanishes into fees. The cracker keeps the blame.",
                              "Instant rank tastes synthetic. The spoon leaves a bad mood and no usable receipt."},
             5, -2, 35, 0, -1, 2},
            {"SEIZE EVIDENCE",
             "Seized cheese makes the Barman a careful informant.",
             {"Real XP only. The counterfeit wheel sweats under cross-examination.",
                                 "Case closed. The evidence pairs well with responsible disclosure.",
                                 "The briefcase opens. Inside: one exploit, two alibis, and honest provenance.",
                                 "Pancetta tags the wheel. The Barman respects procedure and hates the label."},
             16, 4, 30, 3, 1, 3},
        },
        BARMAN_NODES, (uint8_t)(sizeof(BARMAN_NODES) / sizeof(BARMAN_NODES[0])),
    },
};

static_assert(sizeof(ENCOUNTERS) / sizeof(ENCOUNTERS[0]) <= 32,
              "seen/case masks support at most 32 encounter files");

size_t count() {
    return sizeof(ENCOUNTERS) / sizeof(ENCOUNTERS[0]);
}

const Encounter& get(size_t index) {
    return ENCOUNTERS[index % count()];
}

const char* replyText(const Choice& choice, uint8_t variant) {
    if (variant < DIALOGUE_VARIANTS && choice.reply[variant]) {
        return choice.reply[variant];
    }
    return choice.reply[0];
}

const char* promptText(const CaseNode& node, uint8_t variant) {
    if (variant < FOLLOWUP_VARIANTS && node.prompt[variant]) {
        return node.prompt[variant];
    }
    return node.prompt[0];
}

const CaseNode* followUp(const Encounter& encounter, const Choice& choice) {
    if (choice.nextNode == CASE_CLOSED) return nullptr;
    if (!encounter.nodes || choice.nextNode > encounter.nodeCount) return nullptr;
    return &encounter.nodes[choice.nextNode - 1];
}

// Depth of the chain hanging off one beat's choice table, counting that beat.
// `budget` is the remaining allowance, so a content typo that points a node back
// at an ancestor terminates instead of recursing forever. The root table and a
// node's table are walked identically — only the budget differs.
static uint8_t depthFrom(const Encounter& encounter, const Choice (&choices)[3],
                         uint8_t budget) {
    if (budget == 0) return 0;
    uint8_t deepest = 0;
    for (uint8_t i = 0; i < 3; ++i) {
        const CaseNode* next = followUp(encounter, choices[i]);
        if (!next) continue;
        uint8_t depth = depthFrom(encounter, next->choices,
                                  (uint8_t)(budget - 1));
        if (depth > deepest) deepest = depth;
    }
    return (uint8_t)(deepest + 1);
}

uint8_t caseDepth(const Encounter& encounter) {
    // +1 because the budget covers the root beat too; the follow-ups hanging
    // off it are the ones MAX_CASE_DEPTH actually limits.
    return depthFrom(encounter, encounter.choices, MAX_CASE_DEPTH + 1);
}

uint8_t beatForNode(const Encounter& encounter, uint8_t node) {
    if (node == 0) return 1;
    if (!encounter.nodes || node > encounter.nodeCount) return 0;

    // Search by beat rather than treating the storage index as a beat number:
    // root choices may fan straight into nodes 1, 2, or 3. A saved node is a
    // location in that graph, not necessarily the fourth story beat.
    uint32_t visited = 0;
    uint32_t frontier = 0;
    for (uint8_t choice = 0; choice < 3; ++choice) {
        uint8_t next = encounter.choices[choice].nextNode;
        if (next > 0 && next <= encounter.nodeCount) {
            frontier |= 1u << (next - 1);
        }
    }

    for (uint8_t beat = 2; frontier != 0 && beat <= MAX_CASE_DEPTH; ++beat) {
        if (frontier & (1u << (node - 1))) return beat;
        visited |= frontier;

        uint32_t nextFrontier = 0;
        for (uint8_t index = 0; index < encounter.nodeCount; ++index) {
            if ((frontier & (1u << index)) == 0) continue;
            const CaseNode& current = encounter.nodes[index];
            for (uint8_t choice = 0; choice < 3; ++choice) {
                uint8_t next = current.choices[choice].nextNode;
                if (next > 0 && next <= encounter.nodeCount) {
                    nextFrontier |= 1u << (next - 1);
                }
            }
        }
        frontier = nextFrontier & ~visited;
    }
    return 0;
}

// Marks every node reachable from a beat's choice table. Same walk as
// depthFrom, collecting a reachability set instead of a depth.
static void walkReachable(const Encounter& encounter,
                          const Choice (&choices)[3], uint32_t& reached,
                          uint8_t budget) {
    if (budget == 0) return;
    for (uint8_t i = 0; i < 3; ++i) {
        const Choice& choice = choices[i];
        const CaseNode* next = followUp(encounter, choice);
        if (!next) continue;
        uint32_t bit = 1u << (choice.nextNode - 1);
        if (reached & bit) continue;  // already walked; cycles stop here
        reached |= bit;
        walkReachable(encounter, next->choices, reached, (uint8_t)(budget - 1));
    }
}

bool caseTreeIsSound(const Encounter& encounter) {
    if (!encounter.nodes) return encounter.nodeCount == 0;
    if (encounter.nodeCount == 0 || encounter.nodeCount > 32) return false;

    // Every choice anywhere in the file must either close the case or name a
    // node that exists. A typo'd index is a dead end the operator cannot leave.
    for (uint8_t i = 0; i < 3; ++i) {
        if (encounter.choices[i].nextNode > encounter.nodeCount) return false;
    }
    for (uint8_t n = 0; n < encounter.nodeCount; ++n) {
        for (uint8_t i = 0; i < 3; ++i) {
            if (encounter.nodes[n].choices[i].nextNode > encounter.nodeCount) {
                return false;
            }
        }
    }

    uint32_t reached = 0;
    walkReachable(encounter, encounter.choices, reached, MAX_CASE_DEPTH + 1);

    // Every node the table declares has to be walkable, and no branch may
    // outrun the depth budget the walker is willing to follow.
    uint32_t declared = (encounter.nodeCount >= 32)
                            ? 0xFFFFFFFFu
                            : ((1u << encounter.nodeCount) - 1u);
    if (reached != declared) return false;
    return caseDepth(encounter) <= MAX_CASE_DEPTH;
}

static bool eligible(const Encounter& e, uint8_t room, uint8_t level, uint8_t kHorseLevel) {
    if (room >= 5 || (e.roomMask & (1u << room)) == 0) return false;
    return level >= e.minLevel && kHorseLevel >= e.minKHorse;
}

int pick(uint32_t roll, uint8_t room, uint8_t level, uint8_t kHorseLevel,
         int lastIndex, uint32_t seenMask) {
    uint16_t weightTotal = 0;
    uint8_t eligibleCount = 0;
    for (size_t i = 0; i < count(); ++i) {
        if (!eligible(ENCOUNTERS[i], room, level, kHorseLevel)) continue;
        ++eligibleCount;
    }
    if (eligibleCount == 0) return -1;

    bool hasLastCharacter = lastIndex >= 0 && (size_t)lastIndex < count();
    Character lastCharacter = hasLastCharacter
                                  ? ENCOUNTERS[lastIndex].character
                                  : Character::COUNT;
    bool hasCharacterAlternative = false;
    if (hasLastCharacter) {
        for (size_t i = 0; i < count(); ++i) {
            if (eligible(ENCOUNTERS[i], room, level, kHorseLevel) &&
                ENCOUNTERS[i].character != lastCharacter) {
                hasCharacterAlternative = true;
                break;
            }
        }
    }

    for (size_t i = 0; i < count(); ++i) {
        if (!eligible(ENCOUNTERS[i], room, level, kHorseLevel)) continue;
        if (hasCharacterAlternative && ENCOUNTERS[i].character == lastCharacter) continue;
        weightTotal += (seenMask & (1u << i)) ? 1u : 3u;
    }
    if (weightTotal == 0) return -1;

    uint16_t target = (uint16_t)(roll % weightTotal);
    for (size_t i = 0; i < count(); ++i) {
        if (!eligible(ENCOUNTERS[i], room, level, kHorseLevel)) continue;
        if (hasCharacterAlternative && ENCOUNTERS[i].character == lastCharacter) continue;
        uint8_t weight = (seenMask & (1u << i)) ? 1u : 3u;
        if (target < weight) return (int)i;
        target -= weight;
    }
    return -1;
}

uint8_t caseArrivalChance(bool hasOpenedCase, uint8_t misses) {
    if (!hasOpenedCase) return 100;
    uint16_t chance = (uint16_t)(38u + (uint16_t)misses * 24u);
    return (uint8_t)(chance > 92u ? 92u : chance);
}

bool deadlineReached(uint32_t now, uint32_t deadline) {
    return (int32_t)(now - deadline) >= 0;
}

uint8_t pickVariant(uint32_t roll, uint8_t previous) {
    if (previous >= DIALOGUE_VARIANTS) {
        return (uint8_t)(roll % DIALOGUE_VARIANTS);
    }
    // Pick among N-1 slots, then step over the previous index.
    uint8_t picked = (uint8_t)(roll % (DIALOGUE_VARIANTS - 1));
    if (picked >= previous) ++picked;
    return picked;
}

uint32_t rememberChoice(uint32_t ledger, Character character, uint8_t choice) {
    uint8_t characterIndex = static_cast<uint8_t>(character);
    if (characterIndex >= CHARACTER_COUNT || choice >= 3) return ledger;
    uint8_t shift = (uint8_t)(characterIndex * 2u);
    ledger &= ~(3u << shift);
    ledger |= (uint32_t)(choice + 1u) << shift;
    return ledger;
}

uint8_t recallChoice(uint32_t ledger, Character character) {
    uint8_t characterIndex = static_cast<uint8_t>(character);
    if (characterIndex >= CHARACTER_COUNT) return NO_CHOICE;
    uint8_t encoded = (uint8_t)((ledger >> (characterIndex * 2u)) & 3u);
    return encoded == 0 ? NO_CHOICE : (uint8_t)(encoded - 1u);
}

int8_t scoreChoiceLedger(uint32_t ledger, uint32_t closedMask) {
    int8_t score = 0;
    for (size_t i = 0; i < count(); ++i) {
        const Encounter& encounter = ENCOUNTERS[i];
        if ((closedMask & characterBit(encounter.character)) == 0) continue;
        uint8_t choice = recallChoice(ledger, encounter.character);
        if (choice < 3) score = (int8_t)(score + encounter.choices[choice].threadImpact);
    }
    return score;
}

CaseEnding endingFromScore(int8_t score) {
    if (score >= 3) return CaseEnding::CLEAN_WIRE;
    if (score <= -2) return CaseEnding::LIVE_WIRE;
    return CaseEnding::OPEN_CIRCUIT;
}

const char* endingTitle(CaseEnding ending) {
    switch (ending) {
        case CaseEnding::CLEAN_WIRE: return "CLEAN WIRE";
        case CaseEnding::LIVE_WIRE: return "LIVE WIRE";
        case CaseEnding::OPEN_CIRCUIT: return "OPEN CIRCUIT";
    }
    return "OPEN CIRCUIT";
}

const char* endingText(CaseEnding ending) {
    switch (ending) {
        case CaseEnding::CLEAN_WIRE:
            return "Six files close with receipts intact. The cast starts sending Pancetta the hard cases first. Trust is now an attack surface.";
        case CaseEnding::LIVE_WIRE:
            return "Six files close with three new incidents. The cast calls Pancetta effective, volatile, and expensive. The wire remembers.";
        case CaseEnding::OPEN_CIRCUIT:
            return "Six files close. Half the cast trusts the badge; all six keep the number. The next call will cost a favor.";
    }
    return "";
}

}  // namespace NpcEventsCore
