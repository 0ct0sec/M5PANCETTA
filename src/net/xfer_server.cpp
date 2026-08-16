/**
 * XferServer — AP file manager implementation
 *
 * ==[ FILE XFER ]== no wifi network dependency. pig makes its own AP.
 * midnight commander UI. neon pink on black. full file ops.
 *
 * routes: / /ui.css /ui.js /api/ls /download /upload /delete
 *         /api/bulkdelete /mkdir /api/rename /api/copy /api/move
 *         /api/creds /api/swine /api/sdinfo
 *         /wpasec/data /wigle/data /info
 * legacy: /list /dl /ul /rm
 */

#include "xfer_server.h"
#include "wigle_client.h"
#include "wpasec_client.h"
#include "../core/config.h"
#include "../core/capture.h"
#include "../core/power.h"
#include "../hal/sd_storage.h"
#include "../build_info.h"
#include "../hamlet.h"
#include "../util/debug_log.h"
#include <WiFi.h>
#include <DNSServer.h>
#include <WebServer.h>
#include <SD.h>
#include <esp_heap_caps.h>
#include <esp_random.h>
#include "../sync/nowflock_transport.h"

namespace Xfer {

// ==[ AP CONFIG ]==
static const char* AP_SSID = "PANCETTA_XFER";
static char apPassword[9];
static const IPAddress AP_IP(192, 168, 4, 1);
static const IPAddress AP_GW(192, 168, 4, 1);
static const IPAddress AP_SN(255, 255, 255, 0);

static void generatePassword() {
    static const char charset[] = "ABCDEFGHJKLMNPQRSTUVWXYZabcdefghjkmnpqrstuvwxyz23456789";
    for (int i = 0; i < 8; i++)
        apPassword[i] = charset[esp_random() % (sizeof(charset) - 1)];
    apPassword[8] = '\0';
}

static WebServer server(80);
static DNSServer dns;
static bool running = false;
static bool routesConfigured = false;

// ==[ SESSION COUNTERS ]==
static uint32_t txBytes = 0;
static uint32_t rxBytes = 0;
static uint32_t uploadCount = 0;
static uint32_t downloadCount = 0;

// ==[ XP AWARD STATE ]==
static char (*xpWpaList)[40] = nullptr;
static uint16_t xpWpaCount = 0;
static bool xpWpaLoaded = false;
static char (*xpWigleList)[40] = nullptr;
static uint16_t xpWigleCount = 0;
static bool xpWigleLoaded = false;
static uint16_t xpSessionAwarded = 0;
static const uint16_t XP_SESSION_CAP = 200;
static const uint16_t XP_MAX_ENTRIES = 512;


// ==[ PROGMEM — CSS ]== Midnight Commander layout, neon pink palette
static const char HTML_STYLE[] PROGMEM = R"rawliteral(
/* ======================================================================
   PANCETTA embedded file server UI
   Midnight Commander layout + Neon Pink palette
   Oldschool terminal aesthetic - spartan and effective
   ====================================================================== */

:root{
  --dr-bg: #0a0a0a;
  --dr-fg: #E6EDF3;
  --dr-current: #1a1a1a;
  --dr-comment: #666666;
  --dr-cyan: #ff0080;
  --dr-green: #00ff66;
  --dr-orange: #F0883E;
  --dr-pink: #ff0080;
  --dr-purple: #ff40a0;
  --dr-red: #ff4444;
  --dr-yellow: #D29922;

  --bg: var(--dr-bg);
  --fg: var(--dr-fg);
  --dim: rgba(230,237,243,.60);
  --border: rgba(255,0,128,.20);
  --border-soft: rgba(255,0,128,.15);
  --col-sep: rgba(255,0,128,.20);
  --panel-bg: #0a0a0a;
  --panel-bg2: #111111;

  --title-inactive-bg: var(--dr-current);
  --title-inactive-fg: var(--fg);
  --title-active-bg: #ff0080;
  --title-active-fg: #000000;

  --focus-bg: #ff0080;
  --focus-fg: #000000;

  --mark-fg: var(--dr-green);
  --warn-fg: var(--dr-orange);
  --danger-fg: var(--dr-red);

  --key-bg: #ff0080;
  --key-fg: #000000;

  --shadow: rgba(0,0,0,.50);
  --frame-gap: 4px;
  --pad-x: 8px;
  --fs: 13px;
}

*{ box-sizing:border-box; margin:0; padding:0; }
html, body{ height:100%; }
body.mc{
  background: var(--bg);
  color: var(--fg);
  font-family: ui-monospace, SFMono-Regular, Menlo, Monaco, Consolas, "Liberation Mono", "Courier New", monospace;
  font-size: var(--fs);
  line-height: 1.25;
  display:flex;
  flex-direction:column;
  overflow:hidden;
  font-variant-ligatures: none;
}

/* ----------------------------------------------------------------------
   Top bars (MC-like)
   ---------------------------------------------------------------------- */
.header{
  padding: 5px var(--pad-x);
  display:flex;
  justify-content:space-between;
  align-items:center;
  flex-shrink:0;
  background: var(--title-active-bg);
  color: var(--title-active-fg);
  border-bottom: 1px solid var(--border);
}
.header h1{
  font-size: 1em;
  font-weight: normal;
  letter-spacing: .6px;
}
.sd-info{
  font-size: .9em;
  opacity: .90;
  white-space: nowrap;
}

.swine-strip{
  padding: 4px var(--pad-x);
  background: var(--panel-bg);
  border-bottom: 1px solid var(--border);
  font-size: .9em;
  flex-shrink:0;
}
.swine-line{
  white-space:nowrap;
  overflow:hidden;
  text-overflow:ellipsis;
}
.swine-line + .swine-line{ opacity:.88; }

/* ----------------------------------------------------------------------
   Main workspace frame
   ---------------------------------------------------------------------- */
.main{
  flex:1;
  display:grid;
  grid-template-rows: minmax(0, 1fr) minmax(0, .72fr);
  min-height:0;
  background: var(--bg);
}

/* Outer border like MC */
.panes{
  display:flex;
  min-height:0;
  overflow:hidden;

  margin: var(--frame-gap) var(--frame-gap) 0 var(--frame-gap);
  border: 1px solid var(--border);
  background: var(--panel-bg);
  box-shadow: inset 0 0 0 1px rgba(0,0,0,.18);
}

/* Two panes inside the workspace border */
.pane{
  flex:1;
  display:flex;
  flex-direction:column;
  min-height:0;
  overflow:hidden;
  background: var(--panel-bg);
}
.pane + .pane{
  border-left: 1px solid var(--border);
}

/* Pane title bar (MC-like with box-drawing decorations) */
.pane-header{
  flex-shrink:0;
  height: 1.4em;
  background: var(--panel-bg);
  display:flex;
  align-items:center;
  font-size: .9em;
  overflow:hidden;
}
.pane-decor-left{
  color: var(--border);
  white-space:nowrap;
  overflow:hidden;
  text-overflow:clip;
  flex: 1;
  text-align: right;
  padding-left: 2px;
}
.pane-decor-right{
  color: var(--border);
  white-space:nowrap;
  overflow:hidden;
  text-overflow:clip;
  flex: 1;
  text-align: left;
  padding-right: 2px;
}
.pane-path{
  flex-shrink: 0;
  white-space:nowrap;
  overflow:hidden;
  text-overflow:ellipsis;
  max-width: 60%;
  padding: 0 2px;
  color: var(--fg);
  background: var(--panel-bg);
}
.pane.active .pane-decor-left,
.pane.active .pane-decor-right{
  color: var(--dr-cyan);
}

/* Column headers row - MC style with vertical separators */
.col-header{
  display:grid;
  grid-template-columns: 4ch minmax(0, 1fr) 8ch 12ch;
  gap: 0;
  padding: 2px 4px;
  background: var(--panel-bg);
  font-size: 1em;
  color: var(--dim);
  text-transform: uppercase;
  letter-spacing: .3px;
  border-bottom: 1px solid var(--border);
}
.col-header > div{
  overflow:hidden;
  text-overflow:ellipsis;
  white-space:nowrap;
  padding: 0 4px;
  border-right: 1px solid var(--col-sep);
}
.col-header > div:last-child{
  border-right: none;
}
.col-header .col-size{
  text-align:right;
  padding-right: 4px;
}
.col-header .col-time{
  text-align:right;
  padding-right: 4px;
}

.file-list{
  flex:1;
  overflow-y:auto;
  overflow-x:hidden;
  background: var(--panel-bg);
}

/* Pane footer with path + disk usage (MC-style with box-drawing) */
.pane-footer{
  flex-shrink:0;
  height: 1.4em;
  background: var(--panel-bg);
  display:flex;
  align-items:center;
  font-size: .85em;
  overflow:hidden;
}
.pane-footer-decor-left{
  color: var(--border);
  white-space:nowrap;
  overflow:hidden;
  text-overflow:clip;
  flex: 1;
  text-align: right;
  padding-left: 2px;
}
.pane-footer-decor-right{
  color: var(--border);
  white-space:nowrap;
  overflow:hidden;
  text-overflow:clip;
  flex: 1;
  text-align: left;
  padding-right: 2px;
}
.pane-footer-disk{
  white-space:nowrap;
  flex-shrink:0;
}
.pane.active .pane-footer-decor-left,
.pane.active .pane-footer-decor-right{
  color: var(--dr-cyan);
}

/* Hide scrollbars (embedded vibe) */
.file-list, .queue-list{
  scrollbar-width: none;
  -ms-overflow-style: none;
}
.file-list::-webkit-scrollbar,
.queue-list::-webkit-scrollbar{ display:none; }

/* ----------------------------------------------------------------------
   File rows (MC-like density with 4 columns + vertical separators)
   ---------------------------------------------------------------------- */
.file-item{
  display:grid;
  grid-template-columns: 4ch minmax(0, 1fr) 8ch 12ch;
  align-items:center;
  gap: 0;
  padding: 2px 4px;
  cursor:pointer;
  background: transparent;
  min-width:0;
  user-select:none;
  touch-action:manipulation;
}

.file-prefix{
  font-weight: normal;
  font-family:inherit;
  font-size:inherit;
  white-space:nowrap;
  padding: 0 2px;
  text-align:center;
  color:inherit;
  background:transparent;
  appearance:none;
  border:0;
  border-right: 1px solid var(--col-sep);
  cursor:pointer;
  touch-action:manipulation;
}
.file-prefix.parent{ cursor:default; }
.file-prefix.dir{
  color: var(--dr-cyan);
}
.file-prefix.exec{
  color: var(--dr-green);
}
.file-prefix.file{
  color: var(--dim);
}
.file-prefix.marked{
  color: var(--mark-fg);
  font-weight:700;
}
.file-name{
  overflow:hidden;
  text-overflow:ellipsis;
  white-space:nowrap;
  min-width:0;
  padding: 0 4px;
  border-right: 1px solid var(--col-sep);
}
.file-name.dir{
  color: var(--dr-cyan);
}
.file-name.exec{
  color: var(--dr-green);
}
.file-size{
  color: var(--dim);
  text-align:right;
  font-size: .95em;
  font-variant-numeric: tabular-nums;
  white-space:nowrap;
  padding: 0 4px;
  border-right: 1px solid var(--col-sep);
}
.file-time{
  color: var(--dim);
  text-align:right;
  font-size: .9em;
  font-variant-numeric: tabular-nums;
  white-space:nowrap;
  padding: 0 4px;
}

/* Hover resembles MC "current line" but subtle */
.file-item:hover{
  background: rgba(68,71,90,.38);
}

/* Marked/selected items */
.file-item.selected{
  background: rgba(80,250,123,.12);
}
.file-item.selected .file-prefix,
.file-item.selected .file-name{
  color: var(--mark-fg);
}

/* Cursor line: inverse (Dracula MC uses a vivid highlight) */
.file-item.focused{
  background: var(--focus-bg);
  color: var(--focus-fg);
}
.file-item.focused .file-prefix,
.file-item.focused .file-name,
.file-item.focused .file-size,
.file-item.focused .file-time{
  color: var(--focus-fg);
}

/* Cursor + marked: marker still "marked" */
.file-item.selected.focused{
  background: var(--focus-bg);
}
.file-item.selected.focused .file-prefix{
  color: var(--mark-fg);
}

/* ----------------------------------------------------------------------
   Ops panels (bottom half) - styled like MC panels
   ---------------------------------------------------------------------- */
.ops{
  display:grid;
  grid-template-columns: 1fr 1fr;
  gap: 0;
  margin: var(--frame-gap);
  border: 1px solid var(--border);
  background: var(--panel-bg2);
  min-height:0;
  overflow:hidden;
}
.ops-panel{
  padding: 0;
  overflow:hidden;
  display:flex;
  flex-direction:column;
  min-height:0;
}
.ops-panel + .ops-panel{
  border-left: 1px solid var(--border);
}
.ops-block{
  display:flex;
  flex-direction:column;
  min-height:0;
  height:100%;
}
.ops-header{
  display:flex;
  align-items:center;
  height: 1.4em;
  background: var(--panel-bg);
  font-size: .9em;
  overflow:hidden;
}
.ops-decor-left{
  color: var(--border);
  white-space:nowrap;
  overflow:hidden;
  text-overflow:clip;
  flex: 1;
  text-align: right;
  padding-left: 2px;
}
.ops-decor-right{
  color: var(--border);
  white-space:nowrap;
  overflow:hidden;
  text-overflow:clip;
  flex: 1;
  text-align: left;
  padding-right: 2px;
}
.ops-title-wrap{
  flex-shrink: 0;
  display:flex;
  align-items:center;
  gap: 8px;
  background: var(--panel-bg);
  padding: 0 2px;
}
.ops-title{
  text-transform: uppercase;
  letter-spacing: .5px;
  color: var(--fg);
  white-space:nowrap;
}
.ops-meta{
  white-space:nowrap;
  overflow:hidden;
  text-overflow:ellipsis;
  color: var(--dim);
}
.ops-meta-link{ cursor:pointer; }
.ops-meta-link:hover{ color: var(--dr-cyan); text-decoration: underline; }
.ops-actions{
  display:flex;
  gap: 6px;
}

/* Queues are "tables" => fixed columns + separators for perfect alignment */
.queue-head, .queue-row{
  display:grid;
  gap: 0; /* separators act as gaps */
  padding: 2px 8px;
  font-size: .9em;
  align-items:center;
  font-variant-numeric: tabular-nums;
  min-width:0;
}
.queue-head{
  background: var(--panel-bg2);
  border-bottom: 1px solid var(--border);
  text-transform: uppercase;
  letter-spacing: .4px;
  color: var(--fg);
}
.queue-head.wpa, .queue-row.wpa{
  grid-template-columns:
    minmax(18ch, 1.55fr)
    minmax(10ch, 1.00fr)
    minmax(10ch, 1.00fr)
    10ch;
}
.queue-head.wigle, .queue-row.wigle{
  grid-template-columns:
    minmax(20ch, 1.60fr)
    7ch
    10ch;
}

.queue-head > div, .queue-row > div{
  overflow:hidden;
  text-overflow:ellipsis;
  white-space:nowrap;
  min-width:0;
}
.queue-head > div:not(:first-child),
.queue-row > div:not(:first-child){
  padding-left: 1ch;
}
.queue-list{
  flex:1;
  min-height:0;
  overflow-y:auto;
  background: var(--panel-bg);
}

.queue-dim{ opacity:.88; color: var(--dim); }
.queue-row .queue-dim{ color: var(--dim); }

/* Column-specific alignment */
.queue-head.wigle > div:nth-child(2),
.queue-row.wigle > div:nth-child(2){
  text-align:right;
  padding-right: 1ch;
}
.queue-head .queue-status{ text-align:center; }
.queue-row .queue-status{
  text-align:left;
  font-weight: 600;
  letter-spacing: .2px;
}
.queue-row.wigle .queue-status{ text-align:left; }

.status-ok{ color: var(--dr-green); }
.status-wait{ color: var(--dr-yellow); }
.status-local{ color: var(--dr-cyan); }
.status-cracked{ color: var(--dr-orange); }

/* ----------------------------------------------------------------------
   Buttons - keep behavior, render like MC-like flat buttons
   ---------------------------------------------------------------------- */
.btn{
  background: transparent;
  color: var(--fg);
  border: 1px solid var(--border-soft);
  padding: 3px 10px;
  cursor:pointer;
  font-family: inherit;
  font-size: .9em;
  letter-spacing: .3px;
}
.btn:hover{ background: rgba(68,71,90,.40); border-color: var(--border); }
.btn:disabled{ opacity:.35; cursor:not-allowed; }
.btn:focus-visible,
.fkey:focus-visible,
.file-prefix:focus-visible{
  outline: 2px solid var(--dr-green);
  outline-offset: -2px;
}
.btn-outline{
  background: transparent;
  color: var(--fg);
  border: 1px solid var(--border);
}
.btn-outline:hover{ background: rgba(68,71,90,.52); }

/* ----------------------------------------------------------------------
   Function key bar + status line
   ---------------------------------------------------------------------- */
.fkey-bar{
  display:flex;
  background: var(--panel-bg);
  border-top: 1px solid var(--border);
  flex-shrink:0;
}
.fkey{
  flex:1;
  padding: 4px 6px;
  text-align:center;
  font-size: .9em;
  font-family:inherit;
  color:var(--fg);
  background:transparent;
  appearance:none;
  border:0;
  border-right: 1px solid var(--border-soft);
  border-radius:0;
  cursor:pointer;
  user-select:none;
  white-space:nowrap;
  overflow:hidden;
  text-overflow:ellipsis;
  touch-action:manipulation;
}
.fkey:last-child{ border-right:none; }
.fkey:hover{ background: rgba(68,71,90,.40); }
.fkey span{
  display:inline-block;
  padding: 0 4px;
  margin-right: 6px;
  background: var(--key-bg);
  color: var(--key-fg);
  border-radius: 0;
}

.status{
  padding: 4px var(--pad-x);
  font-size: .95em;
  background: var(--panel-bg2);
  color: var(--fg);
  border-top: 1px solid var(--border);
  min-height: 22px;
  flex-shrink:0;
  white-space:nowrap;
  overflow:hidden;
  text-overflow:ellipsis;
}

/* Progress bar: keep tiny, MC-like */
.progress-bar{
  height: 4px;
  background: rgba(68,71,90,.55);
  display:none;
}
.progress-bar.active{ display:block; }
.progress-fill{
  height:100%;
  background: var(--dr-green);
  width:0%;
  transition: width .1s linear;
}

/* ----------------------------------------------------------------------
   Dialogs / Modals (MC dialog vibe)
   ---------------------------------------------------------------------- */
.modal{
  display:none;
  position:fixed;
  inset:0;
  background: rgba(0,0,0,.85);
  justify-content:center;
  align-items:center;
  z-index:100;
}
.modal-content{
  background: var(--panel-bg);
  border: 1px solid var(--border);
  box-shadow: 0 14px 40px var(--shadow);
  padding: 14px;
  max-width: 460px;
  width: 92%;
}
.modal-content h3{
  margin: -14px -14px 12px -14px;
  padding: 6px 10px;
  font-weight: normal;
  background: var(--title-active-bg);
  color: var(--title-active-fg);
  border-bottom: 1px solid var(--border);
  letter-spacing: .6px;
}
.modal-body{
  font-size: .95em;
  line-height: 1.5;
  opacity: .95;
}
.modal-tip{
  margin-top: 8px;
  color: var(--dim);
  font-size: .9em;
}
.modal-actions{
  display:flex;
  flex-wrap:wrap;
  gap: 10px;
  margin-top: 12px;
}

/* Log console */
.log-console{
  background: var(--panel-bg);
  border: 1px solid var(--border);
  box-shadow: 0 14px 40px var(--shadow);
  padding: 0;
  max-width: 620px;
  width: 92%;
}
.log-console h3{
  margin: 0;
  padding: 6px 10px;
  font-weight: normal;
  background: var(--title-active-bg);
  color: var(--title-active-fg);
  border-bottom: 1px solid var(--border);
}
.log-console pre{
  font-family: inherit;
  font-size: .95em;
  line-height: 1.35;
  margin: 0;
  padding: 10px;
  background: #191A22;
  color: var(--fg);
  min-height: 7em;
  white-space: pre-wrap;
}

/* ----------------------------------------------------------------------
   Embedded editor (mcedit-ish)
   ---------------------------------------------------------------------- */
.editor-content{
  background: var(--panel-bg);
  border: 1px solid var(--border);
  box-shadow: 0 14px 40px var(--shadow);
  padding: 0;
  max-width: 820px;
  width: 94%;
  height: 82vh;
  display:flex;
  flex-direction:column;
  gap: 0;
}
.editor-header{
  background: var(--title-active-bg);
  color: var(--title-active-fg);
  padding: 6px 10px;
  font-size: .95em;
  display:flex;
  justify-content:space-between;
  align-items:center;
  border-bottom: 1px solid var(--border);
}
.editor-title{
  font-weight: normal;
  letter-spacing: .6px;
}
.editor-meta{ opacity:.9; }
.editor-body{
  flex:1;
  display:flex;
  background: #191A22;
}
.editor-textarea{
  width:100%;
  height:100%;
  resize:none;
  background: transparent;
  color: var(--fg);
  border:none;
  font-family: inherit;
  font-size: .95em;
  line-height: 1.35;
  padding: 10px;
}
.editor-textarea:focus{ outline:none; }
.editor-footer{
  background: var(--panel-bg2);
  border-top: 1px solid var(--border);
  padding: 6px 10px;
  font-size: .9em;
  display:flex;
  justify-content:space-between;
  align-items:center;
}
.editor-keys span{
  display:inline-block;
  padding: 0 4px;
  margin: 0 2px;
  background: var(--key-bg);
  color: var(--key-fg);
  border-radius: 0;
}
.editor-status{ color: var(--fg); opacity: .95; }

/* Inputs */
input[type="text"],
input[type="password"]{
  background: #191A22;
  color: var(--fg);
  border: 1px solid var(--border);
  padding: 8px;
  font-family: inherit;
  width: 100%;
}
input[type="text"]:focus,
input[type="password"]:focus{
  outline:none;
  border-color: var(--dr-cyan);
  box-shadow: 0 0 0 2px rgba(255,0,128,.18);
}

/* Responsive: keep the same logic, collapse like before */
@media (max-width: 600px){
  .main{ grid-template-rows: 1fr 1fr; }
  .panes{ flex-direction:column; margin: var(--frame-gap); }
  .pane + .pane{ border-left:none; border-top: 1px solid var(--border); }
  .ops{ grid-template-columns: 1fr; }
  .ops-panel + .ops-panel{ border-left:none; border-top: 1px solid var(--border); }
  .col-header, .file-item{
    grid-template-columns: 4ch minmax(0, 1fr) 11ch;
  }
  .col-size, .file-size{ display:none; }
  .queue-head.wpa, .queue-row.wpa{
    grid-template-columns: minmax(9ch, 1.35fr) minmax(6ch, .9fr) minmax(6ch, .9fr) 8ch;
  }
  .queue-head.wigle, .queue-row.wigle{
    grid-template-columns: minmax(10ch, 1fr) 5ch 8ch;
  }
  .ops-title-wrap{ gap:4px; }
  .fkey-bar{
    display:grid;
    grid-template-columns:repeat(5, minmax(0, 1fr));
  }
  .fkey{ padding:7px 2px; }
  .fkey:nth-child(-n+5){ border-bottom:1px solid var(--border-soft); }
  .fkey:nth-child(5n){ border-right:none; }
  .fkey span{ margin-right:2px; padding:0 2px; }
  .modal{ padding:8px; }
  .modal-content, .log-console{
    width:100%;
    max-height:calc(100vh - 16px);
    max-height:calc(100dvh - 16px);
    overflow:auto;
  }
}

@media (max-height: 600px){
  body.mc{
    min-height:600px;
    overflow:auto;
  }
}
)rawliteral";

