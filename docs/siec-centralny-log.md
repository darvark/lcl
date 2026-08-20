# Praca Sieciowa w Topologii Gwiazdy z Centralnym Logiem

## Cel dokumentu

Dokument opisuje konkretna propozycje techniczna dla tego repo, aby dodac prace wielu stacji w sieci (topologia gwiazdy), gdzie:

- jedna instancja pelni role centralnego serwera logu,
- stacje klienckie synchronizuja swoje logi z serwerem,
- zachowana jest spojna numeracja seriali contestowych,
- system jest odporny na reconnect i duplikacje komunikatow.

Zakres dokumentu:

1. propozycja zmian schematu SQLite,
2. definicja protokolu komunikatow,
3. lista zmian funkcji i plikow source,
4. plan testow regresyjnych i sieciowych.

## Zalozenia projektowe

- Serwer centralny jest jedynym source of truth dla wspolnego logu.
- Stacja zachowuje lokalna baze SQLite jako cache i bufor offline (outbox).
- Synchronizacja jest operacyjna (log operacji), nie tabelaryczna (pelne zrzuty tabel).
- Kazda operacja jest idempotentna i ma globalne `op_id`.
- Numeracja seriali contestowych jest rezerwowana centralnie.
- Transport produkcyjny: TCP (UDP tylko ewentualnie do telemetrii, nie do krytycznej synchronizacji).

## 1. Propozycja zmian schematu SQLite

## 1.1 Rozszerzenia tabeli `qso`

Do tabeli `qso` dodajemy pola niezbedne do synchronizacji i rozstrzygania konfliktow:

- `qso_uid TEXT NOT NULL DEFAULT ''` - globalny identyfikator QSO (UUIDv4 generowany przy tworzeniu wpisu).
- `origin_station_id TEXT NOT NULL DEFAULT ''` - identyfikator stacji, ktora utworzyla QSO.
- `origin_station_seq INTEGER NOT NULL DEFAULT 0` - sekwencja lokalna stacji przy tworzeniu QSO.
- `last_op_id TEXT NOT NULL DEFAULT ''` - ostatnia operacja, ktora zmodyfikowala rekord.
- `last_modified_utc TEXT NOT NULL DEFAULT ''` - znacznik czasu modyfikacji (UTC, ISO-8601).
- `version INTEGER NOT NULL DEFAULT 1` - wersja rekordu do ochrony przed stale writes.

Indeksy:

- `CREATE UNIQUE INDEX IF NOT EXISTS idx_qso_qso_uid ON qso(qso_uid);`
- `CREATE INDEX IF NOT EXISTS idx_qso_last_modified ON qso(last_modified_utc);`
- `CREATE INDEX IF NOT EXISTS idx_qso_origin_station ON qso(origin_station_id, origin_station_seq);`

Uwagi:

- `id` lokalne pozostaje jako techniczne PK SQLite.
- W protokole i synchronizacji biznesowy klucz rekordu to `qso_uid`.

## 1.2 Nowe tabele synchronizacji

### `sync_identity`

Przechowuje tozsamosc tej instancji.

```sql
CREATE TABLE IF NOT EXISTS sync_identity (
  id INTEGER PRIMARY KEY CHECK (id = 1),
  station_id TEXT NOT NULL,
  station_name TEXT NOT NULL DEFAULT '',
  role TEXT NOT NULL DEFAULT 'client', -- client|server
  created_utc TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP
);
```

### `sync_cursors`

Przechowuje kursory synchronizacji.

```sql
CREATE TABLE IF NOT EXISTS sync_cursors (
  id INTEGER PRIMARY KEY CHECK (id = 1),
  last_pulled_global_seq INTEGER NOT NULL DEFAULT 0,
  last_acked_local_seq INTEGER NOT NULL DEFAULT 0,
  last_server_epoch TEXT NOT NULL DEFAULT ''
);
```

### `log_ops`

Kanoniczny dziennik operacji zastosowanych lokalnie (na serwerze pelna historia, na kliencie minimum do odtworzenia i debugowania).

