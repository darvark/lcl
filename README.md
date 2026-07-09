# Logger

Logger is an amateur radio logging application for entering QSOs, looking up DXCC information, and monitoring DXCluster spots.

The application uses shared controller/core logic, with Qt providing the user interface.


## Architecture

```mermaid
classDiagram
direction LR

class QtFrontend {
	+main()
	+LoggerQtWindow
}

class AppController {
	+app_controller_init()
	+app_controller_shutdown()
	+app_controller_get_render_state()
	+app_controller_handle_key()
	+app_controller_perform_cty_update()
	+app_controller_get_active_frequency_khz()
}

class QSOCore {
	+qso_init()
	+qso_add()
	+qso_add_fields()
	+qso_mark_invalid()
	+detect_band()
	+detect_mode()
}

class CatControl {
	+cat_init()
	+cat_connect()
	+cat_disconnect()
	+cat_get_frequency_khz()
	+cat_set_frequency_khz()
}

class Database {
	+db_init()
	+db_shutdown()
	+db_load_qsos()
	+db_insert_qso()
	+db_export_csv()
	+db_export_adif()
}

class CTYLookup {
	+cty_load()
	+cty_download_latest()
	+cty_lookup()
}

class DXCluster {
	+dxcluster_start()
	+dxcluster_stop()
	+dxcluster_set_status()
}

class CallSuggestion {
	+call_suggestion_refresh()
	+call_suggestion_apply()
}

class Statistics {
	+stats_update()
}

class Export {
	+export_csv()
	+export_adif()
}

class Maidenhead {
	+locator_to_latlon()
}

class Config {
	+config_load()
}

class ExternalData {
	<<database>> logger.db
	<<file>> logger.conf
	<<file>> wl_cty.dat
	<<service>> DXCluster server
}

QtFrontend --> AppController
AppController --> QSOCore
AppController --> CTYLookup
AppController --> DXCluster
AppController --> CatControl
AppController --> CallSuggestion
AppController --> Statistics
AppController --> Export
AppController --> Database

QSOCore --> CTYLookup
QSOCore --> Database
Statistics --> QSOCore
Export --> Database
Database --> ExternalData
Config --> ExternalData
CTYLookup --> ExternalData
DXCluster --> ExternalData
Maidenhead --> ExternalData
```


![lnx_logger](./lnx_logger.png "LNX Logger") 

## What it does

- Records QSOs from the Qt desktop UI
- Uses split entry fields: `call`, `rst`, `comments`
- Lets you set manual operating frequency by entering only digits in the call field
- Displays DXCC, CQ zone, and ITU zone information while typing a callsign
- Shows a dedicated callsign suggestions panel in the top-right corner with all matching history entries
- Connects to a DXCluster server, shows received spots in the cluster window, and stops the cluster worker cleanly when the app exits
- Tracks simple statistics
- Stores the QSO logbook and call history in SQLite
- Exports log data to CSV and ADIF files
- Exports contest logs to Cabrillo using a DXLog-like contest definition file
- Supports SO1R, SO2V, and SO2R operating techniques in configuration

## Features

- Split QSO entry with call/rst/comments and Space-based field cycling
- CAT-aware frequency handling (live rig frequency when connected, manual fallback)
- Optional CAT-aware mode handling from the rig's current operating mode
- Callsign history suggestions with multi-match list view (top-right panel)
- Local DXCC lookup from a CTY database
- DXCluster status and spot display, with a stop-safe shutdown path
- Invalid QSO marking for export exclusion
- CSV/ADIF export support, including comments and custom ADIF filename
- Cabrillo export support (`exportcab`) with category metadata from contest definition
- DXLog-like contest definition parser (`contest <file>`) with field declarations
- Dual Hamlib CAT profiles for SO2R (`CAT_*` + `CAT2_*`)
- One-key CTY database update from the internet
- SQLite-backed logbook and call-history storage with `LOGGER_DB_PATH` override
- New clean log action to truncate the current SQLite logbook and history
- Independent named logbooks stored in SQLite, with selection by ID or name
- New-log flow with optional contest preset selection in the Qt UI

## Requirements

- C compiler (GCC or Clang)
- CMake
- make
- pthread support
- curl or wget (for CTY database download)

Optional for GUI frontend:

- Qt Widgets development package (Qt 5 or Qt 6)
- Hamlib development package (for CAT support)

On Debian/Ubuntu systems, install the required packages with:

```bash
sudo apt-get update
sudo apt-get install -y build-essential cmake
```

## Build

From the project root:

```bash
cmake -S . -B build
cmake --build build
```

Executables are created in the build directory:

- logger (GUI)

## Regression Tests

The project includes a regression suite in `tests/regression` that validates
core non-UI behavior:

- configuration parsing and defaults
- CTY database loading and callsign lookup
- QSO parsing, band/mode detection, and invalid toggle behavior
- statistics aggregation
- CSV and ADIF export content
- Maidenhead locator conversion

Run the tests with:

```bash
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

## Unit Tests

The project also includes unit tests in `tests/unit` to verify exported
non-UI functions from core modules:

- `app_controller`: shared frontend-independent key/state flow used by the Qt frontend
- `config`: `config_load`
- `cty`: `cty_load`, `cty_lookup`
- `qso`: `qso_init`, `qso_add`, `qso_mark_invalid`, `detect_band`, `detect_mode`
- `stats`: `stats_update`
- `export`: `export_csv`, `export_adif`
- `maidenhead`: `locator_to_latlon`
- `dxcluster`: `dxcluster_set_status`

UI rendering itself is intentionally not covered by automated tests and should
be verified manually.

Run all tests (regression + unit):

```bash
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