// ==[ PROGMEM — JAVASCRIPT ]== dual-pane MC commander with WPA/WiGLE queues
static const char HTML_SCRIPT[] PROGMEM = R"rawliteral(

// Pane state
const DEFAULT_LEFT = '/hamlet/export';
const DEFAULT_RIGHT = '/hamlet/wardrive';
const HANDSHAKES_DIR = '/hamlet/export';
const WIGLE_DIR = '/hamlet/wardrive';
const panes = {
    L: { path: '/', items: [], selected: new Set(), focusIdx: 0, loading: false },
    R: { path: '/', items: [], selected: new Set(), focusIdx: 0, loading: false }
};
let activePane = 'L';
let sdInfoLoading = false;
let refreshInProgress = false;
let refreshPending = false;
let lastRefreshAt = 0;
let fetchQueue = Promise.resolve();
const LIST_LIMIT = 200;
const DEFAULT_STATUS = 'READY | TAP/ARROWS NAV | [ ]/SPACE SEL | DOUBLE TAP/ENTER EXEC';
let selectionStatusPane = '';
let opsBusy = false;
let queueLoading = false;
const creds = {
    wpaKey: '',
    wigleUser: '',
    wigleToken: ''
};
let wpaQueue = [];
let wigleQueue = [];
let wpaResultsHandle = null;
let swineTimer = null;
let wpaAuthGateShown = false;
let wpaAuthState = 'REQUIRED';
let wpaAuthModalPromise = null;
let wpaAuthModalResolve = null;
const EDIT_MAX_BYTES = 2048;
let editPath = '';
let editDirty = false;
let editLoading = false;
const LOG_MAX = 5;
let logBuffer = [];

function queuedFetch(url, options) {
    const run = () => fetch(url, options);
    const p = fetchQueue.then(run, run);
    fetchQueue = p.catch(() => {});
    return p;
}

function showModalElement(modal, focusSelector = '') {
    if (!modal) return;
    modal.style.display = 'flex';
    if (!focusSelector) return;
    const target = modal.querySelector(focusSelector);
    if (target) target.focus();
}

function hideModalElement(modal) {
    if (!modal) return;
    const active = document.activeElement;
    if (active && modal.contains(active)) active.blur();
    modal.style.display = 'none';
}

// Initialize
document.addEventListener('DOMContentLoaded', () => {
    document.addEventListener('keydown', handleKeydown);
    bootstrap();
});

async function bootstrap() {
    await loadConfigFromDevice();
    await loadPane('L', DEFAULT_LEFT);
    await loadPane('R', DEFAULT_RIGHT);
    await loadSDInfo();
    await loadSwine();
    if (!swineTimer) {
        swineTimer = setInterval(loadSwine, 15000);
    }
    initWpaPicker();
    await loadQueues();
    addSysLog('COMMANDER ONLINE');
}

function setActivePane(id) {
    const changed = activePane !== id;
    activePane = id;
    document.getElementById('paneL').classList.toggle('active', id === 'L');
    document.getElementById('paneR').classList.toggle('active', id === 'R');
    if (changed) {
        renderPane('L');
        renderPane('R');
        updateSelectionInfo(id, true);
    }
}

async function loadConfigFromDevice() {
    creds.wpaKey = '';
    creds.wigleUser = '';
    creds.wigleToken = '';
    try {
        const r = await queuedFetch('/api/creds');
        if (r.ok) {
            const cfg = await r.json();
            creds.wpaKey = (cfg.wpasec_key || '').trim();
            creds.wigleUser = (cfg.wigle_user || '').trim();
            creds.wigleToken = (cfg.wigle_token || '').trim();
        }
    } catch(e) {
        // keep defaults
    }
    updateCredsStatus();
}

function updateCredsStatus() {
    const wpa = creds.wpaKey ? 'LOADED' : 'MISSING';
    const wigle = (creds.wigleUser && creds.wigleToken) ? 'LOADED' : 'MISSING';
    const wpaEl = document.getElementById('wpaMeta');
    const wigleEl = document.getElementById('wigleMeta');
    if (wpaEl) {
        wpaEl.textContent = 'KEY: ' + wpa;
    }
    if (wigleEl) wigleEl.textContent = 'CREDS: ' + wigle;
}

function setWpaAuthState(state) {
    wpaAuthState = state;
    updateCredsStatus();
}

function setOpsBusy(busy) {
    opsBusy = busy;
    const ids = ['btnWpaSync', 'btnWpaOpen', 'btnWigleSync'];
    ids.forEach(id => {
        const el = document.getElementById(id);
        if (el) el.disabled = busy;
    });
    const wpaPick = document.getElementById('wpaPick');
    if (wpaPick) wpaPick.disabled = busy;
}

function formatLogLine(source, msg) {
    const ts = new Date().toLocaleTimeString();
    return ts + ' [' + source + '] ' + msg;
}

function renderLogConsole() {
    const log = document.getElementById('logConsole');
    if (!log) return;
    log.textContent = logBuffer.length ? logBuffer.join('\n') : 'NO LOGS';
}

function pushLog(source, msg) {
    const line = formatLogLine(source, msg);
    logBuffer.push(line);
    while (logBuffer.length > LOG_MAX) {
        logBuffer.shift();
    }
    setStatus('[' + source + '] ' + msg);
    renderLogConsole();
}

function showLogConsole() {
    renderLogConsole();
    const modal = document.getElementById('logModal');
    showModalElement(modal, '.btn');
}

function hideLogConsole() {
    const modal = document.getElementById('logModal');
    hideModalElement(modal);
}

function addWpaLog(msg) {
    pushLog('WPA', msg);
}

function addWigleLog(msg) {
    pushLog('WIGLE', msg);
}

function addSysLog(msg) {
    pushLog('SYS', msg);
}

async function loadSDInfo() {
    if (sdInfoLoading) return;
    sdInfoLoading = true;
    try {
        const r = await queuedFetch('/api/sdinfo');
        const d = await r.json();
        const pct = ((d.used / d.total) * 100).toFixed(0);
        const usedStr = formatSize(d.used * 1024);
        const totalStr = formatSize(d.total * 1024);
        document.getElementById('sdInfo').textContent = usedStr + ' / ' + totalStr + ' (' + pct + '%)';
        updateAllFooterDisk(usedStr, totalStr, pct);
    } catch(e) {
        document.getElementById('sdInfo').textContent = 'NO SD. NO LOOT.';
        updateAllFooterDisk('--', '--', '?');
    } finally {
        sdInfoLoading = false;
    }
}

function formatNumber(value) {
    try {
        return Number(value || 0).toLocaleString('en-US');
    } catch (e) {
        return String(value || 0);
    }
}

function renderSwineHeader(data) {
    if (!data) return;
    const line1 = document.getElementById('swineLine1');
    const line2 = document.getElementById('swineLine2');
    if (!line1 || !line2) return;
    const level = data.level || 0;
    const xp = formatNumber(data.xp || 0);
    line1.textContent = 'LV' + level + ' | XP ' + xp;
    const rank = data.rank || 0;
    const wifi = data.wifi || 0;
    if (rank > 0) {
        line2.textContent = 'W1GL3 #' + rank + ' | W:' + wifi;
    } else {
        line2.textContent = 'W1GL3 N/A';
    }
}

async function loadSwine() {
    try {
        const r = await queuedFetch('/api/swine');
        if (!r.ok) return;
        const data = await r.json();
        renderSwineHeader(data);
    } catch (e) {
        // ignore
    }
}

async function loadPane(id, path) {
    const pane = panes[id];
    if (pane.loading) return;
    pane.loading = true;
    pane.path = path;
    pane.selected.clear();
    pane.focusIdx = 0;
    
    document.getElementById('path' + id).textContent = path || '/';
    const list = document.getElementById('list' + id);
    list.innerHTML = '<div style="padding:20px;opacity:0.5">jacking in...</div>';
    
    try {
        const r = await queuedFetch('/api/ls?path=' + encodeURIComponent(path));
        const items = await r.json();
        items.forEach(i => { if(i.n !== undefined) { i.name = i.n; i.isDir = !!i.d; i.size = i.s || 0; i.mtime = i.t || 0; } });
        
        // Sort: directories first, then alphabetically
        pane.items = [];
        
        // Parent directory entry
        if (path !== '/') {
            pane.items.push({ name: '..', isDir: true, isParent: true, size: 0 });
        }
        
        // Directories
        items.filter(i => i.isDir).sort((a,b) => a.name.localeCompare(b.name))
            .forEach(i => pane.items.push(i));
        
        // Files
        items.filter(i => !i.isDir).sort((a,b) => a.name.localeCompare(b.name))
            .forEach(i => pane.items.push(i));
        
        renderPane(id);
    } catch(e) {
        list.innerHTML = '<div style="padding:20px;opacity:0.5">load failed</div>';
    } finally {
        pane.loading = false;
        updateSelectionInfo(id);
    }
}

