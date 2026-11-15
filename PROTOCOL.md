# Protokół PiggyProg ↔ Digital ECU Cheater I (firmware 1.5.3.17)

Odczytany z zapisów komunikacji `trace-20261115-1052.log` i `trace-20261115-1101.log`
(`./piggyprog --trace`) oraz z opisu czynności `opis-czynnosci.txt`. Stan: **wstępny**.

## Warstwa fizyczna

FT232R → `/dev/ttyUSB0`, **111000 bodów, 8N1**, DTR i RTS ustawione. PiggyProg ustawia
limity czasu odczytu 5000 ms i zapisu 1000 ms.

## Ramka

```
02  LEN  CMD  payload[LEN]  ff 03
```

`LEN` to liczba bajtów danych, bez `CMD`. Nie ma sumy kontrolnej. Odpowiedzi sterownika
mają ten sam format.

## Komendy (PC → ECU)

| CMD | ASCII | Payload | Odpowiedź | Znaczenie |
|---|---|---|---|---|
| `56` | V | — | `68` (h) + ASCII `1.5.3.17` | wersja firmware, PiggyProg ponawia ją też przy ponownym łączeniu |
| `54` | T | — | `66` (f), 176 bajtów | odczyt bloku konfiguracji |
| `4d` | M | `hi lo` | `67` (g) `hi lo` + 16 B (strona `05 00`: 32 B) | odczyt pamięci map, krok 0x10 |
| `59` | Y | `hi idx` + 16 B | brak | zapis kolumny mapy (`idx` = kolumna obr/min 0–15, 16 wartości od najniższego kPa) |
| `53` | S | `off` + 1–N B | brak | zapis bajtów konfiguracji od offsetu `off` w bloku z `T` |
| `42` | B | `01 ff` | brak | **Permanent**: zapis do pamięci nieulotnej |

- **„Save” w PiggyProgu zapisuje tylko plik** na dysku i nic nie wysyła do sterownika.
- **W trybie TUNING każda zmiana pola od razu wysyła ramkę `Y` albo `S`**, zanim użytkownik kliknie „Send”.
- **„Send … to ECU” wysyła wszystkie kolumny albo wszystkie parametry jeszcze raz.**

## Dane na żywo

Sterownik **sam nadaje** ramki `65` (e) z 36 bajtami danych co ok. 46 ms. PC o nie nie prosi.
Mapowanie tych bajtów na 26 kolumn pliku `.lgo` jest **do ustalenia**.

## Pamięć map (odczyt `M`, strony po 16 B)

| hi | zawartość | uwagi |
|---|---|---|
| `00` | 256 B, w zapisie same `0x80` | prawdopodobnie mapa korekty zapłonu (`ign`, 0x80 = 0) |
| `01` | **VE**, 16×16 | odczyt wiersz po wierszu: `01 00` = 20 kPa, `01 f0` = 103 kPa; zapis `Y` kolumnami |
| `02` | **boost / IAC**, 16×16 | `02 00` = 20 kPa: `2d 2d 1f 1f 00…` = 45 45 31 31 |
| `03` | 16×16, zera | prawdopodobnie `anl` |
| `04` | ? | |
| `05` | osie, słowa 16-bit big-endian | `00 14 00 19 00 1f …` = 20, 25, 31 … kPa; dalej obr/min? |

Odczyt i zapis mają **różny układ**: `M 01 00` zwraca wiersz 20 kPa, a `Y 01 00` zapisuje
kolumnę 700 obr/min. Potwierdzone na polu VE 103 kPa / 6500 obr/min: kolumna `Y 01 0f`
kończy się wartością 110, a po cofnięciu zmiany 109.

## Konfiguracja (blok `T`, 176 B, zapis `S off val`)

- offset `0x3e` = **VssConst** (zmiana 77 → 78 → 77 widoczna w zapisie);
- PiggyProg przy „Send CONFIG” wysyła ok. 68 ramek `S`. Część z nich ma wiele bajtów (tabele CLT, IAT, IacRefPos…);
- przypisanie offsetów do kluczy `config.cfg` jest **do ustalenia**: trzeba porównać ramki `S` z wartościami w pliku.