```sql
CREATE TABLE IF NOT EXISTS log_ops (
  global_seq INTEGER PRIMARY KEY AUTOINCREMENT,
  op_id TEXT NOT NULL UNIQUE,
  station_id TEXT NOT NULL,
  station_seq INTEGER NOT NULL,
  logbook_id INTEGER NOT NULL,
  op_type TEXT NOT NULL, -- QSO_INSERT|QSO_SET_INVALID|QSO_SET_CONTEST_FIELDS|CALL_HISTORY_APPEND
  entity_id TEXT NOT NULL, -- np. qso_uid
  payload_json TEXT NOT NULL,
  op_utc TEXT NOT NULL,
  applied_utc TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP
);

CREATE INDEX IF NOT EXISTS idx_log_ops_station_seq
  ON log_ops(station_id, station_seq);
```

### `log_outbox`

Kolejka operacji oczekujacych na potwierdzenie serwera (u klienta).

```sql
CREATE TABLE IF NOT EXISTS log_outbox (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  op_id TEXT NOT NULL UNIQUE,
  station_seq INTEGER NOT NULL,
  logbook_id INTEGER NOT NULL,
  op_type TEXT NOT NULL,
  entity_id TEXT NOT NULL,
  payload_json TEXT NOT NULL,
  op_utc TEXT NOT NULL,
  status TEXT NOT NULL DEFAULT 'pending', -- pending|sent|acked|failed
  retry_count INTEGER NOT NULL DEFAULT 0,
  next_retry_utc TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP
);

CREATE INDEX IF NOT EXISTS idx_log_outbox_status_retry
  ON log_outbox(status, next_retry_utc);
```

### `serial_alloc`

Stan centralnego serwera numerow seriali per logbook.

```sql
CREATE TABLE IF NOT EXISTS serial_alloc (
  logbook_id INTEGER PRIMARY KEY,
  next_serial INTEGER NOT NULL DEFAULT 1,
  updated_utc TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP
);
```

### `serial_reservations`

Rezerwacje seriali dla stacji (do audytu i ewentualnego timeout/release).

```sql
CREATE TABLE IF NOT EXISTS serial_reservations (
  reservation_id TEXT PRIMARY KEY,
  logbook_id INTEGER NOT NULL,
  station_id TEXT NOT NULL,
  reserved_serial INTEGER NOT NULL,
  status TEXT NOT NULL DEFAULT 'reserved', -- reserved|consumed|released|expired
  reserved_utc TEXT NOT NULL,
  expires_utc TEXT NOT NULL,
  consumed_utc TEXT NOT NULL DEFAULT ''
);

CREATE INDEX IF NOT EXISTS idx_serial_reservations_lookup
  ON serial_reservations(logbook_id, station_id, status);
```

## 1.3 Migracje w `db.c`

W `ensure_open()` dodac:

- tworzenie nowych tabel,
- `ALTER TABLE qso ADD COLUMN ...` z warunkiem jak dla obecnych migracji,
- tworzenie indeksow,
- inicjalizacje rekordow singleton (`sync_identity`, `sync_cursors`).

## 1.4 Zasady integralnosci

- `qso_uid` musi byc niepuste dla nowych wpisow.
- Kazda operacja musi miec unikalne `op_id`.
- Powtorzona operacja `op_id` jest traktowana jako sukces idempotentny.
- `station_seq` musi byc monotoniczny w ramach `station_id`.

## 2. Definicja protokolu komunikatow

## 2.1 Transport i framing

- Transport: TCP.
- Kodowanie: UTF-8 JSON.
- Framing: `uint32_be length` + `length` bajtow JSON.
- Keepalive: heartbeat aplikacyjny co 5 s (konfigurowalne).

## 2.2 Envelope

Kazdy komunikat ma wspolny envelope:

```json
{
  "protocol_ver": 1,
  "msg_type": "HELLO",
  "msg_id": "7c74f0a4-1a27-4d9f-a2a2-4dd6bba7e449",
  "station_id": "STN-A",
  "logbook_id": 3,
  "sent_utc": "2026-08-20T19:01:15Z",
  "payload": {}
}
```

## 2.3 Typy komunikatow