function renderPane(id) {
    const pane = panes[id];
    const list = document.getElementById('list' + id);
    
    if (pane.items.length === 0) {
        list.innerHTML = '<div style="padding:20px;opacity:0.4;text-align:center">void</div>';
        updatePaneFooter(id);
        return;
    }
    
    let html = '';
    pane.items.forEach((item, idx) => {
        const isSel = pane.selected.has(idx);
        const isFocus = (idx === pane.focusIdx && activePane === id);
        const cls = 'file-item' + (isSel ? ' selected' : '') + (isFocus ? ' focused' : '');
        
        // Explicit mark cell keeps selection usable on touch-only clients.
        let prefix = isSel ? '[x]' : '[ ]';
        let prefixCls = 'file-prefix file';
        let nameCls = 'file-name';
        if (item.isDir) {
            prefixCls = 'file-prefix dir';
            nameCls = 'file-name dir';
        } else if (isExecutable(item.name)) {
            prefixCls = 'file-prefix exec';
            nameCls = 'file-name exec';
        }
        if (item.isParent) {
            prefix = ' ^ ';
            prefixCls += ' parent';
        }
        if (isSel) prefixCls += ' marked';
        
        // Size column: UP--DIR for parent, empty for dirs, size for files
        let size = '';
        if (item.isParent) {
            size = 'UP--DIR';
        } else if (!item.isDir) {
            size = formatSize(item.size);
        }
        
        // Time column: use mtime if available
        const time = item.isParent ? '' : (item.mtime ? formatMtime(item.mtime) : '');
        
        html += '<div class="' + cls + '" data-idx="' + idx + '" data-pane="' + id + '"';
        html += ' onclick="onItemClick(event,' + idx + ',\'' + id + '\')"';
        html += ' ondblclick="onItemDblClick(event,' + idx + ',\'' + id + '\')">';
        if (item.isParent) {
            html += '<span class="' + prefixCls + '">' + prefix + '</span>';
        } else {
            const markLabel = (isSel ? 'Unmark ' : 'Mark ') + item.name;
            html += '<button type="button" class="' + prefixCls + '" aria-pressed="' + isSel + '" aria-label="' + escapeHtml(markLabel) + '">' + prefix + '</button>';
        }
        html += '<div class="' + nameCls + '">' + escapeHtml(item.name) + '</div>';
        html += '<div class="file-size">' + size + '</div>';
        html += '<div class="file-time">' + time + '</div>';
        html += '</div>';
    });
    list.innerHTML = html;
    
    // Scroll focused item into view
    const focused = list.querySelector('.focused');
    if (focused) focused.scrollIntoView({ block: 'nearest' });
    
    updatePaneFooter(id);
}

function isExecutable(name) {
    if (!name) return false;
    const lower = name.toLowerCase();
    return lower.endsWith('.sh') || lower.endsWith('.exe') || lower.endsWith('.bat') || lower.endsWith('.py');
}

function formatMtime(ts) {
    if (!ts) return '';
    const d = new Date(ts * 1000);
    const mon = ['Jan','Feb','Mar','Apr','May','Jun','Jul','Aug','Sep','Oct','Nov','Dec'][d.getMonth()];
    const day = String(d.getDate()).padStart(2, ' ');
    const hh = String(d.getHours()).padStart(2, '0');
    const mm = String(d.getMinutes()).padStart(2, '0');
    return mon + ' ' + day + ' ' + hh + ':' + mm;
}

function updatePaneFooter(id) {
    const pane = panes[id];
    const footerPath = document.getElementById('footerPath' + id);
    const footerDisk = document.getElementById('footerDisk' + id);
    if (footerPath) {
        footerPath.textContent = pane.path || '/';
    }
    // Disk info is updated separately via loadSDInfo
}

function updateAllFooterDisk(usedStr, totalStr, pct) {
    const diskText = usedStr + '/' + totalStr + ' (' + pct + '%)';
    const footerDiskL = document.getElementById('footerDiskL');
    const footerDiskR = document.getElementById('footerDiskR');
    if (footerDiskL) footerDiskL.textContent = diskText;
    if (footerDiskR) footerDiskR.textContent = diskText;
}

function onItemClick(event, idx, paneId) {
    setActivePane(paneId);
    panes[paneId].focusIdx = idx;

    if (event.target && event.target.classList.contains('file-prefix')) {
        event.stopPropagation();
        if (panes[paneId].items[idx].isParent) {
            renderPane(paneId);
            return;
        }
        toggleSelect(paneId, idx);
        return;
    }
    
    if (event.ctrlKey || event.metaKey) {
        toggleSelect(paneId, idx);
    } else if (event.shiftKey) {
        // Range select not implemented for simplicity
        toggleSelect(paneId, idx);
    } else {
        renderPane(paneId);
    }
}

function onItemDblClick(event, idx, paneId) {
    if (event && event.target && event.target.classList.contains('file-prefix')) return;
    const pane = panes[paneId];
    const item = pane.items[idx];
    
    if (item.isParent) {
        const parent = pane.path.substring(0, pane.path.lastIndexOf('/')) || '/';
        loadPane(paneId, parent);
    } else if (item.isDir) {
        const newPath = (pane.path === '/' ? '' : pane.path) + '/' + item.name;
        loadPane(paneId, newPath);
    } else {
        downloadFile(paneId, idx);
    }
}

function toggleSelect(paneId, idx) {
    const pane = panes[paneId];
    const item = pane.items[idx];
    if (item.isParent) return; // Can't select parent dir
    
    if (pane.selected.has(idx)) {
        pane.selected.delete(idx);
    } else {
        pane.selected.add(idx);
    }
    renderPane(paneId);
    updateSelectionInfo(paneId, true);
}

function updateSelectionInfo(id, clearWhenEmpty = false) {
    // Selection info now shown in status bar instead of removed pane elements
    const pane = panes[id];
    const count = pane.selected.size;
    if (count > 0) {
        setStatus('[' + count + ' SELECTED IN ' + id + '] | [ ]/SPACE TOGGLE | F5 COPY | F6 MOVE | F8 DELETE', id);
    } else if (id === activePane && (clearWhenEmpty || selectionStatusPane === id)) {
        setStatus(DEFAULT_STATUS);
    }
}

function initWpaPicker() {
    const input = document.getElementById('wpaPick');
    if (!input) return;
    input.addEventListener('change', async () => {
        const file = input.files && input.files[0] ? input.files[0] : null;
        if (file) {
            setOpsBusy(true);
            await applyWpasecResultsFile(file);
            setOpsBusy(false);
        }
        input.value = '';
    });
}

function showWpaAuthModal() {
    if (wpaAuthModalPromise) return wpaAuthModalPromise;
    wpaAuthModalPromise = new Promise(resolve => {
        wpaAuthModalResolve = resolve;
        const modal = document.getElementById('wpaAuthModal');
        showModalElement(modal, '.btn');
    });
    return wpaAuthModalPromise;
}

function closeWpaAuthModal(result) {
    const modal = document.getElementById('wpaAuthModal');
    hideModalElement(modal);
    if (wpaAuthModalResolve) {
        const resolve = wpaAuthModalResolve;
        wpaAuthModalResolve = null;
        resolve(result);
    }
    wpaAuthModalPromise = null;
}

function openWpaAuthTab() {
    if (!creds.wpaKey) {
        addWpaLog('AUTH BLOCKED: KEY MISSING - CLICK STATUS TO CONFIGURE');
        return;
    }
    window.open('https://wpa-sec.stanev.org/', '_blank');
    addWpaLog('AUTH TAB OPENED - PASTE KEY ON SITE IF NEEDED');
}

function handleWpaAuthAction(action) {
    if (action === 'auth') {
        openWpaAuthTab();
    }
    closeWpaAuthModal(action);
}

async function ensureWpaAuthGate(pendingCount) {
    if (pendingCount <= 0 || wpaAuthGateShown) return true;
    const result = await showWpaAuthModal();
    if (result === 'auth' || result === 'proceed') {
        wpaAuthGateShown = true;
        setWpaAuthState('ASSUMED');
        if (result === 'proceed') {
            addWpaLog('AUTH: PROCEED ANYWAY');
        }
        return true;
    }
    setWpaAuthState('REQUIRED');
    return false;
}

async function loadQueues() {
    if (queueLoading) return;
    queueLoading = true;
    try {
        wpaQueue = await buildWpaQueue();
        wigleQueue = await buildWigleQueue();
        renderWpaQueue();
        renderWigleQueue();
    } catch (e) {
        addSysLog('QUEUE LOAD FAILED: ' + describeError(e));
    } finally {
        queueLoading = false;
    }
}

async function listDir(path) {
    try {
        const r = await queuedFetch('/api/ls?path=' + encodeURIComponent(path));
        if (!r.ok) return [];
        const items = await r.json();
        if (Array.isArray(items)) items.forEach(i => { if(i.n !== undefined) { i.name = i.n; i.isDir = !!i.d; i.size = i.s || 0; i.mtime = i.t || 0; } });
        return Array.isArray(items) ? items : [];
    } catch (e) {
        return [];
    }
}

function stripExtension(name) {
    const dot = name.lastIndexOf('.');
    return dot > 0 ? name.substring(0, dot) : name;
}

function parseUploadedPaths(text) {
    const out = new Set();
    text.split(/\r?\n/).forEach(line => {
        const trimmed = line.trim();
        if (trimmed) out.add(trimmed);
    });
    return out;
}

function wigleBaseName(path) {
    const trimmed = (path || '').trim();
    return trimmed.split('/').pop() || trimmed;
}

function parseWigleTransactions(text) {
    const out = new Map();
    text.split(/\r?\n/).forEach(line => {
        const trimmed = line.trim();
        if (!trimmed || trimmed.startsWith('#')) return;
        const parts = trimmed.split('\t');
        if (parts.length < 4) return;
        const name = wigleBaseName(parts[0]);
        if (!name) return;
        out.set(name, {
            transId: parts[1] || '',
            status: parts[2] || 'W',
            size: parseInt(parts[3] || '0', 10) || 0
        });
    });
    return out;
}

function parseUploadedBssids(text) {
    const out = new Set();
    text.split(/\r?\n/).forEach(line => {
        const trimmed = line.trim();
        if (!trimmed) return;
        const bssid = normalizeBssid(trimmed);
        if (bssid.length >= 12) out.add(bssid);
    });
    return out;
}

function parseWpasecResultsMap(text) {
    const out = new Map();
    text.split(/\r?\n/).forEach(line => {
        const trimmed = line.trim();
        if (!trimmed || trimmed.startsWith('#')) return;
        if (trimmed.startsWith('WPA*')) {
            const lastColon = trimmed.lastIndexOf(':');
            if (lastColon <= 0) return;
            const hash = trimmed.substring(0, lastColon);
            const pass = trimmed.substring(lastColon + 1).trim();
            const parts = hash.split('*');
            if (parts.length >= 6) {
                const bssid = normalizeBssid(parts[3] || '');
                if (bssid.length < 12) return;
                const ssidHex = parts[5] || '';
                const ssid = decodeHexSSID(ssidHex);
                out.set(bssid, { ssid, pass });
            }
            return;
        }
        const first = trimmed.indexOf(':');
        const last = trimmed.lastIndexOf(':');
        if (first <= 0 || last <= first) return;
        const bssid = normalizeBssid(trimmed.substring(0, first));
        if (bssid.length < 12) return;
        const ssid = trimmed.substring(first + 1, last).trim();
        const pass = trimmed.substring(last + 1).trim();
        out.set(bssid, { ssid, pass });
    });
    return out;
}

function decodeHexSSID(hex) {
    if (!hex || hex.length < 2 || hex.length % 2 !== 0) return '';
    let out = '';
    for (let i = 0; i < hex.length; i += 2) {
        const byte = parseInt(hex.substr(i, 2), 16);
        if (isNaN(byte)) return '';
        if (byte === 0) continue;
        if (byte < 32 || byte > 126) {
            out += '.';
        } else {
            out += String.fromCharCode(byte);
        }
    }
    return out.trim();
}

async function buildWpaQueue() {
    const [items, uploadedText, sentText, resultsText] = await Promise.all([
        listDir(HANDSHAKES_DIR),
        fetchDeviceText('/hamlet/export/wpasec_uploaded.txt'),
        fetchDeviceText('/hamlet/export/wpasec_sent.txt'),
        fetchDeviceText('/hamlet/export/wpasec.pot')
    ]);
    const uploadedSet = parseUploadedBssids(uploadedText);
    const sentSet = parseUploadedBssids(sentText);
    const resultsMap = parseWpasecResultsMap(resultsText);
    const queue = [];
    for (const item of items) {
        if (!item || item.isDir) continue;
        if (!item.name || !item.name.toLowerCase().endsWith('.pcap')) continue;
        let base = stripExtension(item.name);
        if (base.endsWith('_hs')) base = base.substring(0, base.length - 3);
        const bssidKey = normalizeBssid(base);
        const result = resultsMap.get(bssidKey);
        const ssid = (await readHandshakeSSID(base)) || (result ? result.ssid : '');
        const pass = result ? result.pass : '';
        let status = 'LOCAL';
        if (bssidKey && result) status = 'CRACKED';
        else if (bssidKey && uploadedSet.has(bssidKey)) status = 'UPLOADED';
        else if (bssidKey && sentSet.has(bssidKey)) status = 'SENT';
        queue.push({
            path: HANDSHAKES_DIR + '/' + item.name,
            name: item.name,
            bssidKey,
            ssid: ssid || 'NONAME BRO',
            pass,
            status
        });
    }
    queue.sort((a, b) => a.name.localeCompare(b.name));
    return queue;
}

async function buildWigleQueue() {
    const [items, uploadedText, txText] = await Promise.all([
        listDir(WIGLE_DIR),
        fetchDeviceText('/hamlet/wardrive/.wigle_uploaded'),
        fetchDeviceText('/hamlet/wardrive/.wigle_transactions')
    ]);
    const uploadedSet = parseUploadedPaths(uploadedText);
    const txMap = parseWigleTransactions(txText);
    const queue = [];
    for (const item of items) {
        if (!item || item.isDir) continue;
        if (!item.name || !item.name.toLowerCase().endsWith('.csv')) continue;
        const path = WIGLE_DIR + '/' + item.name;
        let status = 'LOCAL';
        if (uploadedSet.has(path) || uploadedSet.has(item.name)) status = 'UPLOADED';
        else if (txMap.has(item.name)) status = 'PENDING';
        const nets = await countWigleNetworks(path);
        if (nets === null) continue;
        queue.push({ path, name: item.name, nets, size: item.size || 0, status });
    }
    queue.sort((a, b) => a.name.localeCompare(b.name));
    return queue;
}

async function readHandshakeSSID(baseName) {
    const path = HANDSHAKES_DIR + '/' + baseName + '.txt';
    const text = await fetchDeviceText(path);
    if (!text) return '';
    const line = text.split(/\r?\n/).find(l => l.trim());
    return line ? line.trim() : '';
}

function statusClassFor(status) {
    if (status === 'CRACKED') return 'status-cracked';
    if (status === 'UPLOADED') return 'status-ok';
    if (status === 'LOCAL') return 'status-local';
    return 'status-wait';
}

function renderWpaQueue() {
    const list = document.getElementById('wpaQueue');
    if (!list) return;
    if (!wpaQueue.length) {
        list.innerHTML = '<div class="queue-row wpa queue-dim"><div>--</div><div>--</div><div>--</div><div class="queue-status">EMPTY</div></div>';
        return;
    }
    let html = '';
    wpaQueue.forEach(item => {
        const dim = item.status === 'LOCAL' ? ' queue-dim' : '';
        const cls = statusClassFor(item.status);
        const pass = item.pass ? item.pass : '--';
        html += '<div class="queue-row wpa' + dim + '" title="' + escapeHtml(item.path) + '">';
        html += '<div>' + escapeHtml(item.name) + '</div>';
        html += '<div>' + escapeHtml(item.ssid) + '</div>';
        html += '<div>' + escapeHtml(pass) + '</div>';
        html += '<div class="queue-status ' + cls + '">' + item.status + '</div>';
        html += '</div>';
    });
    list.innerHTML = html;
}

function renderWigleQueue() {
    const list = document.getElementById('wigleQueue');
    if (!list) return;
    if (!wigleQueue.length) {
        list.innerHTML = '<div class="queue-row wigle queue-dim"><div>--</div><div>--</div><div class="queue-status">EMPTY</div></div>';
        return;
    }
    let html = '';
    wigleQueue.forEach(item => {
        const dim = item.status === 'LOCAL' ? ' queue-dim' : '';
        const cls = statusClassFor(item.status);
        const nets = (item.nets === undefined || item.nets === null) ? '?' : String(item.nets);
        html += '<div class="queue-row wigle' + dim + '" title="' + escapeHtml(item.path) + '">';
        html += '<div>' + escapeHtml(item.name) + '</div>';
        html += '<div>' + escapeHtml(nets) + '</div>';
        html += '<div class="queue-status ' + cls + '">' + item.status + '</div>';
        html += '</div>';
    });
    list.innerHTML = html;
}

function isWigleDataLine(line) {
    const trimmed = line.trim();
    if (!trimmed) return false;
    if (trimmed.startsWith('#')) return false;
    if (trimmed.startsWith('WigleWifi-')) return false;
    if (trimmed.startsWith('MAC,') || trimmed.startsWith('BSSID,')) return false;
    return true;
}