## Kodowanie parametrów ustalone zmianą w PiggyProg (15.11.2026, `trace-20261115-1159.log`)

| offset | parametr | kodowanie |
|---|---|---|
| 0x00 | IgnitionType | 0–3 bez przeliczenia (0 = single signal / ignition module) |
| 0x01 | SigPer720 | indeks z config.cfg + 1 (= liczba sygnałów; widziane 1, 4, 10) |
| 0x02 | TriggInputType | indeks + 1 (Hall = 3) |
| 0x03 | CylinderNo | indeks + 1 (= liczba cylindrów 1–8) |
| 0x18 / 0x19 | LeanLimit / RichLimit | bez przeliczenia |
| 0x31 | LcRpm | obr/min ÷ 100 |
| 0x32 | LcIgn | −5,0° → 127 (0° → 0); jeden punkt, skala nieustalona |
| 0x7b | ignMaxRetAdv | 0–50 bez przeliczenia (wcześniej błędnie przypisany do BoostControlByte) |
| 0xa3 | BitFieldA | bit 0x04 = Smart fuel saving |
| 0xa9 | SmartFuelSavingLimit | bez przeliczenia |
| 0xab | IdleIndicator | 0–2 (2 = always idle) |
| 0xad | RevLimitHysteresis | obr/min bez przeliczenia (200 → 200) |
| 0xaf | IacAdditionalAfterstartOpen | bez przeliczenia |
| 0x0b | tpsType | 0/1 |

Trzecia sesja (`trace-20261115-1229.log`, `opis-czynnosci3.txt`):

| offset | parametr | kodowanie |
|---|---|---|
| 0x04 | InjectionDiv | indeks listy: 0 = ×2, 1 = /1, 2 = /2, … |
| 0x08 | InjectionType | 0 = two groups, 1 = one group |
| 0x1c | TpsAccelAmount | surowo; w GUI ms = wartość / 31,25 (maks. 8,16) |
| 0x24 | TpsAccelTime | ms ÷ 50 (450 → 9, 4400 → 88); jednostka w config.cfg nieznana |
| 0x32 | LcIgn | surowo; w GUI kąt = wartość / 25,5 − 10° (0 → −10,0°, 255 → 0,0°, krok 0,2°) |
| 0x77 | FuelAlgorithm | 0 = Speed density, 1 = Alpha N |
| 0x7c | BoostTargetTab | 16 słów little-endian (pierwsza komórka 5 → `05 00`) |
| 0x9c | BoostControlByte | bit 0x01 = closed loop, 0x02 = valve reverse |
| 0x9d–0xa2 | BoostMaxFeedbackInc/Dec, Kp, Ki, Kd, IntegralLimit | o bajt dalej niż wcześniej zakładano |
| 0xa3 | BitFieldA | 0x04 = Smart fuel saving, 0x10 = No injection divider on idle |
| 0xae | IacWhenNotInIdle | bez przeliczenia |

Cały blok (176 B) jest przypisany. Nieustalona pozostaje tylko jednostka TpsAccelTime w config.cfg;
nieprzetestowane przy wartościach ≠ 0: IacCrankingPosition, parametry regulatora boost.

## Do zrobienia przed zapisem z własnego programu

1. Odczytać pełne strony bezpośrednio z portu. Zapis z nakładki ucina linie przy 256 B, więc część odpowiedzi `M` jest niepełna.
2. Ustalić strukturę stron `00`, `03`, `04` i `05` oraz mapowanie ramki `e` na kolumny `.lgo`.
3. Zbudować mapę offsetów konfiguracji na podstawie zapisu „Send CONFIG” i `config.cfg`.
4. Sprawdzić, czy sterownik w jakikolwiek sposób potwierdza `Y`, `S` i `B`. Na razie nic nie odsyła, więc po każdym zapisie trzeba odczytać dane z powrotem i porównać.