### 1) `HELLO`

Cel: rozpoczecie sesji i negocjacja parametrow.

Payload:

```json
{
  "station_name": "RUN1",
  "role": "client",
  "last_pulled_global_seq": 10234,
  "last_local_station_seq": 876,
  "capabilities": ["qso_ops", "serial_reservation"]
}
```

### 2) `HELLO_ACK`

Payload:

```json
{
  "server_epoch": "2026-08-20T00:00:00Z#1",
  "accepted": true,
  "next_expected_station_seq": 877,
  "server_global_seq": 10250,
  "heartbeat_sec": 5
}
```

### 3) `APPEND_OPS`

Klient wysyla paczke lokalnych operacji (outbox).

```json
{
  "ops": [
    {
      "op_id": "f3e1...",
      "station_id": "STN-A",
      "station_seq": 877,
      "op_type": "QSO_INSERT",
      "entity_id": "qso:8b9d...",
      "op_utc": "2026-08-20T19:03:00Z",
      "payload": {
        "qso_uid": "8b9d...",
        "call": "SP6ABC",
        "freq": 14022,
        "mode": "CW",
        "rst": "599",
        "exchange_sent": "001",
        "exchange_recv": "074",
        "radio_nr": 1,
        "points": 3,
        "invalid": false
      }
    }
  ]
}
```

### 4) `APPEND_ACK`

```json
{
  "accepted_ops": ["f3e1..."],
  "rejected_ops": [
    {
      "op_id": "...",
      "code": "SEQ_GAP",
      "message": "expected station_seq=877 got 879"
    }
  ],
  "last_acked_station_seq": 877,
  "server_global_seq": 10251
}
```

### 5) `PULL_OPS`

Pobranie zmian z serwera.

```json
{
  "from_global_seq_exclusive": 10234,
  "max_ops": 500
}
```

### 6) `PULL_OPS_RESP`

```json
{
  "ops": [
    {
      "global_seq": 10235,
      "op_id": "...",
      "op_type": "QSO_SET_INVALID",
      "entity_id": "qso:8b9d...",
      "payload": { "invalid": true },
      "op_utc": "2026-08-20T19:04:00Z"
    }
  ],
  "last_global_seq": 10251,
  "has_more": false
}
```

### 7) `RESERVE_SERIAL`

```json
{
  "request_id": "2c33...",
  "requested_for_qso_uid": "8b9d...",
  "ttl_sec": 120
}
```

### 8) `RESERVE_SERIAL_ACK`

```json
{
  "request_id": "2c33...",
  "reservation_id": "rsv-...",
  "serial": 142,
  "expires_utc": "2026-08-20T19:06:00Z"
}
```

### 9) `COMMIT_SERIAL`

```json
{
  "reservation_id": "rsv-...",
  "qso_uid": "8b9d..."
}
```

### 10) `ERROR`

```json
{
  "code": "AUTH_FAILED",
  "message": "station not allowed in this logbook",
  "retryable": false
}
```

### 11) `HEARTBEAT`

```json
{
  "server_global_seq": 10251,
  "last_seen_station_seq": 877
}
```

## 2.4 Semantyka i reguly

- `op_id` zapewnia idempotencje.
- `station_seq` wykrywa luki i out-of-order per stacja.
- `global_seq` porzadkuje caly dziennik serwera.
- Konflikty aktualizacji QSO:
  - preferowane: `version` optimistic lock,
  - fallback: `last_modified_utc` + `station_id` tie-breaker.
- Operacja typu toggle nie jest przesylana; przesylamy stan docelowy (`invalid=true/false`).

## 2.5 Bezpieczenstwo (MVP vs docelowo)

MVP (LAN):

- bez TLS,
- allowlist `station_id` w config serwera,
- osobny port sieci lokalnej.

Docelowo:

- TLS (mTLS lub token HMAC),
- rotacja kluczy,
- jawne wersjonowanie protokolu i kompatybilnosc wsteczna.

## 3. Lista zmian funkcji w plikach source

Poniżej jest konkretna mapa zmian dla istniejacego kodu.

## 3.1 `src/db.c` / `src/db.h`