async function countWigleNetworks(path) {
    try {
        const resp = await queuedFetch('/download?path=' + encodeURIComponent(path));
        if (!resp.ok) return '?';
        if (!resp.body || !resp.body.getReader) {
            const text = await resp.text();
            const lines = text.split(/\r?\n/);
            const headers = lines.filter(line => line.trim()).slice(0, 2);
            if (headers.length < 2 || !headers[0].trim().startsWith('WigleWifi-') ||
                !headers[1].trim().startsWith('MAC,SSID,AuthMode,FirstSeen,')) return null;
            let count = 0;
            lines.forEach(line => { if (isWigleDataLine(line)) count++; });
            return count;
        }
        const reader = resp.body.getReader();
        const decoder = new TextDecoder();
        let carry = '';
        let count = 0;
        let headerStage = 0;
        let validHeader = false;
        const consumeLine = line => {
            const trimmed = line.trim();
            if (!trimmed) return;
            if (headerStage === 0) {
                validHeader = trimmed.startsWith('WigleWifi-');
                headerStage = 1;
                return;
            }
            if (headerStage === 1) {
                validHeader = validHeader && trimmed.startsWith('MAC,SSID,AuthMode,FirstSeen,');
                headerStage = 2;
                return;
            }
            if (validHeader && isWigleDataLine(line)) count++;
        };
        while (true) {
            const { value, done } = await reader.read();
            if (done) break;
            carry += decoder.decode(value, { stream: true });
            const lines = carry.split(/\r?\n/);
            carry = lines.pop() || '';
            lines.forEach(consumeLine);
        }
        if (carry) consumeLine(carry);
        return validHeader && headerStage === 2 ? count : null;
    } catch (e) {
        return '?';
    }
}

function selectAll() {
    const pane = panes[activePane];
    pane.items.forEach((item, idx) => {
        if (!item.isParent) pane.selected.add(idx);
    });
    renderPane(activePane);
    updateSelectionInfo(activePane, true);
}

function handleKeydown(e) {
    // Don't handle if in modal input
    const activeTag = document.activeElement ? document.activeElement.tagName : '';
    if (activeTag === 'INPUT' || activeTag === 'TEXTAREA') return;
    if (activeTag === 'BUTTON') {
        if (e.key === 'Enter' || e.key === ' ' || e.key === 'Tab') return;
        document.activeElement.blur();
    }
    const editModal = document.getElementById('editModal');
    if (editModal && editModal.style.display === 'flex') {
        if (e.key === 'Escape') {
            e.preventDefault();
            closeEditModal(false);
        }
        return;
    }
    const logModal = document.getElementById('logModal');
    if (logModal && logModal.style.display === 'flex') {
        if (e.key === 'Escape' || e.key === 'F10') {
            e.preventDefault();
            hideLogConsole();
        }
        return;
    }
    const blockingModalIds = ['newFolderModal', 'helpModal', 'renameModal', 'wpaAuthModal', 'credsModal'];
    const blockingModal = blockingModalIds.find(id => {
        const modal = document.getElementById(id);
        return modal && modal.style.display === 'flex';
    });
    if (blockingModal) {
        if (e.key === 'Escape') {
            e.preventDefault();
            if (blockingModal === 'wpaAuthModal') handleWpaAuthAction('cancel');
            else if (blockingModal === 'credsModal') hideCredsModal();
            else hideModal();
        }
        return;
    }
    if (e.ctrlKey && (e.key === 'Enter' || e.key === 'NumpadEnter')) {
        e.preventDefault();
        downloadSelected();
        return;
    }
    
    const pane = panes[activePane];
    
    switch(e.key) {
        case 'ArrowUp':
            e.preventDefault();
            if (pane.focusIdx > 0) {
                pane.focusIdx--;
                renderPane(activePane);
            }
            break;
        case 'ArrowDown':
            e.preventDefault();
            if (pane.focusIdx < pane.items.length - 1) {
                pane.focusIdx++;
                renderPane(activePane);
            }
            break;
        case 'Enter':
            e.preventDefault();
            onItemDblClick(null, pane.focusIdx, activePane);
            break;
        case ' ':
            e.preventDefault();
            toggleSelect(activePane, pane.focusIdx);
            break;
        case 'Tab':
            e.preventDefault();
            setActivePane(activePane === 'L' ? 'R' : 'L');
            break;
        case 'Backspace':
            e.preventDefault();
            if (pane.path !== '/') {
                const parent = pane.path.substring(0, pane.path.lastIndexOf('/')) || '/';
                loadPane(activePane, parent);
            }
            break;
        case 'Delete':
            e.preventDefault();
            deleteSelected();
            break;
        case 'a':
            if (e.ctrlKey || e.metaKey) {
                e.preventDefault();
                selectAll();
            }
            break;
        case 'F1':
            e.preventDefault();
            showHelp();
            break;
        case 'F2':
            e.preventDefault();
            showRenameModal();
            break;
        case 'F3':
            e.preventDefault();
            refresh();
            break;
        case 'F4':
            e.preventDefault();
            showEditModal();
            break;
        case 'F5':
            e.preventDefault();
            copySelected();
            break;
        case 'F6':
            e.preventDefault();
            moveSelected();
            break;
        case 'F7':
            e.preventDefault();
            showNewFolderModal();
            break;
        case 'F8':
            e.preventDefault();
            deleteSelected();
            break;
        case 'F9':
            e.preventDefault();
            triggerUploadPicker();
            break;
        case 'F10':
            e.preventDefault();
            showLogConsole();
            break;
    }
}

function getSelectedPaths(paneId = activePane) {
    const paths = [];
    const pane = panes[paneId];
    pane.selected.forEach(idx => {
        const item = pane.items[idx];
        if (item && !item.isParent) {
            const path = (pane.path === '/' ? '' : pane.path) + '/' + item.name;
            paths.push({ path, isDir: item.isDir });
        }
    });
    return paths;
}

async function deleteSelected() {
    const items = getSelectedPaths();
    if (items.length === 0) {
        addSysLog('SELECT TARGETS FIRST');
        return;
    }
    
    const msg = 'NUKE ' + items.length + ' ITEM(S)? NO UNDO. NO REGRETS.';
    if (!confirm(msg)) return;
    
    addSysLog('NUKING ' + items.length + ' TARGETS...');
    
    try {
        const resp = await queuedFetch('/api/bulkdelete', {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify({ paths: items.map(i => i.path) })
        });
        const result = await resp.json();
        addSysLog('NUKED ' + (result.n || result.deleted || 0) + '/' + items.length);
        refresh();
    } catch(e) {
        addSysLog('NUKE FAILED: ' + e.message);
    }
}

async function downloadSelected() {
    const items = getSelectedPaths().filter(i => !i.isDir);
    if (items.length === 0) {
        addSysLog('NO FILES MARKED. DIRS NEED ZIP. WE AINT GOT ZIP.');
        return;
    }
    
    addSysLog('EXFILTRATING ' + items.length + ' FILE(S)...');
    
    // Download files sequentially (browser limitation)
    for (let i = 0; i < items.length; i++) {
        await new Promise(resolve => {
            const a = document.createElement('a');
            a.href = '/download?path=' + encodeURIComponent(items[i].path);
            a.download = items[i].path.split('/').pop();
            a.click();
            setTimeout(resolve, 300); // Small delay between downloads
        });
    }
    
    addSysLog('EXFIL COMPLETE: ' + items.length);
}

function downloadFile(paneId, idx) {
    const pane = panes[paneId];
    const item = pane.items[idx];
    if (item.isDir) return;
    
    const path = (pane.path === '/' ? '' : pane.path) + '/' + item.name;
    window.location.href = '/download?path=' + encodeURIComponent(path);
}

async function refresh() {
    const now = Date.now();
    if (refreshInProgress) {
        refreshPending = true;
        return;
    }
    if (now - lastRefreshAt < 500) {
        return;
    }
    refreshInProgress = true;
    lastRefreshAt = now;
    await loadPane('L', panes.L.path);
    await loadPane('R', panes.R.path);
    await loadSDInfo();
    await loadSwine();
    await loadQueues();
    refreshInProgress = false;
    if (refreshPending) {
        refreshPending = false;
        refresh();
    }
}

function showNewFolderModal() {
    document.getElementById('newFolderName').value = '';
    showModalElement(document.getElementById('newFolderModal'), '#newFolderName');
}

function showHelp() {
    showModalElement(document.getElementById('helpModal'), '.btn');
}

function hideModal() {
    ['newFolderModal', 'helpModal', 'renameModal', 'credsModal', 'logModal'].forEach(id => {
        hideModalElement(document.getElementById(id));
    });
}

function showCredsModal() {
    document.getElementById('credWpaKey').value = creds.wpaKey || '';
    document.getElementById('credWigleName').value = creds.wigleUser || '';
    document.getElementById('credWigleToken').value = creds.wigleToken || '';
    showModalElement(document.getElementById('credsModal'), '#credWpaKey');
}

function hideCredsModal() {
    hideModalElement(document.getElementById('credsModal'));
}

async function saveCreds() {
    const wpaKey = document.getElementById('credWpaKey').value.trim();
    const wigleName = document.getElementById('credWigleName').value.trim();
    const wigleToken = document.getElementById('credWigleToken').value.trim();
    try {
        const r = await queuedFetch('/api/creds', {
            method: 'POST',
            headers: {'Content-Type': 'application/json'},
            body: JSON.stringify({
                wpasec_key: wpaKey,
                wigle_user: wigleName,
                wigle_token: wigleToken
            })
        });
        if (r.ok) {
            creds.wpaKey = wpaKey;
            creds.wigleUser = wigleName;
            creds.wigleToken = wigleToken;
            updateCredsStatus();
            hideCredsModal();
            addSysLog('CREDENTIALS SAVED');
        } else {
            addSysLog('CREDS SAVE FAILED: ' + r.status);
        }
    } catch(e) {
        addSysLog('CREDS SAVE ERROR');
    }
}

async function clearCreds() {
    document.getElementById('credWpaKey').value = '';
    document.getElementById('credWigleName').value = '';
    document.getElementById('credWigleToken').value = '';
    await saveCreds();
}

function getEditBytes(text) {
    return new TextEncoder().encode(text || '').length;
}

function limitEditBytes(text, maxBytes) {
    const enc = new TextEncoder();
    if (enc.encode(text).length <= maxBytes) return text;
    let end = text.length;
    while (end > 0 && enc.encode(text.slice(0, end)).length > maxBytes) {
        end--;
    }
    return text.slice(0, end);
}

function updateEditMeta() {
    const area = document.getElementById('editText');
    const meta = document.getElementById('editMeta');
    const status = document.getElementById('editStatus');
    if (!area || !meta || !status) return;
    const bytes = getEditBytes(area.value);
    meta.textContent = bytes + '/' + EDIT_MAX_BYTES + ' B';
    status.textContent = editDirty ? 'MODIFIED' : 'READY';
}

function closeEditModal(force) {
    if (!force && editDirty) {
        if (!confirm('DISCARD UNSAVED CHANGES?')) return;
    }
    const modal = document.getElementById('editModal');
    hideModalElement(modal);
    const area = document.getElementById('editText');
    if (area) {
        area.value = '';
        area.disabled = false;
    }
    editPath = '';
    editDirty = false;
    editLoading = false;
}

function handleEditKey(e) {
    if (e.ctrlKey && (e.key === 'o' || e.key === 'O' || e.key === 's' || e.key === 'S')) {
        e.preventDefault();
        saveEdit();
        return;
    }
    if (e.ctrlKey && (e.key === 'x' || e.key === 'X')) {
        e.preventDefault();
        closeEditModal(false);
        return;
    }
    if (e.key === 'Escape') {
        e.preventDefault();
        closeEditModal(false);
    }
}

function handleEditInput() {
    if (editLoading) return;
    const area = document.getElementById('editText');
    if (!area) return;
    const limited = limitEditBytes(area.value, EDIT_MAX_BYTES);
    if (limited !== area.value) {
        area.value = limited;
        addSysLog('EDIT: 2KB LIMIT');
    }
    editDirty = true;
    updateEditMeta();
}

async function showEditModal() {
    const pane = panes[activePane];
    const item = pane.items[pane.focusIdx];
    if (!item || item.isParent || item.isDir) { addSysLog('SELECT FILE TO EDIT'); return; }
    if (item.size > EDIT_MAX_BYTES) { addSysLog('EDIT: FILE TOO LARGE'); return; }

    const path = (pane.path === '/' ? '' : pane.path) + '/' + item.name;
    editPath = path;
    editDirty = false;
    editLoading = true;

    const modal = document.getElementById('editModal');
    const area = document.getElementById('editText');
    const title = document.getElementById('editTitle');
    const meta = document.getElementById('editMeta');
    const status = document.getElementById('editStatus');
    if (!modal || !area || !title || !meta || !status) return;

    title.textContent = 'EDIT - ' + item.name;
    meta.textContent = (item.size || 0) + '/' + EDIT_MAX_BYTES + ' B';
    status.textContent = 'LOADING...';
    area.value = '';
    area.disabled = true;
    showModalElement(modal);

    try {
        const text = await fetchDeviceText(path);
        if (text.length > EDIT_MAX_BYTES) {
            addSysLog('EDIT: FILE TOO LARGE');
            closeEditModal(true);
            return;
        }
        area.value = text;
        area.disabled = false;
        area.focus();
        editLoading = false;
        updateEditMeta();
    } catch (e) {
        editLoading = false;
        addSysLog('EDIT LOAD FAILED: ' + describeError(e));
        closeEditModal(true);
    }
}

async function saveEdit() {
    if (editLoading) return;
    if (!editPath) { addSysLog('EDIT: NO FILE'); return; }
    const area = document.getElementById('editText');
    if (!area) return;
    let text = area.value || '';
    text = limitEditBytes(text, EDIT_MAX_BYTES);
    if (text !== area.value) area.value = text;
    const bytes = getEditBytes(text);
    if (bytes > EDIT_MAX_BYTES) {
        addSysLog('EDIT: 2KB LIMIT');
        return;
    }

    const tempPath = editPath + '.tmp';
    const backupPath = editPath + '.bak';
    const originalPath = editPath;
    let backupCreated = false;

    addSysLog('EDIT SAVING: ' + originalPath);

    try {
        await queuedFetch('/delete', {method:'POST', headers:{'Content-Type':'application/json'}, body:JSON.stringify({path:tempPath})});
        await queuedFetch('/delete', {method:'POST', headers:{'Content-Type':'application/json'}, body:JSON.stringify({path:backupPath})});
        await uploadTextToDevice(tempPath, text, 'text/plain');
        const renameOrig = await queuedFetch('/api/rename?from=' + encodeURIComponent(originalPath) + '&to=' + encodeURIComponent(backupPath));
        if (renameOrig.ok) backupCreated = true;
        const renameTemp = await queuedFetch('/api/rename?from=' + encodeURIComponent(tempPath) + '&to=' + encodeURIComponent(originalPath));
        if (!renameTemp.ok) throw new Error('RENAME FAILED');
        if (backupCreated) {
            await queuedFetch('/delete', {method:'POST', headers:{'Content-Type':'application/json'}, body:JSON.stringify({path:backupPath})});
        }
        editDirty = false;
        updateEditMeta();
        closeEditModal(true);
        refresh();
        addSysLog('EDIT SAVED: ' + originalPath);
    } catch (e) {
        if (backupCreated) {
            await queuedFetch('/api/rename?from=' + encodeURIComponent(backupPath) + '&to=' + encodeURIComponent(originalPath));
        }
        await queuedFetch('/delete', {method:'POST', headers:{'Content-Type':'application/json'}, body:JSON.stringify({path:tempPath})});
        addSysLog('EDIT SAVE FAILED: ' + describeError(e));
    }
}

function showRenameModal() {
    const pane = panes[activePane];
    const item = pane.items[pane.focusIdx];
    if (!item || item.isParent) { addSysLog('SELECT ITEM TO RENAME'); return; }
    const path = (pane.path === '/' ? '' : pane.path) + '/' + item.name;
    document.getElementById('renameOldPath').value = path;
    const nameInput = document.getElementById('renameNewName');
    nameInput.value = item.name;
    showModalElement(document.getElementById('renameModal'));
    nameInput.select();
}

async function doRename() {
    const oldPath = document.getElementById('renameOldPath').value;
    const newName = document.getElementById('renameNewName').value.trim();
    if (!newName) { alert('PROVIDE NEW NAME'); return; }
    if (newName.includes('/') || newName.includes('..')) { alert('ILLEGAL CHARACTERS'); return; }
    
    const pane = panes[activePane];
    const newPath = (pane.path === '/' ? '' : pane.path) + '/' + newName;
    
    try {
        const resp = await queuedFetch('/api/rename?from=' + encodeURIComponent(oldPath) + '&to=' + encodeURIComponent(newPath));
        const result = await resp.json();
        if (result.ok) {
            addSysLog('RENAMED: ' + newName);
            hideModal();
            loadPane(activePane, pane.path);
            loadQueues();
        } else {
            addSysLog('RENAME FAILED: ' + (result.error || 'UNKNOWN'));
        }
    } catch(e) {
        addSysLog('FAULT: ' + e.message);
    }
}

