# Self-Signed TLS dla Central Log

## Cel

Dokument opisuje operacyjny model TLS dla obecnej implementacji centralnego logu. Model jest celowo prosty:

- serwer utrzymuje lokalny self-signed certyfikat i klucz,
- klient pinuje fingerprint certyfikatu serwera,
- dodatkowa autoryzacja aplikacyjna dalej opiera sie na `NET_SHARED_KEY` / `NET_AUTH_TOKEN`.

## Pliki i ustawienia

W `logger.conf` obsluzone sa pola:

- `NET_TLS=1`
- `NET_TLS_CERT_FILE=logger_net_cert.pem`
- `NET_TLS_KEY_FILE=logger_net_key.pem`
- `NET_TLS_PEER_FINGERPRINT=`
- `NET_RATE_LIMIT_WINDOW_SEC=1`
- `NET_RATE_LIMIT_BURST=32`
- `NET_MAX_FRAME_BYTES=65536`

Znaczenie:

- `NET_TLS_CERT_FILE`: sciezka PEM certyfikatu serwera.
- `NET_TLS_KEY_FILE`: sciezka PEM klucza prywatnego serwera.
- `NET_TLS_PEER_FINGERPRINT`: pin SHA-256 certyfikatu serwera po stronie klienta.

## Pierwszy start serwera

Przy pierwszym uruchomieniu z `NET_TLS=1`:

1. serwer sprawdza istnienie `NET_TLS_CERT_FILE` i `NET_TLS_KEY_FILE`,
2. jezeli ich nie ma, generuje self-signed certyfikat RSA 2048,
3. zapisuje oba pliki w formacie PEM,
4. uzywa ich przy kolejnych startach.

To oznacza stabilny fingerprint, o ile pliki nie zostana usuniete lub podmienione.

## Pierwszy start klienta

Przy pierwszym udanym handshake TLS klient:

- odczytuje fingerprint certyfikatu serwera,
- jezeli `NET_TLS_PEER_FINGERPRINT` jest puste, zapisuje odczytany fingerprint do aktywnego `logger.conf`.

To zachowuje model TOFU (trust on first use).

## Kolejne polaczenia klienta

Przy nastepnych polaczeniach:

- klient porownuje otrzymany fingerprint z `NET_TLS_PEER_FINGERPRINT`,
- mismatch konczy handshake bledem i sesja sync nie startuje.

## Rotacja certyfikatu

Jesli certyfikat serwera musi zostac zrotowany:

1. zatrzymac serwer,
2. podmienic lub usunac `NET_TLS_CERT_FILE` i `NET_TLS_KEY_FILE`,
3. uruchomic serwer,
4. odczytac nowy fingerprint,
5. zaktualizowac `NET_TLS_PEER_FINGERPRINT` na klientach.

## Ograniczenia modelu

- brak mTLS,
- brak CRL/OCSP,
- brak zewnetrznego CA,
- TOFU wymaga zaufania do pierwszego zestawienia polaczenia.

## Rekomendacja operacyjna

Dla deploymentu klubowego/LAN:

- trzymac pliki cert/key poza katalogami tymczasowymi,
- zbackupowac PEM-y razem z konfiguracja serwera,
- po pierwszym uruchomieniu zweryfikowac i zamrozic fingerprint na klientach,
- nie polegac wylacznie na TLS: zostawic ustawiony `NET_SHARED_KEY`.