### Rozszerzyc

- `ensure_open()`
  - migracje kolumn `qso_uid`, `origin_station_id`, `origin_station_seq`, `last_op_id`, `last_modified_utc`, `version`;
  - tworzenie tabel: `sync_identity`, `sync_cursors`, `log_ops`, `log_outbox`, `serial_alloc`, `serial_reservations`.

- `db_insert_qso(const QSO*, long long*)`
  - wymagac `qso_uid` dla nowych wpisow;
  - ustawic metadata origin/version;
  - opcjonalnie zapisac odpowiadajaca operacje `QSO_INSERT` do `log_outbox`.

- `db_update_qso_invalid(long long id, int invalid)`
  - aktualizowac `version`, `last_op_id`, `last_modified_utc`;
  - odkaldac `QSO_SET_INVALID` do `log_outbox`.

- `db_update_qso_contest_fields(...)`
  - analogicznie odkaldac `QSO_SET_CONTEST_FIELDS`.

- `db_load_qsos(...)`
  - ladowac nowe pola synchronizacyjne do `QSO`.

### Dodac nowe API

- `int db_sync_get_or_create_station_id(char *out, size_t out_size);`
- `int db_sync_next_station_seq(long long *out_seq);`
- `int db_sync_outbox_enqueue(...);`
- `int db_sync_outbox_mark_sent(const char *op_id);`
- `int db_sync_outbox_mark_acked(const char *op_id);`
- `int db_sync_pull_pending(...);`
- `int db_sync_apply_remote_op(...);`
- `int db_sync_get_last_global_seq(long long *out_seq);`
- `int db_sync_set_last_global_seq(long long seq);`
- `int db_serial_reserve_local_cache(...);` (opcjonalnie)

## 3.2 `src/qso.h` / `src/qso.c`

### Struktura `QSO`

Dodac pola:

- `char qso_uid[40];`
- `char origin_station_id[32];`
- `long long origin_station_seq;`
- `int version;`

### Funkcje do modyfikacji

- `qso_add_fields(...)` i `qso_add_contest_fields(...)`
  - generacja `qso_uid` przy tworzeniu;
  - pobranie `station_id` i `station_seq`;
  - dla contestu wymagajacego seriali: rezerwacja serialu przez `net_sync` przed zapisem.

- `qso_mark_invalid(int index)`
  - wysylac docelowy stan invalid (nie toggle-op) przez warstwe sync.

## 3.3 `src/app_controller_runtime.inc`

### Inicjalizacja i shutdown

- `app_controller_init()`
  - uruchomic `net_sync_start()` po `db_init()/qso_init()`.

- `app_controller_shutdown()`
  - `net_sync_stop()` przed `db_shutdown()`.

### Komendy operatora

Rozszerzyc `process_command(...)` o:

- `net on|off`
- `net status`
- `net sync` (manualny catch-up)
- `net role server|client`

## 3.4 `src/config.h` / `src/config.c`

Dodac pola konfiguracyjne:

- `NET_ENABLED=0|1`
- `NET_ROLE=server|client`
- `NET_STATION_ID=RUN1`
- `NET_SERVER_HOST=...`
- `NET_SERVER_PORT=...`
- `NET_HEARTBEAT_SEC=5`
- `NET_RETRY_MIN_MS=200`
- `NET_RETRY_MAX_MS=5000`
- `NET_TLS=0|1` (future flag)
- `NET_SHARED_KEY=...` (opcjonalnie dla MVP)

## 3.5 Nowe pliki

- `src/net_protocol.h` / `src/net_protocol.c`
  - serializacja/deserializacja ramek,
  - walidacja `msg_type`, `protocol_ver`, wymaganych pol.

- `src/net_sync.h` / `src/net_sync.c`
  - worker klienta i/lub serwera,
  - reconnect, heartbeat, outbox flush, pull loop,
  - metryki i status do UI.

- `src/net_server.h` / `src/net_server.c` (jesli rozdzielimy role)
  - accept loop,
  - sesje klientow,
  - walidacja sekwencji i ack.

- `src/uuid.h` / `src/uuid.c`
  - generator UUIDv4 w C.