async function copySelected() {
    const src = panes[activePane];
    const dst = panes[activePane === 'L' ? 'R' : 'L'];
    
    // Prevent copying to same directory
    if (src.path === dst.path) {
        addSysLog('SOURCE AND DEST ARE SAME DIRECTORY');
        return;
    }
    
    // Get selected or focused items
    let items = [];
    src.selected.forEach(idx => {
        const item = src.items[idx];
        if (item && !item.isParent) items.push(item);
    });
    if (!items.length && src.focusIdx >= 0) {
        const item = src.items[src.focusIdx];
        if (item && !item.isParent) items = [item];
    }
    if (!items.length) { addSysLog('SELECT FILES TO COPY'); return; }
    
    const paths = items.map(i => (src.path === '/' ? '' : src.path) + '/' + i.name);
    addSysLog('COPYING ' + items.length + ' ITEM(S)...');
    
    let copied = 0;
    for (const p of paths) {
        const name = p.split('/').pop();
        const dstPath = (dst.path === '/' ? '' : dst.path) + '/' + name;
        try {
            const resp = await queuedFetch('/api/copy', {
                method: 'POST',
                headers: {'Content-Type': 'application/json'},
                body: JSON.stringify({src: p, dst: dstPath})
            });
            const result = await resp.json();
            if (result.ok) copied++;
        } catch(e) {}
    }
    if (copied > 0) {
        addSysLog('COPIED: ' + copied + ' ITEM(S)');
        loadPane(activePane === 'L' ? 'R' : 'L', dst.path);
        loadQueues();
    } else {
        addSysLog('COPY FAILED');
    }
}

async function moveSelected() {
    const src = panes[activePane];
    const dst = panes[activePane === 'L' ? 'R' : 'L'];
    
    // Prevent moving to same directory
    if (src.path === dst.path) {
        addSysLog('SOURCE AND DEST ARE SAME DIRECTORY');
        return;
    }
    
    // Get selected or focused items
    let items = [];
    src.selected.forEach(idx => {
        const item = src.items[idx];
        if (item && !item.isParent) items.push(item);
    });
    if (!items.length && src.focusIdx >= 0) {
        const item = src.items[src.focusIdx];
        if (item && !item.isParent) items = [item];
    }
    if (!items.length) { addSysLog('SELECT FILES TO MOVE'); return; }
    
    const paths = items.map(i => (src.path === '/' ? '' : src.path) + '/' + i.name);
    addSysLog('MOVING ' + items.length + ' ITEM(S)...');
    
    let moved = 0;
    for (const p of paths) {
        const name = p.split('/').pop();
        const dstPath = (dst.path === '/' ? '' : dst.path) + '/' + name;
        try {
            const resp = await queuedFetch('/api/move', {
                method: 'POST',
                headers: {'Content-Type': 'application/json'},
                body: JSON.stringify({src: p, dst: dstPath})
            });
            const result = await resp.json();
            if (result.ok) moved++;
        } catch(e) {}
    }
    if (moved > 0) {
        addSysLog('MOVED: ' + moved + ' ITEM(S)');
        loadPane('L', panes.L.path);
        loadPane('R', panes.R.path);
        loadQueues();
    } else {
        addSysLog('MOVE FAILED');
    }
}

async function createFolder() {
    const name = document.getElementById('newFolderName').value.trim();
    if (!name) { alert('NAME THE DIRECTORY'); return; }
    if (name.includes('/') || name.includes('..')) { alert('ILLEGAL CHARACTERS'); return; }
    
    const pane = panes[activePane];
    const path = (pane.path === '/' ? '' : pane.path) + '/' + name;
    
    try {
        const resp = await queuedFetch('/mkdir', {
            method: 'POST',
            headers: {'Content-Type': 'application/json'},
            body: JSON.stringify({path: path})
        });
        if (resp.ok) {
            addSysLog('SPAWNED: ' + name);
            hideModal();
            loadPane(activePane, pane.path);
        } else {
            addSysLog('SPAWN FAILED');
        }
    } catch(e) {
        addSysLog('FAULT: ' + e.message);
    }
}

async function uploadFiles(files) {
    if (!files || !files.length) return;
    
    const pane = panes[activePane];
    const bar = document.getElementById('progressBar');
    const fill = document.getElementById('progressFill');
    bar.classList.add('active');
    bar.setAttribute('aria-valuenow', '0');
    
    let uploaded = 0;
    for (let i = 0; i < files.length; i++) {
        addSysLog('INJECTING ' + (i+1) + '/' + files.length + ': ' + files[i].name);
        fill.style.width = '0%';
        bar.setAttribute('aria-valuenow', '0');
        
        const formData = new FormData();
        formData.append('file', files[i]);
        
        try {
            await new Promise((resolve, reject) => {
                const xhr = new XMLHttpRequest();
                xhr.upload.onprogress = (e) => {
                    if (e.lengthComputable) {
                        const pct = Math.round(e.loaded/e.total*100);
                        fill.style.width = pct + '%';
                        bar.setAttribute('aria-valuenow', String(pct));
                    }
                };
                xhr.onload = () => xhr.status === 200 ? resolve() : reject();
                xhr.onerror = () => reject();
                xhr.open('POST', '/upload?path=' + encodeURIComponent(pane.path));
                xhr.send(formData);
            });
            uploaded++;
        } catch(e) {
            addSysLog('INJECT FAILED: ' + files[i].name);
        }
    }
    
    bar.classList.remove('active');
    bar.setAttribute('aria-valuenow', '0');
    addSysLog('INJECTED ' + uploaded + '/' + files.length + ' PAYLOADS');
    loadPane(activePane, pane.path);
    loadQueues();
    const input = document.getElementById('uploadPick');
    if (input) input.value = '';
}

function triggerUploadPicker() {
    const input = document.getElementById('uploadPick');
    if (input) {
        input.value = '';
        input.click();
    }
}

function normalizeBssid(raw) {
    return raw.replace(/[^a-fA-F0-9]/g, '').toUpperCase();
}

async function fetchDeviceBlob(path) {
    const resp = await queuedFetch('/download?path=' + encodeURIComponent(path));
    if (!resp.ok) throw new Error('device read failed (' + resp.status + ')');
    return await resp.blob();
}

async function fetchDeviceText(path) {
    const resp = await queuedFetch('/download?path=' + encodeURIComponent(path));
    if (!resp.ok) return '';
    return await resp.text();
}

async function uploadFileToDevice(dir, file) {
    const formData = new FormData();
    formData.append('file', file);
    const resp = await fetch('/upload?path=' + encodeURIComponent(dir || '/'), {
        method: 'POST',
        body: formData
    });
    if (!resp.ok) throw new Error('device write failed (' + resp.status + ')');
}

async function uploadTextToDevice(path, text, mime) {
    const slash = path.lastIndexOf('/');
    const dir = slash <= 0 ? '/' : path.substring(0, slash);
    const name = slash < 0 ? path : path.substring(slash + 1);
    const file = new File([text], name, { type: mime || 'text/plain' });
    await uploadFileToDevice(dir, file);
}

function mergeLines(existingText, additions) {
    const set = new Set();
    existingText.split(/\r?\n/).forEach(line => {
        const trimmed = line.trim();
        if (trimmed) set.add(trimmed);
    });
    additions.forEach(line => {
        if (line) set.add(line);
    });
    return Array.from(set).join('\n') + '\n';
}

function describeError(e) {
    if (!e) return 'REQUEST FAILED';
    const msg = e.message || String(e);
    if (msg === 'Failed to fetch') return 'PHONE INTERNET ROUTE BLOCKED - USE DEVICE LOOT UPLINK';
    return msg;
}

async function updateWpasecUploadedList(newBssid) {
    const path = '/hamlet/export/wpasec_uploaded.txt';
    const bssid = normalizeBssid(newBssid || '');
    if (!bssid || bssid.length < 12) return;
    const existing = await fetchDeviceText(path);
    const existingSet = parseUploadedBssids(existing);
    const already = existingSet.has(bssid);
    if (already) return false;
    const merged = mergeLines(existing, [bssid]);
    await uploadTextToDevice(path, merged, 'text/plain');
    return true;
}

async function updateWpasecSentList(newBssid) {
    const path = '/hamlet/export/wpasec_sent.txt';
    const bssid = normalizeBssid(newBssid || '');
    if (!bssid || bssid.length < 12) return;
    const existing = await fetchDeviceText(path);
    const existingSet = parseUploadedBssids(existing);
    const already = existingSet.has(bssid);
    if (already) return false;
    const merged = mergeLines(existing, [bssid]);
    await uploadTextToDevice(path, merged, 'text/plain');
    return true;
}

async function updateWpasecFromResults(text) {
    const resultsPath = '/hamlet/export/wpasec.pot';
    await uploadTextToDevice(resultsPath, text, 'text/plain');
    const resultsMap = parseWpasecResultsMap(text);
    return resultsMap.size;
}

async function updateWigleUploadedList(fullPath) {
    const path = '/hamlet/wardrive/.wigle_uploaded';
    const trimmed = (fullPath || '').trim();
    if (!trimmed) return;
    const existing = await fetchDeviceText(path);
    const existingSet = parseUploadedPaths(existing);
    const base = wigleBaseName(trimmed);
    const already = existingSet.has(trimmed) || existingSet.has(base);
    if (already) return false;
    const merged = mergeLines(existing, [trimmed]);
    await uploadTextToDevice(path, merged, 'text/plain');
    return true;
}

async function updateWigleTransactionList(fullPath, transId, size, status) {
    const path = '/hamlet/wardrive/.wigle_transactions';
    const name = wigleBaseName(fullPath);
    const txid = (transId || '').trim();
    if (!name || !txid) return false;
    const txStatus = (status || 'W').trim().charAt(0) || 'W';
    const byteSize = Number.isFinite(size) && size > 0 ? Math.floor(size) : 0;
    const existing = await fetchDeviceText(path);
    const lines = [];
    let replaced = false;
    existing.split(/\r?\n/).forEach(line => {
        const trimmed = line.trim();
        if (!trimmed) return;
        if (trimmed.startsWith('#')) {
            if (!lines.length) lines.push('#hamlet-wigle-tx-v1');
            return;
        }
        const parts = trimmed.split('\t');
        if (wigleBaseName(parts[0] || '') === name) {
            lines.push([name, txid, txStatus, String(byteSize)].join('\t'));
            replaced = true;
        } else {
            lines.push(trimmed);
        }
    });
    if (!lines.length) lines.push('#hamlet-wigle-tx-v1');
    if (!replaced) lines.push([name, txid, txStatus, String(byteSize)].join('\t'));
    await uploadTextToDevice(path, lines.join('\n') + '\n', 'text/plain');
    return true;
}

async function wpaSync() {
    if (opsBusy) return;
    if (!creds.wpaKey) {
        addWpaLog('KEY MISSING - CLICK STATUS TO CONFIGURE');
        return;
    }
    const pendingPre = wpaQueue.filter(item => item.status === 'LOCAL');
    if (pendingPre.length) {
        const ok = await ensureWpaAuthGate(pendingPre.length);
        if (!ok) {
            addWpaLog('SYNC CANCELLED');
            return;
        }
    }
    setOpsBusy(true);
    await applyWpasecResultsAuto(true);
    const pending = wpaQueue.filter(item => item.status === 'LOCAL');
    if (pending.length) {
        addWpaLog('SYNC BLOCKED: DEVICE LOOT UPLINK REQUIRED');
        await wpaUploadItem(pending[0]);
    } else {
        addWpaLog('SYNC: NOTHING PENDING');
    }
    const applied = await applyWpasecResultsAuto(false);
    setOpsBusy(false);
    if (applied && applied.error) {
        addWpaLog('APPLY FAIL: ' + applied.error);
    } else if (applied && applied.count !== undefined) {
        addWpaLog('APPLIED: ' + applied.count);
    } else if (applied && applied.pending) {
        addWpaLog('SELECT POTFILE TO APPLY');
    } else {
        addWpaLog('SYNC DONE');
    }
    await loadQueues();
}

function wpaOpenResults() {
    if (!creds.wpaKey) {
        addWpaLog('KEY MISSING - CLICK STATUS TO CONFIGURE');
        return;
    }
    window.open('https://wpa-sec.stanev.org/?api&dl=1', '_blank');
    addWpaLog('RESULTS TAB OPENED - SITE COOKIE REQUIRED');
}

async function applyWpasecResultsFile(file) {
    if (!file) return { count: 0 };
    addWpaLog('APPLY: ' + file.name);
    try {
        const text = await file.text();
        const count = await updateWpasecFromResults(text);
        addWpaLog('APPLIED: ' + count + ' ENTRIES');
        await loadQueues();
        return { count };
    } catch (e) {
        const msg = describeError(e);
        addWpaLog('APPLY FAIL: ' + msg);
        return { count: 0, error: msg };
    }
}

async function applyWpasecResultsAuto(allowPrompt) {
    try {
        if (window.showOpenFilePicker) {
            if (!wpaResultsHandle) {
                if (!allowPrompt) return { pending: true };
                const picks = await window.showOpenFilePicker({
                    multiple: false,
                    types: [{ description: 'WPA-SEC results', accept: { 'text/plain': ['.txt', '.potfile'] } }]
                });
                wpaResultsHandle = picks && picks.length ? picks[0] : null;
            }
            if (!wpaResultsHandle) {
                return { pending: true };
            }
            const file = await wpaResultsHandle.getFile();
            return await applyWpasecResultsFile(file);
        }
        const picker = document.getElementById('wpaPick');
        if (picker && allowPrompt) {
            picker.click();
            return { pending: true };
        }
    } catch (e) {
        const msg = describeError(e);
        wpaResultsHandle = null;
        addWpaLog('AUTO APPLY FAIL: ' + msg);
        return { error: msg };
    }
    return { pending: true };
}

async function wpaUploadItem(item) {
    addWpaLog('SEND: ' + item.name);
    addWpaLog('DEVICE LOOT UPLINK REQUIRED');
    addWpaLog('BROWSER WPA UPLOAD DISABLED: WPA-SEC NEEDS COOKIE + RESPONSE CHECK');
    return false;
}

async function wigleSync() {
    if (opsBusy) return;
    if (!creds.wigleUser || !creds.wigleToken) {
        addWigleLog('CREDS MISSING - CLICK STATUS TO CONFIGURE');
        return;
    }
    const pending = wigleQueue.filter(item => item.status === 'LOCAL');
    if (!pending.length) {
        addWigleLog('SYNC: NOTHING PENDING');
        return;
    }
    setOpsBusy(true);
    addWigleLog('SYNC START: ' + pending.length + ' FILE(S)');
    let okCount = 0;
    for (const item of pending) {
        const ok = await wigleUploadItem(item);
        if (ok) okCount++;
    }
    if (okCount > 0) {
        await wigleFetchStats();
        await loadSwine();
    }
    setOpsBusy(false);
    addWigleLog('SYNC SUBMITTED: ' + okCount + '/' + pending.length);
    await loadQueues();
}

async function wigleUploadItem(item) {
    addWigleLog('UPLOAD: ' + item.name);
    try {
        const blob = await fetchDeviceBlob(item.path);
        const file = new File([blob], item.name, { type: 'text/csv' });
        const auth = btoa(creds.wigleUser + ':' + creds.wigleToken);
        const form = new FormData();
        form.append('file', file);
        const resp = await fetch('https://api.wigle.net/api/v2/file/upload', {
            method: 'POST',
            headers: { 'Authorization': 'Basic ' + auth },
            body: form,
            mode: 'cors'
        });
        let data = null;
        try { data = await resp.json(); } catch (e) {}
        if (!resp.ok || !data || data.success !== true) {
            throw new Error((data && data.message) ? data.message : ('HTTP ' + resp.status));
        }
        const transids = data && data.results && Array.isArray(data.results.transids)
            ? data.results.transids : [];
        const trans = transids.find(t => t && (t.transId || t.transid));
        const transId = trans ? (trans.transId || trans.transid) : '';
        if (!transId) throw new Error('NO TRANSID IN RECEIPT');
        await updateWigleTransactionList(item.path, transId, item.size || blob.size || 0, 'W');
        addWigleLog('SUBMITTED: ' + item.name + ' TX ' + transId);
        return true;
    } catch (e) {
        const msg = describeError(e);
        addWigleLog('UPLOAD FAIL: ' + item.name + ' - ' + msg);
        return false;
    }
}

async function wigleFetchStats() {
    try {
        const auth = btoa(creds.wigleUser + ':' + creds.wigleToken);
        const statsResp = await fetch('https://api.wigle.net/api/v2/stats/user', {
            headers: { 'Authorization': 'Basic ' + auth },
            mode: 'cors'
        });
        if (!statsResp.ok) {
            addWigleLog('STATS FAIL: HTTP ' + statsResp.status);
            return false;
        }
        const statsJson = await statsResp.json();
        const stats = {};
        stats.rank = statsJson.rank || (statsJson.statistics ? statsJson.statistics.rank : 0) || 0;
        const s = statsJson.statistics || {};
        stats.wifi = s.discoveredWiFi || s.wifiCount || 0;
        stats.cell = s.discoveredCell || s.cellCount || 0;
        stats.bt = s.discoveredBt || s.btCount || 0;
        await uploadTextToDevice('/hamlet/wardrive/.wigle_stats.json', JSON.stringify(stats), 'application/json');
        addWigleLog('STATS SAVED');
        return true;
    } catch (e) {
        addWigleLog('STATS FAIL: ' + describeError(e));
        return false;
    }
}

