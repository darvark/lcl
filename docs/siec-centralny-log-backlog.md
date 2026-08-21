# Centralny Log - Backlog Implementacyjny

Data przygotowania: 2026-08-20
Status: dokument roboczy do dalszej implementacji

## Cel

Lista brakujacych lub niepelnych funkcjonalnosci dla pracy wielostanowiskowej z centralnym logiem.
Dokument ma sluzyc jako praktyczna checklista wdrozeniowa.

## Priorytet P0 (krytyczne dla spojnosci danych)

- [x] Dodac twarde wykrywanie konfliktu station_seq dla tej samej station_id przy innym op_id.
  - Oczekiwane zachowanie: serwer odrzuca konflikt i zwraca czytelny blad.
  - Dotyczy: src/net_server.c, src/db.c

- [x] Dodac ograniczenie unikalnosci station_id + station_seq po stronie dziennika operacji.
  - Oczekiwane zachowanie: baza nie pozwala zapisac dwoch roznych operacji o tej samej parze (station_id, station_seq).
  - Dotyczy: src/db.c (indeksy/migracja)

- [x] Ujednolicic semantyke commit seriali: po zarezerwowaniu numeru klient musi wysylac COMMIT_SERIAL po zapisie QSO.
  - Oczekiwane zachowanie: rezerwacje nie pozostaja w stanie reserved bez potrzeby.
  - Dotyczy: src/qso.c, src/net_sync.c, src/net_protocol.c, src/net_protocol.h

## Priorytet P1 (sprawnosc synchronizacji i zgodnosc z dokumentem projektu)

- [x] Dodac komunikat OP_BROADCAST (server -> client) i obsluge push zmian do aktywnych klientow.
  - Oczekiwane zachowanie: klient otrzymuje zmiany bez czekania na kolejny poll.
  - Dotyczy: src/net_protocol.c, src/net_protocol.h, src/net_server.c, src/net_sync.c

- [x] Dodac obsluge CATCHUP_REQUEST / CATCHUP_BATCH jako jawne typy protokolu (lub zaktualizowac dokumentacje, jesli PULL_OPS zostaje docelowo).
  - Oczekiwane zachowanie: jednoznaczna warstwa catch-up i zgodnosc nazewnictwa protokolu.
  - Dotyczy: src/net_protocol.c, src/net_protocol.h, src/net_sync.c, src/net_server.c, docs/siec-centralny-log-projekt.md

- [x] Dodac walidacje protocol_version po obu stronach oraz zwracanie dedykowanego bledu ERROR_UNSUPPORTED_PROTOCOL.
  - Oczekiwane zachowanie: kontrolowane zerwanie sesji przy niezgodnej wersji protokolu.
  - Dotyczy: src/net_protocol.c, src/net_server.c, src/net_sync.c

- [x] Dodac jitter (+/-20%) do mechanizmu backoff.
  - Oczekiwane zachowanie: mniejsze ryzyko efektu thundering herd po reconnect.
  - Dotyczy: src/net_sync.c

## Priorytet P2 (UX operatora i komendy)

- [x] Dodac status po zapisie QSO z informacja o kolejce sync (np. SYNC:PENDING).
  - Oczekiwane zachowanie: operator widzi od razu, czy wpis czeka na wysylke.
  - Dotyczy: src/qso.c, src/app_controller_runtime.inc

- [x] Dodac komunikat offline z rozmiarem kolejki (np. NET OFFLINE - local queue: N).
  - Oczekiwane zachowanie: czytelna diagnostyka w czasie braku lacznosci.
  - Dotyczy: src/app_controller_runtime.inc

- [x] Dodac aliasy/komendy zgodne z dokumentem projektu:
  - netsync on|off
  - netsync status
  - netsync catchup
  - netserver start|stop
  - Dotyczy: src/app_controller_runtime.inc

## Priorytet P3 (hardening i WAN-ready)

- [ ] Rozszerzyc TLS do modelu mTLS lub integracji z zewnetrznym PKI.
  - Dotyczy: src/net_tls.c, src/net_sync.c, src/net_server.c, logger.conf, docs

- [ ] Rozszerzyc rate limiting o centralne statystyki, telemetrie i mechanizm blokad/blacklist.
  - Dotyczy: src/net_server.c, src/db.c, docs

- [ ] Dodac dedykowany runner fault-injection/chaos (profil WAN) do CI.
  - Dotyczy: tests/, CMakeLists.txt, pipeline CI (poza repo lub w repo)

## Testy do dopisania/rozszerzenia

- [ ] Test konfliktu station_seq (ten sam station_seq, rozne op_id).
- [ ] Test protocol_version mismatch -> ERROR_UNSUPPORTED_PROTOCOL.
- [ ] Test reserve + commit serial (pelny cykl) oraz cleanup po czasie TTL.
- [ ] Test push OP_BROADCAST do 2 klientow bez aktywnego pull.
- [ ] Test reconnect z jitter backoff i kontrola opoznien.
- [ ] Test komend operatorskich netsync/netserver.

## Sugerowana kolejnosc pracy

1. P0: konflikt station_seq + unikalny indeks + COMMIT_SERIAL end-to-end.
2. P1: protocol_version + bledy + OP_BROADCAST/catch-up.
3. P2: statusy operatorskie i komendy.
4. P3: hardening WAN, mTLS, telemetry/rate-limit, chaos CI.

## Uwagi organizacyjne

- Po kazdym zadaniu uruchamiac: build + unit_tests + regression_tests.
- Po wdrozeniach protokolu aktualizowac dokumentacje techniczna, aby uniknac rozjazdu kod vs docs.
- Dla zmian w DB dodawac migracje zgodne wstecznie i test migracyjny.