## 3.6 UI i status

- `src/app_controller.h` + frontend Qt (`qt_frontend_main_window.inc`)
  - dodac status lacza: `NET ON/OFF`, `SYNC OK/LAG`, liczba pending ops,
  - pokazac blad typu `SERIAL RESERVATION FAILED`.

## 3.7 Testy i CMake

- `tests/unit/unit_tests.c`
  - testy API sync w DB,
  - testy idempotencji.

- `tests/regression/regression_tests.c`
  - scenariusze wielostacyjne i reconnect.

- `CMakeLists.txt`
  - dolaczyc nowe pliki `net_*.c`, `uuid.c`.

## 4. Plan testow regresyjnych i sieciowych

## 4.1 Testy jednostkowe

1. Migracje DB
   - start na starej bazie bez nowych kolumn,
   - potwierdzenie dodania kolumn/tabel,
   - brak utraty danych historycznych.

2. Idempotencja operacji
   - dwukrotne `QSO_INSERT` z tym samym `op_id` i `qso_uid`,
   - oczekiwany wynik: pojedynczy rekord QSO.

3. Sekwencje stacji
   - gap (`1,2,4`) -> odrzucenie z `SEQ_GAP`,
   - duplikat (`2` drugi raz) -> `already applied`.

4. Aktualizacje QSO
   - `QSO_SET_INVALID`,
   - `QSO_SET_CONTEST_FIELDS`,
   - inkrementacja `version` i aktualizacja `last_modified_utc`.

5. Rezerwacje seriali
   - 1000 kolejnych rezerwacji bez kolizji,
   - timeout rezerwacji,
   - consume/release.

## 4.2 Testy integracyjne (2-4 stacje)

1. Happy path
   - STN-A i STN-B loguja naprzemiennie,
   - po sync kazda stacja ma identyczny zestaw QSO.

2. Reconnect klienta
   - STN-B odlaczona, STN-A loguje 50 QSO,
   - STN-B wraca i wykonuje `PULL_OPS`,
   - wynik: pelny catch-up, brak duplikatow.

3. Retry/outbox
   - chwilowy brak serwera,
   - operacje zostaja `pending`,
   - po powrocie serwera outbox czyszczony do `acked`.

4. Konflikt modyfikacji
   - ten sam `qso_uid` modyfikowany na 2 stacjach,
   - weryfikacja polityki konfliktu (version/LWW).

5. Serial server
   - 2 stacje prosza jednoczesnie o serial,
   - brak duplikacji numerow,
   - zgodnosc seriala zalogowanego i wyslanego.

## 4.3 Testy odporności

1. Utrata pakietow / opoznienia (symulacja)
   - duze RTT, jitter, chwilowe timeouty,
   - brak uszkodzenia danych, eventual consistency.

2. Restart serwera
   - klienci reconnect,
   - odtworzenie kursora i kontynuacja global_seq.

3. Restart klienta w trakcie wysylki
   - outbox persystentny,
   - po starcie kontynuacja od `pending/sent`.

## 4.4 Testy regresyjne funkcji istniejacych

Uruchamiac pelny zestaw obecnych testow po kazdym etapie:

```bash
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

Do tego dodac testy manualne:

- eksport CSV/ADIF/Cabrillo po synchronizacji,
- DXCluster nadal dziala niezaleznie,
- tworzenie/otwieranie named logbookow dziala bez regresji.

## 4.5 Kryteria akceptacji

- Brak utraty QSO przy reconnect i restartach.
- Brak duplikatow QSO przy powtornej dostawie komunikatow.
- Spojna numeracja seriali miedzy stacjami.
- Czas catch-up po reconnect (1000 operacji) < 5 s w LAN.
- Wszystkie istniejace testy przechodza bez regresji.

## Plan wdrozenia etapami

1. Etap A (MVP LAN)
   - `QSO_INSERT`, `APPEND_OPS`, `PULL_OPS`, idempotencja, podstawowy status sieci.

2. Etap B
   - `QSO_SET_INVALID`, `QSO_SET_CONTEST_FIELDS`, pelny outbox/retry.

3. Etap C
   - centralna rezerwacja seriali (`RESERVE_SERIAL`).

4. Etap D
   - hardening: auth/TLS, limity, metryki, fault-injection CI.

## Poza zakresem (na teraz)

- Federacja wielu serwerow centralnych.
- Rozproszony consensus (Raft itp.).
- Synchronizacja przez UDP jako kanal krytyczny.

## 6. Status implementacji (2026-08-20)

Zrealizowano pelny pakiet 5 etapow wdrozenia technicznego (bez czekania na kroki posrednie):

1. Etap 1: DB sync apply/pull
- Dodano API i implementacje dla zdalnego apply i pull operacji:
  - `db_sync_apply_remote_op(...)`
  - `db_sync_pull_ops(...)`
- Dodano strukture `SyncLogOpEntry`.
- Outbox payload dla `QSO_INSERT`, `QSO_INVALID`, `QSO_CONTEST` zostal rozszerzony do pelnej migawki QSO (`kind=qso_full`) zamiast payloadu minimalnego.

2. Etap 2: centralny serwer
- Dodano nowy modul:
  - `src/net_server.h`
  - `src/net_server.c`
- Serwer obsluguje: `APPEND_OPS`, `PULL_OPS`, `RESERVE_SERIAL`, `COMMIT_SERIAL`, `HEARTBEAT`.
- Serwer pracuje juz na framed transporcie (`uint32_be + JSON`), obsluguje `HELLO/HELLO_ACK` oraz walidacje tokenu/shared-key.
- Gdy `NET_TLS=1` i build ma OpenSSL, polaczenie klient-serwer przechodzi przez rzeczywista warstwe TLS przed wymiana ramek protokolu.
- Serwer utrzymuje trwale pliki self-signed (`NET_TLS_CERT_FILE`, `NET_TLS_KEY_FILE`) zamiast generowac nowy certyfikat przy kazdym starcie.
- Klient moze pinowac fingerprint certyfikatu serwera przez `NET_TLS_PEER_FINGERPRINT`; przy pustej wartosci pierwszy udany handshake zapisuje fingerprint do aktywnego `logger.conf`.
- Sesja przechowuje rzeczywisty `station_id` klienta zamiast stalego `"remote"`.
- Serwer jest uruchamiany w trybie `NET_ROLE=server` przez `net_sync_start()`.

3. Etap 3: klient pull+apply
- Rozszerzono klienta sync (`src/net_sync.c`) o:
  - odbior `PULL_OPS_RESP`,
  - apply zdalnych operacji do lokalnej bazy,
  - aktualizacje kursora `last_global_seq` po pull.
- Dodano handshake `HELLO -> HELLO_ACK`, framed send/recv, interpretacje `APPEND_ACK` oraz konserwatywne ustawianie `connected=1` dopiero po poprawnej wymianie.
- Retry/backoff i heartbeat korzystaja z ustawien runtime (`NET_RETRY_MIN_MS`, `NET_RETRY_MAX_MS`, `NET_HEARTBEAT_SEC`).
- `NetSyncStatus` i `net status` pokazuja dodatkowo stan TLS, reconnect count, failure streak oraz znaczniki ostatniego heartbeat/sukcesu sync.
- Klient respektuje tez `NET_MAX_FRAME_BYTES`, a serwer egzekwuje `NET_RATE_LIMIT_WINDOW_SEC`, `NET_RATE_LIMIT_BURST` i maksymalny rozmiar ramki.

4. Etap 4: serial reservation
- Dodano API DB:
  - `db_sync_reserve_serial(...)`
  - `db_sync_commit_serial(...)`
- Dodano protokolowe encode/decode:
  - `RESERVE_SERIAL`
  - `RESERVE_SERIAL_ACK`
  - `COMMIT_SERIAL`
- W `qso_add_contest_fields(...)` dodano probe centralnej rezerwacji serialu w trybie klienta (`NET_ENABLED=1`, `NET_ROLE=client`) gdy `exchange_sent` jest puste/zerowe.
- Dodano housekeeping wygasania rezerwacji (`db_sync_expire_serial_reservations()`) wykonywany okresowo w workerze runtime.

5. Etap 5: runtime i komendy operatora
- Dodano cykliczny worker poll w runtime (`app_controller_runtime.inc`) oparty o `NET_SYNC_INTERVAL_MS`.
- Dodano komendy:
  - `net on`
  - `net off`
  - `net status`
  - `net sync`
  - `net role client|server`
- Komendy `sync` i `syncstatus` pozostaly wspierane.
- Dodano wsparcie konfiguracyjne dla: `NET_STATION_ID`, `NET_SHARED_KEY`, `NET_HEARTBEAT_SEC`, `NET_RETRY_MIN_MS`, `NET_RETRY_MAX_MS`, `NET_TLS`.
- Konfiguracja sieci obejmuje tez: `NET_TLS_CERT_FILE`, `NET_TLS_KEY_FILE`, `NET_TLS_PEER_FINGERPRINT`, `NET_RATE_LIMIT_WINDOW_SEC`, `NET_RATE_LIMIT_BURST`, `NET_MAX_FRAME_BYTES`.

## 6.1 Rozszerzenia protokolu

W `src/net_protocol.h/.c` dodano:

- nowe typy wiadomosci:
  - `NET_MSG_HELLO_ACK`
  - `NET_MSG_PULL_OPS_RESP`
  - `NET_MSG_RESERVE_SERIAL`
  - `NET_MSG_RESERVE_SERIAL_ACK`
  - `NET_MSG_COMMIT_SERIAL`
  - `NET_MSG_APPEND_ACK`
- nowe API:
  - `net_protocol_encode_hello_ack(...)`
  - `net_protocol_parse_hello_meta(...)`
  - `net_protocol_parse_hello_ack(...)`
  - `net_protocol_send_framed(...)`
  - `net_protocol_recv_framed(...)`
  - `net_protocol_encode_pull_ops_resp(...)`
  - `net_protocol_parse_pull_ops_resp(...)`
  - `net_protocol_parse_append_ops(...)`
  - `net_protocol_encode_reserve_serial(...)`
  - `net_protocol_encode_reserve_serial_ack(...)`
  - `net_protocol_parse_reserve_serial(...)`
  - `net_protocol_parse_commit_serial(...)`

## 6.2 CMake i testy

- `src/net_server.c` zostal dolaczony do targetow: `logger`, `unit_tests`, `regression_tests`.
- Dodano testy jednostkowe obejmujace:
  - parse/encode `APPEND_OPS` i `PULL_OPS_RESP`,
  - handshake `HELLO/HELLO_ACK` i framed I/O,
  - roundtrip klient-serwer z `NET_TLS=1`,
  - fingerprint pinning dla self-signed TLS,
  - server-side rate limiting,
  - fault-injection: `drop APPEND_ACK`, opozniony `PULL_OPS_RESP`, duplicate `APPEND_OPS`,
  - `db_sync_apply_remote_op` + idempotencja,
  - `db_sync_pull_ops`,
  - roundtrip serwera dla `APPEND_OPS/PULL_OPS`,
  - rezerwacje i commit seriali,
  - komendy runtime `net on/off/role/status`.
- Rozszerzono regresje `config_load/config_save` o nowe pola sieciowe i hardeningowe.

## 6.3 Wynik walidacji

Po wdrozeniu:

- build: przechodzi,
- `unit_tests`: przechodza,
- `regression_tests`: przechodza,
- wczesniejszy pakiet 5 historycznych faili testow srodowiskowo-konkursowych zostal usuniety przez stabilniejsze rozwiazywanie sciezek runtime (`logger.conf`, `wl_cty.dat`, `contest_defs/*`).

Otwarte swiadome braki po tej iteracji:

- TLS pozostaje na modelu self-signed + fingerprint pinning; nie ma jeszcze mTLS ani zewnetrznego PKI.
- Rate limiting jest per-sesja/proces i nie ma jeszcze centralnych statystyk ani blacklistingu.
- Fault-injection pokrywa scenariusze testowe w unitach, ale nie ma osobnego runnera chaos/network do CI WAN.