function setStatus(msg, selectedPane = '') {
    document.getElementById('status').textContent = msg;
    selectionStatusPane = selectedPane;
}

function formatSize(bytes) {
    if (bytes < 1024) return bytes + 'B';
    if (bytes < 1024*1024) return (bytes/1024).toFixed(1) + 'K';
    if (bytes < 1024*1024*1024) return (bytes/1024/1024).toFixed(1) + 'M';
    return (bytes/1024/1024/1024).toFixed(2) + 'G';
}

function escapeHtml(s) {
    return s.replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/>/g,'&gt;').replace(/"/g,'&quot;');
}


)rawliteral";

// ==[ PROGMEM — HTML TEMPLATE ]== MC shell
static const char HTML_TEMPLATE[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>PANCETTA X-FER</title>
    <link rel="stylesheet" href="/ui.css">
</head>
<body class="mc">
    <div class="header">
        <h1>PANCETTA // XF3R M0D3</h1>
        <div class="sd-info" id="sdInfo">...</div>
    </div>
    <div class="swine-strip">
        <div class="swine-line" id="swineLine1">LV0 N0 D4TA | T13R N0NE | XP 0 (0%) | B4DG3S 0/0</div>
        <div class="swine-line" id="swineLine2">W1GL3 N/A | BUFF: N0N3</div>
    </div>
    
    <div class="main">
        <div class="panes">
            <div class="pane active" id="paneL" onclick="setActivePane('L')">
                <div class="pane-header">
                    <span class="pane-decor-left">────────────────────────────────────────────────────────────────────────────────┤</span>
                    <span class="pane-path" id="pathL">/</span>
                    <span class="pane-decor-right">├────────────────────────────────────────────────────────────────────────────────</span>
                </div>
                <div class="col-header">
                    <div class="col-sort">SEL</div>
                    <div class="col-name">Name</div>
                    <div class="col-size">Size</div>
                    <div class="col-time">Modif</div>
                </div>
                <div class="file-list" id="listL"></div>
                <div class="pane-footer">
                    <span class="pane-footer-decor-left">────────────────────────────────────────────────────────────────────────────────┤</span>
                    <span class="pane-footer-disk" id="footerDiskL"></span>
                    <span class="pane-footer-decor-right">├────────────────────────────────────────────────────────────────────────────────</span>
                </div>
            </div>
            <div class="pane" id="paneR" onclick="setActivePane('R')">
                <div class="pane-header">
                    <span class="pane-decor-left">────────────────────────────────────────────────────────────────────────────────┤</span>
                    <span class="pane-path" id="pathR">/</span>
                    <span class="pane-decor-right">├────────────────────────────────────────────────────────────────────────────────</span>
                </div>
                <div class="col-header">
                    <div class="col-sort">SEL</div>
                    <div class="col-name">Name</div>
                    <div class="col-size">Size</div>
                    <div class="col-time">Modif</div>
                </div>
                <div class="file-list" id="listR"></div>
                <div class="pane-footer">
                    <span class="pane-footer-decor-left">────────────────────────────────────────────────────────────────────────────────┤</span>
                    <span class="pane-footer-disk" id="footerDiskR"></span>
                    <span class="pane-footer-decor-right">├────────────────────────────────────────────────────────────────────────────────</span>
                </div>
            </div>
        </div>
    <div class="ops">
        <div class="ops-panel">
            <div class="ops-block">
                <div class="ops-header">
                    <span class="ops-decor-left">────────────────────────────────────────────────────────────────────────────────┤</span>
                    <span class="ops-title-wrap">
                        <span class="ops-title">WPA-SEC QUEUE</span>
                        <span class="ops-meta ops-meta-link" id="wpaMeta" onclick="showCredsModal()">KEY: UNKNOWN</span>
                        <span class="ops-actions">
                            <button class="btn btn-outline" id="btnWpaOpen" onclick="wpaOpenResults()">POT FILE</button>
                        </span>
                    </span>
                    <span class="ops-decor-right">├────────────────────────────────────────────────────────────────────────────────</span>
                </div>
                <div class="queue-head wpa">
                    <div>FILE</div><div>SSID</div><div>PASS</div><div class="queue-status">ST</div>
                </div>
                <div class="queue-list" id="wpaQueue"></div>
                <input type="file" id="wpaPick" accept=".txt,.potfile" style="display:none">
            </div>
        </div>
        <div class="ops-panel">
            <div class="ops-block">
                <div class="ops-header">
                    <span class="ops-decor-left">────────────────────────────────────────────────────────────────────────────────┤</span>
                    <span class="ops-title-wrap">
                        <span class="ops-title">WIGLE QUEUE</span>
                        <span class="ops-meta ops-meta-link" id="wigleMeta" onclick="showCredsModal()">CREDS: UNKNOWN</span>
                        <span class="ops-actions">
                            <button class="btn" id="btnWigleSync" onclick="wigleSync()">SYNC</button>
                        </span>
                    </span>
                    <span class="ops-decor-right">├────────────────────────────────────────────────────────────────────────────────</span>
                </div>
                <div class="queue-head wigle">
                    <div>FILE</div><div>NETS</div><div class="queue-status">ST</div>
                </div>
                <div class="queue-list" id="wigleQueue"></div>
            </div>
        </div>
    </div>
    </div>
    
    <div class="progress-bar" id="progressBar" role="progressbar" aria-label="Upload progress" aria-valuemin="0" aria-valuemax="100" aria-valuenow="0"><div class="progress-fill" id="progressFill"></div></div>
    
    <div class="status" id="status" role="status" aria-live="polite">READY | TAP/ARROWS NAV | [ ]/SPACE SEL | DOUBLE TAP/ENTER EXEC</div>

    
    <div class="fkey-bar" role="toolbar" aria-label="File actions">
        <button type="button" class="fkey" onclick="showHelp()"><span>F1</span>?</button>
        <button type="button" class="fkey" onclick="showRenameModal()"><span>F2</span>REN</button>
        <button type="button" class="fkey" onclick="refresh()"><span>F3</span>REF</button>
        <button type="button" class="fkey" onclick="showEditModal()"><span>F4</span>EDT</button>
        <button type="button" class="fkey" onclick="copySelected()"><span>F5</span>CPY</button>
        <button type="button" class="fkey" onclick="moveSelected()"><span>F6</span>MOV</button>
        <button type="button" class="fkey" onclick="showNewFolderModal()"><span>F7</span>MKD</button>
        <button type="button" class="fkey" onclick="deleteSelected()"><span>F8</span>DEL</button>
        <button type="button" class="fkey" onclick="triggerUploadPicker()"><span>F9</span>PUT</button>
        <button type="button" class="fkey" onclick="showLogConsole()"><span>F10</span>LOG</button>
    </div>
<input type="file" id="uploadPick" multiple onchange="uploadFiles(this.files)" style="display:none">
    
    <!-- New Folder Modal -->
    <div class="modal" id="newFolderModal" role="dialog" aria-modal="true" aria-label="New folder" onclick="if(event.target===this)hideModal()">
        <div class="modal-content">
            <h3>NEW FOLDER</h3>
            <input type="text" id="newFolderName" placeholder="FOLDER NAME" 
                   onkeydown="if(event.key==='Enter')createFolder();if(event.key==='Escape')hideModal()">
            <div class="modal-actions">
                <button class="btn" onclick="createFolder()">CREATE</button>
                <button class="btn btn-outline" onclick="hideModal()">CANCEL</button>
            </div>
        </div>
    </div>
    
    <!-- Help Modal -->
    <div class="modal" id="helpModal" role="dialog" aria-modal="true" aria-label="Keyboard shortcuts" onclick="if(event.target===this)hideModal()">
        <div class="modal-content">
            <h3>KEYBOARD SHORTCUTS</h3>
            <pre style="font-size:0.85em;line-height:1.6;opacity:0.8">
ARROW UP/DOWN  NAVIGATE FILES
ENTER          OPEN FOLDER / DOWNLOAD
TAP [ ]/SPACE  TOGGLE SELECTION
TAB            SWITCH PANE
CTRL+A         SELECT ALL
F2             RENAME FOCUSED ITEM
F3             REFRESH
F4             EDIT FILE (<=2KB)
F5             COPY SEL → OTHER PANE
F6             MOVE SEL → OTHER PANE
F7             NEW FOLDER
F8/DELETE      DELETE SEL IN ACTIVE PANE
F9             UPLOAD
F10            LOG CONSOLE
CTRL+ENTER     MULTI DOWNLOAD
BACKSPACE      PARENT FOLDER
            </pre>
            <div class="modal-actions">
                <button class="btn" onclick="hideModal()">CLOSE</button>
            </div>
        </div>
    </div>

    <!-- Log Console Modal -->
    <div class="modal" id="logModal" role="dialog" aria-modal="true" aria-label="Log console" onclick="if(event.target===this)hideLogConsole()">
        <div class="log-console">
            <h3>LOG CONSOLE</h3>
            <pre id="logConsole">NO LOGS</pre>
            <div class="modal-actions">
                <button class="btn" onclick="hideLogConsole()">CLOSE</button>
            </div>
        </div>
    </div>

    <!-- Edit Modal -->
    <div class="modal" id="editModal" role="dialog" aria-modal="true" aria-labelledby="editTitle" onclick="if(event.target===this)closeEditModal(false)">
        <div class="modal-content editor-content">
            <div class="editor-header">
                <div class="editor-title" id="editTitle">EDIT</div>
                <div class="editor-meta" id="editMeta">0/2048 B</div>
            </div>
            <div class="editor-body">
                <textarea class="editor-textarea" id="editText" onkeydown="handleEditKey(event)" oninput="handleEditInput()"></textarea>
            </div>
            <div class="editor-footer">
                <div class="editor-keys"><span>^O</span> SAVE  <span>^X</span> CANCEL</div>
                <div class="editor-status" id="editStatus">READY</div>
            </div>
        </div>
    </div>
    
    <!-- Rename Modal -->
    <div class="modal" id="renameModal" role="dialog" aria-modal="true" aria-label="Rename item" onclick="if(event.target===this)hideModal()">
        <div class="modal-content">
            <h3>RENAME</h3>
            <input type="text" id="renameNewName" placeholder="NEW NAME"
                   onkeydown="if(event.key==='Enter')doRename();if(event.key==='Escape')hideModal()">
            <input type="hidden" id="renameOldPath">
            <div class="modal-actions">
                <button class="btn" onclick="doRename()">RENAME</button>
                <button class="btn btn-outline" onclick="hideModal()">CANCEL</button>
            </div>
        </div>
    </div>

    <!-- WPA-SEC Auth Modal -->
    <div class="modal" id="wpaAuthModal" role="dialog" aria-modal="true" aria-label="WPA-SEC authentication" onclick="if(event.target===this)handleWpaAuthAction('cancel')">
        <div class="modal-content">
            <h3>WPA-SEC AUTH</h3>
            <div class="modal-body">
                AUTH ONCE IN A SEPARATE TAB, THEN RETURN HERE.<br>
                UPLOADS WITHOUT AUTH WON'T BE CREDITED.
            </div>
            <div class="modal-actions">
                <button class="btn" onclick="handleWpaAuthAction('auth')">AUTH NOW</button>
                <button class="btn btn-outline" onclick="handleWpaAuthAction('proceed')">PROCEED</button>
                <button class="btn btn-outline" onclick="handleWpaAuthAction('cancel')">CANCEL</button>
            </div>
        </div>
    </div>

    <!-- Credentials Modal -->
    <div class="modal" id="credsModal" role="dialog" aria-modal="true" aria-label="API credentials" onclick="if(event.target===this)hideCredsModal()">
        <div class="modal-content">
            <h3>API CREDENTIALS</h3>
            <div style="margin-bottom:10px">
                <div style="color:var(--dim);font-size:0.85em;margin-bottom:4px;text-transform:uppercase;letter-spacing:0.3px">WPA-SEC KEY (32 HEX CHARS)</div>
                <input type="password" id="credWpaKey" placeholder="WPA-SEC API KEY" maxlength="32" spellcheck="false" autocomplete="new-password"
                       onkeydown="if(event.key==='Escape')hideCredsModal()">
            </div>
            <div style="margin-bottom:10px">
                <div style="color:var(--dim);font-size:0.85em;margin-bottom:4px;text-transform:uppercase;letter-spacing:0.3px">WIGLE API NAME</div>
                <input type="text" id="credWigleName" placeholder="WIGLE API NAME" maxlength="64" spellcheck="false" autocomplete="off"
                       onkeydown="if(event.key==='Escape')hideCredsModal()">
            </div>
            <div style="margin-bottom:10px">
                <div style="color:var(--dim);font-size:0.85em;margin-bottom:4px;text-transform:uppercase;letter-spacing:0.3px">WIGLE API TOKEN</div>
                <input type="password" id="credWigleToken" placeholder="WIGLE API TOKEN" maxlength="64" spellcheck="false" autocomplete="new-password"
                       onkeydown="if(event.key==='Escape')hideCredsModal()">
            </div>
            <div class="modal-tip">SAVED TO DEVICE CONFIG. PERSISTS ACROSS REBOOTS.</div>
            <div class="modal-actions">
                <button class="btn" onclick="saveCreds()">SAVE</button>
                <button class="btn btn-outline" onclick="clearCreds()">CLEAR ALL</button>
                <button class="btn btn-outline" onclick="hideCredsModal()">CANCEL</button>
            </div>
        </div>
    </div>

<script src="/ui.js"></script>
</body>
</html>
)rawliteral";

// ==[ HELPERS ]==

// stream PROGMEM blob in 512-byte chunks
static void streamProgmem(const char* pgm, const char* contentType) {
    server.setContentLength(CONTENT_LENGTH_UNKNOWN);
    server.send(200, contentType, "");
    static char chunk[512];
    const char* p = pgm;
    size_t total = strlen_P(pgm);
    size_t rem = total;
    while (rem > 0) {
        size_t n = (rem > sizeof(chunk)) ? sizeof(chunk) : rem;
        memcpy_P(chunk, p, n);
        server.sendContent(chunk, n);
        p += n;
        rem -= n;
    }
    txBytes += total;
}

static constexpr size_t kXferChunkSize = 4096;