## Run

```bash
cd build
./logger
```

## Notes

- The application is intended for desktop environments.

## Configuration

The application reads a configuration file named logger.conf from the working directory.

Example configuration:

```ini
LAT=21.104127
LON=37.300154
LOCATOR=AA00AA

DXC_HOST=dx.da0bcc.de
DXC_PORT=7300
DXC_CALL=AAXAAA
```

### Configuration fields

- LAT: latitude of your station
- LON: longitude of your station
- LOCATOR: Maidenhead locator of your station
- DXC_HOST: DXCluster hostname
- DXC_PORT: DXCluster TCP port
- DXC_CALL: your callsign used for cluster login
- STATION_CALL: station callsign used in contest exports
- OPERATOR_NAME: operator name used for metadata
- CONTEST_DEF_FILE: contest definition file (DXLog-like key/value format)
- CONTEST_TX_EXCHANGE: override TX exchange for contests with static EXCHANGE_SENT templates (for example ITU=28)
- CONTEST_TECHNIQUE: one of SO1R, SO2V, SO2R
- CAT2_MODEL/CAT2_DEVICE/CAT2_BAUD/...: second radio CAT profile for SO2R

Contest definition example:

```ini
NAME=CQ-WW-CW
CABRILLO_NAME=CQ-WW-CW
MODE=CW
CATEGORY_OPERATOR=SINGLE-OP
CATEGORY_BAND=ALL
CATEGORY_POWER=LOW
EXCHANGE_SENT=001
FIELD=SERIAL,Serial Number,required
```

Bundled contest presets (DxLog-style key/value files):

- `contest_defs/iaru_hf_championship.conf`
- `contest_defs/cq_ww_cw.conf`
- `contest_defs/cq_ww_ssb.conf`
- `contest_defs/cq_wpx_ssb.conf`
- `contest_defs/cq_wpx_cw.conf`

## Commands

While running the application, you can use these commands in the input line:

- export: write log.csv and log.adi
- export mylog.adi: write log.csv and custom ADIF file mylog.adi
- invalid: mark the most recent QSO as invalid so it is skipped by exports
- newlog / clear: create a new clean logbook and clear call-history suggestions
- newlog My Contest Name: create and switch to a new empty logbook with the given name
- In Qt, `F2` now asks for log name and contest preset in one flow; selecting `None` clears contest mode
- prevlog / openprev / previous: reopen the previous logbook snapshot from SQLite
- logs: list named log archives stored in SQLite
- openlog 12: open a named log archive by ID
- openlog My Contest Name: open the newest logbook that matches the given name
- quit: exit the program

## Function keys

- F1: help/status hint
- F2: create a new clean logbook and clear call history
- F3: reopen the previous logbook snapshot from SQLite
- F4: prompt for ADIF filename, then export CSV and ADIF using the entered ADIF name
- F5: show or hide the DXCluster window
- F6: recalculate statistics
- F7: download the latest wl_cty.dat and reload CTY entries
- F10: quit

## Entry workflow

- Normal mode input uses three fields: `Call`, `RST`, and `Comments`
- Contest mode input uses two fields only: `Call` and `Exchange`
- In contest mode, exchange label and validation come from the loaded contest definition (`FIELD=...`)
- In contest mode, the UI shows the current exchange to send (`TX <ExchangeLabel>`) derived from contest definition (`EXCHANGE_SENT`) and current QSO number
- In contest mode, you do not enter RST or comments manually
- In contest mode, the QSO table switches to contest-focused columns: `Exch S/R` and `Run/Pts`
- Press `Space` to move active cursor between visible input fields
- Press `Enter` to submit QSO when required fields are filled
- Type only digits in the call field and press `Enter` to set manual frequency in kHz (and rig frequency via CAT when connected)
- Log view displays RST as `sent/received` in the `RST S/R` column
	- sent defaults to `599` for CW
	- sent defaults to `59` for other modes
	- received is exactly the value entered in the RST field (normal mode) or exchange-derived default in contest mode

## Callsign suggestions

When you start typing the first token (callsign), Logger checks the SQLite-backed
call history and shows all matching callsigns in a separate window in the top-right corner.

- Suggestions are ordered by recency (newest first)
- Use Up/Down arrows to select a different suggested callsign
- Press Space to apply the currently selected suggestion and continue with the next field
- Press Tab to apply the currently selected suggestion
- Suggestions are shown only while editing the first token

## Data files

The program expects the DXCC database file named wl_cty.dat in the working directory or in the build directory.

When F7 is used, wl_cty.dat is downloaded and replaced in the current working directory.

The QSO logbook and call history are stored in `logger.db` by default. Set
`LOGGER_DB_PATH` to point at a different SQLite file if you want to keep the
database elsewhere. The first run imports existing `call_history.txt` entries
into SQLite if the database is empty.

`logger.conf` and `wl_cty.dat` remain text-based files.

## Notes

- The application uses Qt Widgets, so it is intended for desktop environments.
- DXCluster connectivity depends on the configured host, port, and network access.
- If you want to use a different DXCluster server, update DXC_HOST and DXC_PORT in logger.conf.
- Closing the application runs the shared shutdown path, which stops the DXCluster worker thread before the database is closed.
