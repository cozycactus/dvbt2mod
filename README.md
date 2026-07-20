# dvbt2mod

`dvbt2mod` — небольшой нативный C++ CLI для преобразования MPEG-TS в
DVB-T2 baseband IQ. Внутри используется полная передающая цепочка `gr-dtv`,
но не нужны Python, GNU Radio Companion, Qt, UHD или Soapy.

Вход — MPEG-TS с пакетами по 188 байт. Выход на проверенной Intel-сборке —
raw CF32LE: чередующиеся `float32 I, float32 Q`, 8 байт на комплексный
отсчёт, без заголовка и метаданных.

## Сборка

Нужны CMake, Ninja, C++17 и GNU Radio 3.10.x с `gr-dtv`.

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

Получившийся `build/dvbt2mod` — нативный executable без Python/GUI. Проверенная
здесь сборка — macOS x86_64. Сам Mach-O занимает около 115 КБ, но динамически
связан с GNU Radio: это тонкий CLI, а не автономный one-file bundle. На другом
компьютере нужна ABI-совместимая GNU Radio 3.10 и её runtime-библиотеки.

## Запуск

Минимальный вызов использует профиль 1:

```sh
./build/dvbt2mod input.ts output.cf32 --profile 1
```

Существующий выходной файл не перезаписывается без явного числового разрешения:

```sh
./build/dvbt2mod input.ts output.cf32 --profile 1 --overwrite 1
```

Быстрый 4K-профиль для проверки:

```sh
./build/dvbt2mod input.ts output.cf32 --profile 9
```

Пути `-` означают stdin/stdout. Диагностика всегда идёт в stderr:

```sh
cat input.ts | ./build/dvbt2mod - - --profile 9 >output.cf32
```

Потоковый режим реализован через POSIX file descriptors с прерываемыми
`poll/read/write`; именованные INPUT/OUTPUT должны быть обычными файлами.

Чтобы зациклить небольшой TS и получить ровно один кадр профиля 9:

```sh
./build/dvbt2mod input.ts one-frame.cf32 \
  --profile 9 --repeat 1 --samples 441344
```

При `--repeat 1 --samples 0` процесс работает непрерывно; SIGINT/SIGTERM
останавливает flowgraph даже при ожидающем stdin или заблокированном stdout,
закрывает выход и сообщает число фактически записанных отсчётов.
Проверить значения и построить граф без модуляции можно через `--check 1`.

Все параметры модуляции задаются числами. Полный актуальный список показывает
`./build/dvbt2mod --help`. Пример явной конфигурации профиля 9:

```sh
./build/dvbt2mod input.ts output.cf32 \
  --profile 9 \
  --bandwidth-khz 8000 \
  --fft 4096 \
  --t2gi 0 \
  --qam 64 \
  --rate-num 2 --rate-den 3 \
  --guard-num 1 --guard-den 32 \
  --pilot 7 \
  --fec-blocks 31 \
  --ti-blocks 3 \
  --data-symbols 100 \
  --frame-size 64800 \
  --l1-qam 16 \
  --t2-frames 2 \
  --rotation 1 \
  --extended 0 \
  --high-efficiency 0 \
  --papr-tr 0 \
  --repeat 0 \
  --overwrite 0
```

Числа не приводятся напрямую к внутренним enum GNU Radio: CLI принимает
физические значения и проверяет их по белым спискам. Проверяются сочетания
FFT/GI/pilot для SISO, специальный T2-GI enum, длительность кадра до 250 мс и
вместимость FEC-блоков. Это важно, потому что порядок внутренних DVB-T2 FFT
enum не совпадает с размером FFT, а сама GNU Radio многие плохие сочетания
только логирует и продолжает.

## Профили

| ID | Основа | FFT | QAM | code rate | GI | PP |
|---:|---|---:|---:|---:|---:|---:|
| 0 | GNU Radio `qa_dtv.py` golden | 4096 | 64 | 2/3 | 1/32 | 7 |
| 1 | GNU Radio `vv001-cr35` | 32768 T2-GI | 256 | 3/5 | 1/128 | 7 |
| 9 | GNU Radio `vv009-4kfft` | 4096 | 64 | 2/3 | 1/32 | 7 |

Профили 1 и 9 воспроизводят официальные flowgraph GNU Radio 3.10.12. Профиль 0
повторяет маленький upstream regression vector и включает PAPR Tone
Reservation; он нужен прежде всего для воспроизводимой проверки.

Частота IQ для полос 5/6/7/8/10 MHz равна `bandwidth * 8 / 7`; для 1,7 MHz —
`131e6 / 71`. Запись в файл идёт настолько быстро, насколько позволяет CPU:
это не передача в реальном времени и не RF sink.

Вход должен быть MPEG-TS из полных 188-байтовых пакетов. Для обычного файла CLI
проверяет sync byte каждого пакета до открытия OUTPUT; stdin проверяется на
лету. При обычном конечном входе он
сохраняет весь хвост: после EOF добавляет канонические null packets PID
`0x1FFF` и обрезает только синтетический последний пакет на точной границе
BB/T2-кадра. Объём дополнения виден как `null_padding_bytes` в stderr.

CLI не меняет muxrate. `--samples N` — низкоуровневый лимит raw IQ и потому
может оборвать последний T2-кадр; это намеренный режим захвата. Для потока из
целых кадров используйте обычный конечный запуск или задавайте `N`, кратное
показанному размеру кадра (профиль 1: 1 983 488, профиль 9: 441 344 complex
samples). При `--repeat 1` stdin запрещён, поскольку поток нельзя перемотать.

Текущая граница реализации: DVB-T2 v1.1.1, SISO, один PLP, один RF, PAPR off
или Tone Reservation. Это не универсальный production exciter: MISO,
T2-Lite, несколько PLP/RF и SDR hardware sink сюда пока не входят.

## Проверка

Быстрый локальный тест:

```sh
ctest --test-dir build --output-on-failure
```

Официальный GNU Radio golden-вектор (скрипт скачивает два fixture из tag
`v3.10.12.0`, проверяет их SHA-256 и сравнивает каждый float):

```sh
python3 tests/golden_test.py build/dvbt2mod
```

На проверенной машине результат: 52 736 complex samples, 421 888 байт,
максимальная абсолютная ошибка против upstream `5.96046448e-7` при допуске
`1e-5`.

## Лицензия

Код распространяется под GPL-3.0-or-later, как и используемые DVB-T2 блоки
GNU Radio. При распространении бинарника нужно соблюдать GPL и предоставить
соответствующий исходный код.