static uint8_t* getXferChunkBuffer() {
    static uint8_t* buf = nullptr;
    if (buf) return buf;

    buf = (uint8_t*)heap_caps_malloc(kXferChunkSize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) {
        buf = (uint8_t*)heap_caps_malloc(kXferChunkSize, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    return buf;
}

// normalize and confine paths to /hamlet
static bool pathOk(String& p) {
    if (p.isEmpty()) return false;
    if (p.charAt(0) != '/') return false;
    if (p.indexOf('\\') >= 0) return false;

    String normalized;
    normalized.reserve(p.length());

    int pos = 1;
    bool firstSeg = true;
    while (true) {
        while (pos < p.length() && p.charAt(pos) == '/') pos++;
        if (pos >= p.length()) break;

        int next = p.indexOf('/', pos);
        if (next < 0) next = p.length();
        String seg = p.substring(pos, next);
        if (seg.isEmpty() || seg == "." || seg == "..") return false;

        if (firstSeg) {
            if (seg != "hamlet") return false;
            normalized = "/hamlet";
            firstSeg = false;
        } else {
            normalized += "/";
            normalized += seg;
        }
        pos = next + 1;
    }

    if (firstSeg) return false;
    p = normalized;
    return true;
}

static bool uploadNameOk(const String& name) {
    if (name.isEmpty() || name.indexOf('/') >= 0 || name.indexOf('\\') >= 0 ||
        name == "." || name == "..") return false;
    for (size_t i = 0; i < name.length(); i++) {
        if ((uint8_t)name.charAt(i) < 0x20) return false;
    }
    return true;
}

// prevent copy/move to self or subpath
static bool isSameOrSubPath(const String& src, const String& dst) {
    if (dst == src) return true;
    if (dst.startsWith(src) && dst.length() > src.length() && dst.charAt(src.length()) == '/') return true;
    return false;
}

// json string escape
static String jsonEsc(const char* s) {
    String r;
    r.reserve(strlen(s) + 4);
    for (const char* p = s; *p; p++) {
        char c = *p;
        if (c == '"')  { r += '\\'; r += '"'; }
        else if (c == '\\') { r += '\\'; r += '\\'; }
        else if (c == '\n') { r += '\\'; r += 'n'; }
        else if (c == '\r') { /* skip */ }
        else r += c;
    }
    return r;
}

static const char* basename(const char* path) {
    const char* b = strrchr(path, '/');
    return b ? b + 1 : path;
}

// extract "key":"value" from simple JSON
static String jsonExtract(const String& json, const char* key) {
    String search = String("\"") + key + "\"";
    int ki = json.indexOf(search);
    if (ki < 0) return "";
    int colon = json.indexOf(':', ki + search.length());
    if (colon < 0) return "";
    int qs = json.indexOf('"', colon + 1);
    if (qs < 0) return "";
    int qe = qs + 1;
    while (qe < (int)json.length()) {
        qe = json.indexOf('"', qe);
        if (qe < 0) return "";
        // check if this quote is escaped
        if (qe > 0 && json[qe - 1] == '\\') { qe++; continue; }
        break;
    }
    return json.substring(qs + 1, qe);
}

// ==[ FILE OPERATIONS ]==

// recursive delete with depth limit and yield
static bool deletePathRecursive(const char* path, uint8_t depth) {
    if (depth > 12) return false;

    File entry = SD.open(path);
    if (!entry) return false;

    if (!entry.isDirectory()) {
        entry.close();
        return SD.remove(path);
    }

    // directory: delete contents first
    File child = entry.openNextFile();
    uint8_t ops = 0;
    while (child) {
        char childPath[256];
        int written = snprintf(childPath, sizeof(childPath), "%s/%s", path, basename(child.name()));
        if (written >= (int)sizeof(childPath)) {
            HAMLET_LOGF("[XFER] path too long, skipping: %s/...\n", path);
            child.close();
            child = entry.openNextFile();
            continue;
        }
        bool isDir = child.isDirectory();
        child.close();

        if (isDir) {
            deletePathRecursive(childPath, depth + 1);
        } else {
            SD.remove(childPath);
        }

        if (++ops % 4 == 0) yield();
        child = entry.openNextFile();
    }
    entry.close();
    return SD.rmdir(path);
}

// Chunked file copy through the shared PSRAM-preferred buffer.
static bool copyFileChunked(const char* src, const char* dst) {
    File sf = SD.open(src, FILE_READ);
    if (!sf) return false;

    File df = SD.open(dst, FILE_WRITE);
    if (!df) { sf.close(); return false; }

    uint8_t* buf = getXferChunkBuffer();
    if (!buf) {
        sf.close();
        df.close();
        return false;
    }

    uint32_t deadline = millis() + 30000;
    bool ok = true;

    uint8_t ops = 0;
    while (sf.available() && ok) {
        if ((millis() - deadline) < 0x80000000UL) { ok = false; break; }
        int n = sf.read(buf, kXferChunkSize);
        if (n > 0) {
            if (df.write(buf, n) != (size_t)n) ok = false;
        }
        if (++ops % 8 == 0) yield();
    }

    sf.close();
    df.close();

    if (!ok) SD.remove(dst);
    return ok;
}

// recursive copy with depth limit
static bool copyPathRecursive(const char* src, const char* dst, uint8_t depth) {
    if (depth > 12) return false;

    File entry = SD.open(src);
    if (!entry) return false;

    if (!entry.isDirectory()) {
        entry.close();
        return copyFileChunked(src, dst);
    }

    // directory: create dst dir, copy contents
    entry.close();
    SD.mkdir(dst);

    File dir = SD.open(src);
    if (!dir) return false;

    File child = dir.openNextFile();
    bool ok = true;
    while (child && ok) {
        const char* name = basename(child.name());
        char srcChild[256], dstChild[256];
        int srcWritten = snprintf(srcChild, sizeof(srcChild), "%s/%s", src, name);
        if (srcWritten >= (int)sizeof(srcChild)) {
            HAMLET_LOGF("[XFER] path too long, skipping: %s/...\n", src);
            child.close();
            child = dir.openNextFile();
            continue;
        }
        int dstWritten = snprintf(dstChild, sizeof(dstChild), "%s/%s", dst, name);
        if (dstWritten >= (int)sizeof(dstChild)) {
            HAMLET_LOGF("[XFER] path too long, skipping: %s/...\n", dst);
            child.close();
            child = dir.openNextFile();
            continue;
        }
        bool isDir = child.isDirectory();
        child.close();

        if (isDir) {
            ok = copyPathRecursive(srcChild, dstChild, depth + 1);
        } else {
            ok = copyFileChunked(srcChild, dstChild);
        }
        child = dir.openNextFile();
    }
    dir.close();
    return ok;
}

// ==[ XP HELPERS ]==

static int xpCmp(const void* a, const void* b) {
    return strcmp((const char*)a, (const char*)b);
}

static void loadXpList(char (**list)[40], uint16_t* count, bool* loaded, const char* path) {
    if (*loaded) return;
    *loaded = true;

    if (!*list) {
        *list = (char(*)[40])heap_caps_malloc(XP_MAX_ENTRIES * 40, MALLOC_CAP_SPIRAM);
        if (!*list) return;
    }
    *count = 0;

    File f = SD.open(path, FILE_READ);
    if (!f) return;

    char line[64];
    int lineLen = 0;
    while (f.available() && *count < XP_MAX_ENTRIES) {
        char c = f.read();
        if (c == '\n') {
            line[lineLen] = '\0';
            if (lineLen > 0 && lineLen < 40) {
                memcpy((*list)[*count], line, lineLen + 1);
                (*count)++;
            }
            lineLen = 0;
        } else if (c != '\r' && lineLen < 63) {
            line[lineLen++] = c;
        }
    }
    f.close();
    qsort(*list, *count, 40, xpCmp);
}

static bool isXpAwarded(char (*list)[40], uint16_t count, const char* name) {
    if (!list || count == 0) return false;
    return bsearch(name, list, count, 40, xpCmp) != nullptr;
}

static void markXpAwarded(char (**list)[40], uint16_t* count, const char* name, const char* path) {
    if (!*list || *count >= XP_MAX_ENTRIES) return;
    if (strlen(name) >= 40) return;

    strcpy((*list)[*count], name);
    (*count)++;
    qsort(*list, *count, 40, xpCmp);

    File f = SD.open(path, FILE_APPEND);
    if (f) { f.println(name); f.close(); }
}

// check uploaded file for XP award
static void checkAndAwardXp(const char* filename, const char* fullPath, uint32_t fileSize) {
    size_t nameLen = strlen(filename);
    if (nameLen < 5) return;

    const char* ext = filename + nameLen - 5;  // .pcap is 5 chars
    const char* ext4 = filename + nameLen - 4; // .csv is 4 chars

    // .pcap or .22000 — WPA capture
    bool isPcap = (nameLen > 5 && strcasecmp(ext, ".pcap") == 0);
    bool isHc = (nameLen > 6 && strcasecmp(filename + nameLen - 6, ".22000") == 0);
    if ((isPcap || isHc) && fileSize >= 300) {
        // validate magic bytes for pcap
        if (isPcap) {
            File f = SD.open(fullPath, FILE_READ);
            if (f) {
                uint32_t magic = 0;
                f.read((uint8_t*)&magic, 4);
                f.close();
                if (magic != 0xa1b2c3d4 && magic != 0xd4c3b2a1) return;
            }
        }

        loadXpList(&xpWpaList, &xpWpaCount, &xpWpaLoaded, "/hamlet/export/xp_wpa.txt");
        if (!isXpAwarded(xpWpaList, xpWpaCount, filename)) {
            if (xpSessionAwarded + 15 <= XP_SESSION_CAP) {
                markXpAwarded(&xpWpaList, &xpWpaCount, filename, "/hamlet/export/xp_wpa.txt");
                Config::addXP(15);
                xpSessionAwarded += 15;
            }
        }
        return;
    }

    // .csv — WiGLE wardrive
    if (nameLen > 4 && strcasecmp(ext4, ".csv") == 0 && fileSize >= 200) {
        // validate WiGLE header
        File f = SD.open(fullPath, FILE_READ);
        if (f) {
            char hdr[32] = {};
            f.read((uint8_t*)hdr, sizeof(hdr) - 1);
            f.close();
            if (!strstr(hdr, "WigleWifi")) return;
        }

        loadXpList(&xpWigleList, &xpWigleCount, &xpWigleLoaded, "/hamlet/wardrive/xp_wigle.txt");
        if (!isXpAwarded(xpWigleList, xpWigleCount, filename)) {
            if (xpSessionAwarded + 10 <= XP_SESSION_CAP) {
                markXpAwarded(&xpWigleList, &xpWigleCount, filename, "/hamlet/wardrive/xp_wigle.txt");
                Config::addXP(10);
                xpSessionAwarded += 10;
            }
        }
    }
}

// ==[ HTTP HANDLERS ]==

static void handleRoot() {
    streamProgmem(HTML_TEMPLATE, "text/html");
}

static void handleStyle() {
    server.sendHeader("Cache-Control", "public, max-age=3600");
    streamProgmem(HTML_STYLE, "text/css");
}

static void handleScript() {
    server.sendHeader("Cache-Control", "public, max-age=3600");
    streamProgmem(HTML_SCRIPT, "application/javascript");
}

// ==[ /api/ls — directory listing JSON ]==
static void handleFileList() {
    String path = server.arg("path");
    if (path.isEmpty()) path = server.arg("p");  // legacy compat
    if (path.isEmpty()) path = "/hamlet";
    if (!pathOk(path)) { server.send(403, "text/plain", "forbidden"); return; }
    if (!SDStorage::isAvailable()) { server.send(503, "text/plain", "no sd"); return; }

    File dir = SD.open(path);
    if (!dir || !dir.isDirectory()) {
        if (dir) dir.close();
        server.send(404, "text/plain", "not found");
        return;
    }

    server.setContentLength(CONTENT_LENGTH_UNKNOWN);
    server.send(200, "application/json", "");
    server.sendContent("[");

    bool first = true;
    File entry = dir.openNextFile();
    while (entry) {
        const char* name = basename(entry.name());
        if (name[0] != '.') {
            bool isDir = entry.isDirectory();
            time_t t = entry.getLastWrite();
            char buf[256];
            snprintf(buf, sizeof(buf),
                "%s{\"n\":\"%s\",\"s\":%lu,\"d\":%s,\"t\":%lu}",
                first ? "" : ",",
                jsonEsc(name).c_str(),
                (unsigned long)(isDir ? 0 : entry.size()),
                isDir ? "true" : "false",
                (unsigned long)t);
            server.sendContent(buf);
            first = false;
        }
        entry.close();
        entry = dir.openNextFile();
    }
    dir.close();
    server.sendContent("]");
}

// ==[ /download — file download ]==
static void handleDownload() {
    String path = server.arg("path");
    if (path.isEmpty()) path = server.arg("f");  // legacy compat
    if (!pathOk(path)) { server.send(403, "text/plain", "forbidden"); return; }
    if (!SDStorage::isAvailable()) { server.send(503, "text/plain", "no sd"); return; }

    File f = SD.open(path, FILE_READ);
    if (!f || f.isDirectory()) {
        if (f) f.close();
        server.send(404, "text/plain", "not found");
        return;
    }

    uint8_t* buf = getXferChunkBuffer();
    if (!buf) {
        f.close();
        server.send(503, "text/plain", "xfer buffer unavailable");
        return;
    }

    const char* fn = basename(path.c_str());
    char disp[96];
    snprintf(disp, sizeof(disp), "attachment; filename=\"%s\"", fn);
    server.sendHeader("Content-Disposition", disp);
    server.setContentLength(f.size());
    server.send(200, "application/octet-stream", "");

    size_t total = 0;
    uint8_t ops = 0;
    while (f.available()) {
        int n = f.read(buf, kXferChunkSize);
        if (n > 0) {
            server.sendContent((const char*)buf, n);
            total += n;
        }
        if (++ops % 8 == 0) yield();
    }
    f.close();
    txBytes += total;
    downloadCount++;
}

// ==[ /upload — multipart file upload ]==
static File uploadFile;
static uint32_t uploadRxTemp = 0;
static char uploadFullPath[256] = {};
static char uploadTempPath[256] = {};
static char uploadBackupPath[256] = {};
static bool uploadOk = false;

static void handleUploadProcess() {
    HTTPUpload& upload = server.upload();
    if (upload.status == UPLOAD_FILE_START) {
        uploadRxTemp = 0;
        uploadFullPath[0] = '\0';
        uploadTempPath[0] = '\0';
        uploadBackupPath[0] = '\0';
        uploadOk = false;

        if (!SDStorage::isAvailable()) return;

        String basePath = server.arg("path");
        if (basePath.isEmpty()) basePath = server.arg("p");  // legacy
        if (!pathOk(basePath)) return;
        String fileName = String(upload.filename.c_str());
        if (!uploadNameOk(fileName)) return;

        String fullPath = basePath;
        if (!fullPath.endsWith("/")) fullPath += "/";
        fullPath += fileName;
        if (!pathOk(fullPath)) return;
        if (fullPath.length() + 8 >= sizeof(uploadFullPath)) return;

        snprintf(uploadFullPath, sizeof(uploadFullPath), "%s", fullPath.c_str());
        snprintf(uploadTempPath, sizeof(uploadTempPath), "%s.upload", fullPath.c_str());
        snprintf(uploadBackupPath, sizeof(uploadBackupPath), "%s.bak", fullPath.c_str());
        SD.remove(uploadTempPath);
        if (!SD.exists(uploadFullPath) && SD.exists(uploadBackupPath) &&
            !SD.rename(uploadBackupPath, uploadFullPath)) return;
        uploadFile = SD.open(uploadTempPath, FILE_WRITE);
        if (uploadFile) uploadOk = true;

    } else if (upload.status == UPLOAD_FILE_WRITE) {
        if (uploadFile && uploadOk) {
            size_t written = uploadFile.write(upload.buf, upload.currentSize);
            if (written != upload.currentSize) {
                uploadOk = false;
                uploadFile.close();
                SD.remove(uploadTempPath);
            } else {
                uploadRxTemp += upload.currentSize;
            }
        }

    } else if (upload.status == UPLOAD_FILE_END) {
        if (uploadFile) {
            uint32_t fileSize = uploadFile.size();
            uploadFile.close();
            if (uploadOk) {
                bool hadOld = SD.exists(uploadFullPath);
                SD.remove(uploadBackupPath);
                if (hadOld && !SD.rename(uploadFullPath, uploadBackupPath)) {
                    uploadOk = false;
                }
                if (uploadOk && !SD.rename(uploadTempPath, uploadFullPath)) {
                    if (hadOld) SD.rename(uploadBackupPath, uploadFullPath);
                    uploadOk = false;
                }
                if (uploadOk) {
                    if (hadOld) SD.remove(uploadBackupPath);
                    rxBytes += uploadRxTemp;
                    uploadCount++;
                    const char* fn = basename(uploadFullPath);
                    checkAndAwardXp(fn, uploadFullPath, fileSize);
                } else {
                    SD.remove(uploadTempPath);
                }
            } else {
                SD.remove(uploadTempPath);
            }
        }
        uploadRxTemp = 0;
    } else if (upload.status == UPLOAD_FILE_ABORTED) {
        if (uploadFile) uploadFile.close();
        if (uploadTempPath[0]) SD.remove(uploadTempPath);
        uploadRxTemp = 0;
        uploadOk = false;
    }
}

static void handleUploadDone() {
    if (uploadOk) {
        server.send(200, "application/json", "{\"ok\":true}");
    } else {
        server.send(500, "application/json", "{\"ok\":false,\"err\":\"upload failed\"}");
    }
}

// ==[ /delete — POST JSON {path:...} ]==
static void handleDeleteJson() {
    if (!SDStorage::isAvailable()) { server.send(503, "text/plain", "no sd"); return; }

    String body = server.arg("plain");
    String path = jsonExtract(body, "path");
    if (!pathOk(path)) { server.send(403, "application/json", "{\"ok\":false,\"err\":\"forbidden\"}"); return; }

    File f = SD.open(path);
    if (!f) { server.send(404, "application/json", "{\"ok\":false,\"err\":\"not found\"}"); return; }

    bool isDir = f.isDirectory();
    f.close();

    bool ok;
    if (isDir) {
        ok = deletePathRecursive(path.c_str(), 0);
    } else {
        ok = SD.remove(path);
    }

    server.send(200, "application/json", ok ? "{\"ok\":true}" : "{\"ok\":false,\"err\":\"delete failed\"}");
}

// ==[ /delete legacy — DELETE ?f= ]==
static void handleDeleteLegacy() {
    String path = server.arg("f");
    if (!pathOk(path)) { server.send(403, "text/plain", "forbidden"); return; }
    if (!SDStorage::isAvailable()) { server.send(503, "text/plain", "no sd"); return; }
    if (SD.remove(path)) {
        server.send(200, "text/plain", "ok");
    } else {
        server.send(500, "text/plain", "failed");
    }
}

// ==[ /api/bulkdelete — POST JSON {paths:[...]} ]==
static void handleBulkDelete() {
    if (!SDStorage::isAvailable()) { server.send(503, "text/plain", "no sd"); return; }

    String body = server.arg("plain");
    int arrStart = body.indexOf('[');
    int arrEnd = body.lastIndexOf(']');
    if (arrStart < 0 || arrEnd < 0) {
        server.send(400, "application/json", "{\"ok\":false,\"err\":\"bad request\"}");
        return;
    }

    uint16_t deleted = 0;
    int pos = arrStart + 1;
    while (pos < arrEnd) {
        int qs = body.indexOf('"', pos);
        if (qs < 0 || qs >= arrEnd) break;
        int qe = body.indexOf('"', qs + 1);
        if (qe < 0 || qe > arrEnd) break;

        String path = body.substring(qs + 1, qe);
        if (pathOk(path)) {
            File f = SD.open(path);
            if (f) {
                bool isDir = f.isDirectory();
                f.close();
                if (isDir) {
                    if (deletePathRecursive(path.c_str(), 0)) deleted++;
                } else {
                    if (SD.remove(path)) deleted++;
                }
            }
        }
        pos = qe + 1;
    }

    char resp[40];
    snprintf(resp, sizeof(resp), "{\"ok\":true,\"n\":%u}", deleted);
    server.send(200, "application/json", resp);
}

// ==[ /mkdir — POST JSON {path:...} ]==
static void handleMkdir() {
    String body = server.arg("plain");
    String path = jsonExtract(body, "path");
    if (!pathOk(path)) { server.send(403, "application/json", "{\"ok\":false,\"err\":\"forbidden\"}"); return; }

    bool ok = SD.mkdir(path);
    server.send(200, "application/json", ok ? "{\"ok\":true}" : "{\"ok\":false,\"err\":\"mkdir failed\"}");
}

// ==[ /api/rename — GET ?from=&to= ]==
static void handleRename() {
    String from = server.arg("from");
    String to = server.arg("to");
    if (!pathOk(from) || !pathOk(to)) {
        server.send(403, "application/json", "{\"ok\":false,\"err\":\"forbidden\"}");
        return;
    }

    bool ok = SD.rename(from, to);
    server.send(200, "application/json", ok ? "{\"ok\":true}" : "{\"ok\":false,\"err\":\"rename failed\"}");
}

// ==[ /api/copy — POST JSON {src:...,dst:...} ]==
static void handleCopy() {
    String body = server.arg("plain");
    String src = jsonExtract(body, "src");
    String dst = jsonExtract(body, "dst");
    if (!pathOk(src) || !pathOk(dst)) {
        server.send(403, "application/json", "{\"ok\":false,\"err\":\"forbidden\"}");
        return;
    }
    if (isSameOrSubPath(src, dst)) {
        server.send(400, "application/json", "{\"ok\":false,\"err\":\"cannot copy to self/subpath\"}");
        return;
    }

    bool ok = copyPathRecursive(src.c_str(), dst.c_str(), 0);
    server.send(200, "application/json", ok ? "{\"ok\":true}" : "{\"ok\":false,\"err\":\"copy failed\"}");
}

// ==[ /api/move — POST JSON {src:...,dst:...} ]==
static void handleMove() {
    String body = server.arg("plain");
    String src = jsonExtract(body, "src");
    String dst = jsonExtract(body, "dst");
    if (!pathOk(src) || !pathOk(dst)) {
        server.send(403, "application/json", "{\"ok\":false,\"err\":\"forbidden\"}");
        return;
    }
    if (isSameOrSubPath(src, dst)) {
        server.send(400, "application/json", "{\"ok\":false,\"err\":\"cannot move to self/subpath\"}");
        return;
    }

    // try rename first (same filesystem, instant)
    if (SD.rename(src, dst)) {
        server.send(200, "application/json", "{\"ok\":true}");
        return;
    }

    // fallback: copy + delete
    bool ok = copyPathRecursive(src.c_str(), dst.c_str(), 0);
    if (ok) {
        deletePathRecursive(src.c_str(), 0);
    }
    server.send(200, "application/json", ok ? "{\"ok\":true}" : "{\"ok\":false,\"err\":\"move failed\"}");
}

// ==[ /api/creds — GET: return credentials ]==
static void handleCredsGet() {
    char buf[256];
    snprintf(buf, sizeof(buf),
        "{\"wpasec_key\":\"%s\",\"wigle_user\":\"%s\",\"wigle_token\":\"%s\"}",
        jsonEsc(Config::getWpaSecKey()).c_str(),
        jsonEsc(Config::getWigleUsername()).c_str(),
        jsonEsc(Config::getWigleToken()).c_str());
    server.send(200, "application/json", buf);
}

// ==[ /api/creds — POST: save credentials ]==
static void handleCredsSave() {
    String body = server.arg("plain");
    if (body.length() < 2 || body.length() > 256) {
        server.send(400, "application/json", "{\"ok\":false,\"err\":\"bad request\"}");
        return;
    }

    String wsk = jsonExtract(body, "wpasec_key");
    String wgu = jsonExtract(body, "wigle_user");
    String wgt = jsonExtract(body, "wigle_token");

    char oldWpaKey[33];
    snprintf(oldWpaKey, sizeof(oldWpaKey), "%s", Config::getWpaSecKey());
    Config::setWpaSecKey(wsk.c_str());
    bool wpaChanged = strcmp(oldWpaKey, Config::getWpaSecKey()) != 0;
    Config::setWigleUsername(wgu.c_str());
    Config::setWigleToken(wgt.c_str());
    if (wpaChanged) WpaSec::resetBackoff();
    Config::save();

    server.send(200, "application/json", "{\"ok\":true}");
}

// ==[ /api/swine — XP + WiGLE stats ]==
static void handleSwine() {
    WiGLE::UserStats ws = WiGLE::getCachedStats();
    char buf[128];
    snprintf(buf, sizeof(buf),
        "{\"xp\":%lu,\"level\":%u,\"rank\":%lu,\"wifi\":%lu,\"cell\":0}",
        (unsigned long)Config::getXP(),
        (unsigned)Config::getLevel(),
        (unsigned long)ws.rank,
        (unsigned long)ws.wifiNets);
    server.send(200, "application/json", buf);
}

// ==[ /api/sdinfo — SD card stats ]==
static void handleSDInfo() {
    uint64_t total = SDStorage::isAvailable() ? SD.totalBytes() : 0;
    uint64_t used  = SDStorage::isAvailable() ? SD.usedBytes() : 0;
    uint64_t free  = total > used ? total - used : 0;
    char buf[80];
    snprintf(buf, sizeof(buf),
        "{\"total\":%lu,\"used\":%lu,\"free\":%lu}",
        (unsigned long)(total / 1024),
        (unsigned long)(used / 1024),
        (unsigned long)(free / 1024));
    server.send(200, "application/json", buf);
}

// ==[ WPA-SEC POTFILE DATA ]==
static void handleWpasecData() {
    if (!SDStorage::isAvailable()) { server.send(200, "application/json", "[]"); return; }

    // collect captured BSSIDs for cross-ref
    uint8_t ourBssids[256][6];
    uint16_t ourCount = 0;
    {
        uint16_t n = Capture::getPMKIDCount();
        for (uint16_t i = 0; i < n && ourCount < 256; i++) {
            CapturedPMKID p;
            if (Capture::getPMKID(i, &p)) memcpy(ourBssids[ourCount++], p.bssid, 6);
        }
    }
    {
        uint16_t n = Capture::getHandshakeCount();
        for (uint16_t i = 0; i < n && ourCount < 256; i++) {
            CapturedHandshake h;
            if (Capture::getHandshake(i, &h)) memcpy(ourBssids[ourCount++], h.bssid, 6);
        }
    }

    File pot = SD.open("/hamlet/export/wpasec.pot", FILE_READ);
    if (!pot) {
        server.send(200, "application/json", "[]");
        return;
    }

    server.setContentLength(CONTENT_LENGTH_UNKNOWN);
    server.send(200, "application/json", "");
    server.sendContent("[");

    char line[160];
    bool first = true;

    uint16_t lineCount = 0;
    while (pot.available()) {
        int len = 0;
        while (pot.available() && len < (int)sizeof(line) - 1) {
            char c = (char)pot.read();
            if (c == '\n') break;
            if (c != '\r') line[len++] = c;
        }
        line[len] = '\0';
        if (++lineCount % 32 == 0) yield();
        if (len < 18) continue;

        int colonCount = 0, ssidStart = -1;
        for (int i = 0; i < len; i++) {
            if (line[i] == ':') {
                if (++colonCount == 6) { ssidStart = i + 1; break; }
            }
        }
        if (ssidStart < 18 || ssidStart >= len) continue;

        char bssidStr[18];
        memcpy(bssidStr, line, 17);
        bssidStr[17] = '\0';

        int passStart = -1;
        for (int i = ssidStart; i < len; i++) {
            if (line[i] == ':') { passStart = i + 1; break; }
        }
        if (passStart < 0 || passStart >= len) continue;

        char ssid[33] = {};
        int ssidLen = passStart - ssidStart - 1;
        if (ssidLen <= 0 || ssidLen > 32) continue;
        memcpy(ssid, line + ssidStart, ssidLen);

        char pass[65] = {};
        int passLen = len - passStart;
        if (passLen <= 0) continue;
        if (passLen > 64) passLen = 64;
        memcpy(pass, line + passStart, passLen);

        uint8_t bssidBytes[6] = {};
        sscanf(bssidStr, "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx",
               &bssidBytes[0], &bssidBytes[1], &bssidBytes[2],
               &bssidBytes[3], &bssidBytes[4], &bssidBytes[5]);
        bool captured = false;
        for (uint16_t i = 0; i < ourCount; i++) {
            if (memcmp(ourBssids[i], bssidBytes, 6) == 0) { captured = true; break; }
        }

        char entry[300];
        snprintf(entry, sizeof(entry),
            "%s{\"s\":\"%s\",\"p\":\"%s\",\"b\":\"%s\",\"c\":%s}",
            first ? "" : ",",
            jsonEsc(ssid).c_str(),
            jsonEsc(pass).c_str(),
            bssidStr,
            captured ? "true" : "false");
        server.sendContent(entry);
        first = false;
    }
    pot.close();
    server.sendContent("]");
}

// ==[ WIGLE DATA ]==
static void handleWigleData() {
    if (!SDStorage::isAvailable()) { server.send(200, "application/json", "[]"); return; }

    File dir = SD.open("/hamlet/wardrive");
    if (!dir || !dir.isDirectory()) {
        if (dir) dir.close();
        server.send(200, "application/json", "[]");
        return;
    }

    server.setContentLength(CONTENT_LENGTH_UNKNOWN);
    server.send(200, "application/json", "");
    server.sendContent("[");

    bool first = true;
    File entry = dir.openNextFile();
    while (entry) {
        const char* name = basename(entry.name());
        size_t nlen = strlen(name);
        if (!entry.isDirectory() && name[0] != '.' &&
            nlen > 4 && strcmp(name + nlen - 4, ".csv") == 0) {

            uint32_t fileSize = entry.size();
            uint32_t nets = 0;
            if (fileSize < 204800) {
                uint32_t byteCount = 0;
                while (entry.available()) {
                    if ((char)entry.read() == '\n') nets++;
                    if (++byteCount % 4096 == 0) yield();
                }
                if (nets > 2) nets -= 2;
                else nets = 0;
            }

            char buf[192];
            snprintf(buf, sizeof(buf),
                "%s{\"n\":\"%s\",\"s\":%lu,\"l\":%lu}",
                first ? "" : ",",
                jsonEsc(name).c_str(),
                (unsigned long)fileSize,
                (unsigned long)nets);
            server.sendContent(buf);
            first = false;
        }
        entry.close();
        entry = dir.openNextFile();
    }
    dir.close();
    server.sendContent("]");
}

// ==[ INFO ]==
static void handleInfo() {
    uint32_t upSec = Hamlet::getUptimeSeconds();
    uint32_t h = upSec / 3600, m = (upSec % 3600) / 60;
    uint64_t freeB = SDStorage::isAvailable() ? (uint64_t)SDStorage::freeBytes() : 0;
    char buf[320];
    snprintf(buf, sizeof(buf),
        "{\"firmware\":\"%s %s\","
        "\"uptime\":\"%uh%um\","
        "\"sd_free\":\"%.1fGB\","
        "\"heap_free\":\"%luKB\","
        "\"psram_free\":\"%luKB\","
        "\"clients\":\"%d\"}",
        HAMLET_VERSION, BUILD_COMMIT,
        h, m,
        (float)freeB / 1073741824.0f,
        (unsigned long)(ESP.getFreeHeap() / 1024),
        (unsigned long)(ESP.getFreePsram() / 1024),
        (int)WiFi.softAPgetStationNum());
    server.send(200, "application/json", buf);
    txBytes += strlen(buf);
}

static void handleNotFound() {
    server.sendHeader("Location", "http://192.168.4.1/");
    server.send(302, "text/plain", "");
}

// WebServer retains handlers after stop(). Register once per boot so repeated
// XFER sessions do not retain duplicate route allocations.
static void configureRoutes() {
    if (routesConfigured) return;

    server.on("/",              HTTP_GET,    handleRoot);
    server.on("/ui.css",        HTTP_GET,    handleStyle);
    server.on("/ui.js",         HTTP_GET,    handleScript);
    server.on("/api/ls",        HTTP_GET,    handleFileList);
    server.on("/download",      HTTP_GET,    handleDownload);
    server.on("/upload",        HTTP_POST,   handleUploadDone, handleUploadProcess);
    server.on("/delete",        HTTP_POST,   handleDeleteJson);
    server.on("/api/bulkdelete",HTTP_POST,   handleBulkDelete);
    server.on("/mkdir",         HTTP_POST,   handleMkdir);
    server.on("/api/rename",    HTTP_GET,    handleRename);
    server.on("/api/copy",      HTTP_POST,   handleCopy);
    server.on("/api/move",      HTTP_POST,   handleMove);
    server.on("/api/creds",     HTTP_GET,    handleCredsGet);
    server.on("/api/creds",     HTTP_POST,   handleCredsSave);
    server.on("/api/swine",     HTTP_GET,    handleSwine);
    server.on("/api/sdinfo",    HTTP_GET,    handleSDInfo);
    server.on("/wpasec/data",   HTTP_GET,    handleWpasecData);
    server.on("/wigle/data",    HTTP_GET,    handleWigleData);
    server.on("/info",          HTTP_GET,    handleInfo);

    // Legacy aliases.
    server.on("/list",          HTTP_GET,    handleFileList);
    server.on("/dl",            HTTP_GET,    handleDownload);
    server.on("/ul",            HTTP_POST,   handleUploadDone, handleUploadProcess);
    server.on("/rm",            HTTP_DELETE, handleDeleteLegacy);

    server.onNotFound(handleNotFound);
    routesConfigured = true;
}

// ==[ PUBLIC API ]==

void start() {
    if (running) return;

    txBytes = 0;
    rxBytes = 0;
    uploadCount = 0;
    downloadCount = 0;
    xpSessionAwarded = 0;
    running = false;

    // ==[ RADIO HANDOFF ]== keep the shared driver initialized. Full WiFi
    // teardown while NimBLE is warm produces coex un-init timeouts.
    WiFi.disconnect(false);
    delay(25);
    WiFi.mode(WIFI_AP);
    generatePassword();
    if (!WiFi.softAPConfig(AP_IP, AP_GW, AP_SN) ||
        !WiFi.softAP(AP_SSID, apPassword)) {
        HAMLET_LOGF("[XFER] AP start failed\n");
        WiFi.mode(WIFI_STA);
        Power::applyCurrentRadioSettings();
        NowFlock::markEspNowNeedsReinit();
        return;
    }
    Power::applyCurrentRadioSettings();
    delay(150);  // AP stabilize

    dns.start(53, "*", AP_IP);
    configureRoutes();
    server.begin();

    running = true;
}

void stop() {
    if (!running) return;
    server.stop();
    dns.stop();

    // Switch AP -> STA in-place. Dropping to WIFI_OFF deinitializes the shared
    // driver and races the still-warm NimBLE controller.
    WiFi.mode(WIFI_STA);
    Power::applyCurrentRadioSettings();
    delay(100);

    NowFlock::markEspNowNeedsReinit();
    running = false;

    // free XP tracking PSRAM
    if (xpWpaList) { heap_caps_free(xpWpaList); xpWpaList = nullptr; }
    xpWpaCount = 0; xpWpaLoaded = false;
    if (xpWigleList) { heap_caps_free(xpWigleList); xpWigleList = nullptr; }
    xpWigleCount = 0; xpWigleLoaded = false;
    xpSessionAwarded = 0;
}

void update() {
    if (!running) return;
    dns.processNextRequest();
    server.handleClient();
}

bool     isRunning()       { return running; }
uint8_t  getClientCount()  { return running ? (uint8_t)WiFi.softAPgetStationNum() : 0; }
uint32_t getTxBytes()      { return txBytes; }
uint32_t getRxBytes()      { return rxBytes; }
const char* getSSID()      { return AP_SSID; }
const char* getPassword()  { return apPassword; }
const char* getIP()        { return "192.168.4.1"; }
uint32_t getUploadCount()  { return uploadCount; }
uint32_t getDownloadCount(){ return downloadCount; }

} // namespace Xfer
