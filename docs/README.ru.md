<p align="center">
  <img src="banner.svg" alt="dvbt2mod — MPEG-TS в DVB-T2 CF32 IQ" width="100%">
</p>

<p align="center">
  <a href="https://github.com/cozycactus/dvbt2mod/actions/workflows/windows.yml"><img src="https://github.com/cozycactus/dvbt2mod/actions/workflows/windows.yml/badge.svg" alt="Windows x64"></a>
  <a href="https://github.com/cozycactus/dvbt2mod/releases/latest"><img src="https://img.shields.io/github/v/release/cozycactus/dvbt2mod?display_name=tag&sort=semver" alt="Последний релиз"></a>
  <a href="../LICENSE"><img src="https://img.shields.io/github/license/cozycactus/dvbt2mod" alt="GPL-3.0-or-later"></a>
</p>

<p align="center"><a href="../README.md">English</a></p>

# dvbt2mod

`dvbt2mod` — небольшой нативный C++ CLI для преобразования MPEG-TS в
DVB-T2 baseband IQ. Внутри используется полная передающая цепочка `gr-dtv`,
но не нужны Python, GNU Radio Companion, Qt, UHD, SoapySDR или SDR-устройство.

Вход — MPEG-TS с пакетами по 188 байт. Выход — raw CF32LE: чередующиеся
`float32 I, float32 Q`, 8 байт на комплексный отсчёт, без заголовка и
метаданных. Все параметры при запуске задаются десятичными числами.

## Готовый Windows x64

Скачайте `dvbt2mod-0.1.0-windows-x64.zip` из
[последнего релиза](https://github.com/cozycactus/dvbt2mod/releases/latest),
распакуйте архив и не отделяйте `dvbt2mod.exe` от лежащих рядом DLL. Для этого
portable-пакета устанавливать MSYS2 или GNU Radio не нужно.

```powershell
.\dvbt2mod.exe input.ts output.cf32 --profile 1
```

Файл пока не подписан сертификатом, поэтому SmartScreen может показать обычное
предупреждение. SHA-256 архива опубликован рядом в файле `.zip.sha256`:

```powershell
(Get-FileHash .\dvbt2mod-0.1.0-windows-x64.zip -Algorithm SHA256).Hash.ToLower()
```

Внутри архива находятся перечень точных версий runtime-пакетов, сторонние
лицензии, контрольные суммы каждого файла и сведения об исходном коде.
Отдельный проверенный архив
`dvbt2mod-0.1.0-windows-x64-corresponding-source.zip` содержит точную ревизию
проекта и полные MSYS2 source archives для каждого пакета с включёнными DLL.

## Запуск

Минимальный вызов использует профиль 1:

```console
dvbt2mod input.ts output.cf32 --profile 1
```

Существующий выходной файл не перезаписывается без явного числового разрешения:

```console
dvbt2mod input.ts output.cf32 --profile 1 --overwrite 1
```

Быстрый 4K-профиль для проверки:

```console
dvbt2mod input.ts output.cf32 --profile 9
```

Пути `-` означают stdin/stdout. Диагностика всегда идёт в stderr:

```console
dvbt2mod - - --profile 9 < input.ts > output.cf32
```

Чтобы зациклить небольшой TS и получить ровно один кадр профиля 9:

```console
dvbt2mod input.ts one-frame.cf32 --profile 9 --repeat 1 --samples 441344
```

При `--repeat 1 --samples 0` процесс работает непрерывно. Ctrl+C останавливает
flowgraph даже при ожидающем stdin или заблокированном stdout, закрывает выход
и сообщает число фактически записанных отсчётов. Проверить значения и построить
граф без модуляции можно через `--check 1`.

Полный актуальный список показывает `dvbt2mod --help`. Пример явной
конфигурации профиля 9:

```console
dvbt2mod input.ts output.cf32 \
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
вместимость FEC-блоков.

## Профили

| ID | Основа | FFT | QAM | code rate | GI | PP |
|---:|---|---:|---:|---:|---:|---:|
| 0 | GNU Radio `qa_dtv.py` golden | 4096 | 64 | 2/3 | 1/32 | 7 |
| 1 | GNU Radio `vv001-cr35` | 32768 T2-GI | 256 | 3/5 | 1/128 | 7 |
| 9 | GNU Radio `vv009-4kfft` | 4096 | 64 | 2/3 | 1/32 | 7 |

Профили 1 и 9 воспроизводят официальные flowgraph GNU Radio 3.10.12. Профиль 0
повторяет небольшой upstream regression vector и включает PAPR Tone
Reservation; он нужен прежде всего для воспроизводимой проверки.

Частота IQ для полос 5/6/7/8/10 MHz равна `bandwidth * 8 / 7`; для 1,7 MHz —
`131e6 / 71`. Запись в файл идёт настолько быстро, насколько позволяет CPU:
это не передача в реальном времени и не RF sink.

Вход должен быть MPEG-TS из полных 188-байтовых пакетов. Для обычного файла CLI
проверяет sync byte каждого пакета до открытия OUTPUT; stdin проверяется на
лету. После EOF добавляются канонические null packets PID `0x1FFF`, а только
синтетический последний пакет обрезается на точной границе BB/T2-кадра. Объём
дополнения виден как `null_padding_bytes` в stderr.

CLI не меняет muxrate. `--samples N` — низкоуровневый лимит raw IQ и потому
может оборвать последний T2-кадр. Для потока из целых кадров используйте
обычный конечный запуск или задавайте `N`, кратное размеру кадра (профиль 1:
1 983 488, профиль 9: 441 344 complex samples). При `--repeat 1` stdin запрещён,
поскольку поток нельзя перемотать.

Текущая граница реализации: DVB-T2 v1.1.1, SISO, один PLP, один RF, PAPR off
или Tone Reservation. MISO, T2-Lite, несколько PLP/RF, изменение muxrate и SDR
hardware sink пока не входят.

Полезные первичные источники: [описание DVB-T2 Modulator в GNU Radio](https://wiki.gnuradio.org/index.php?title=DVB-T2_Modulator),
[официальный QA-вектор GNU Radio 3.10.12](https://github.com/gnuradio/gnuradio/blob/v3.10.12.0/gr-dtv/python/dtv/qa_dtv.py#L28-L158)
и [ETSI EN 302 755 V1.1.1](https://www.etsi.org/deliver/etsi_EN/302700_302799/302755/01.01.01_60/EN_302755v010101p.pdf).

## Сборка

Нужны CMake 3.20+, Ninja, C++17 и GNU Radio 3.10.x с `gr-dtv`.

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

Получившийся executable не использует Python/GUI, но динамически связан с GNU
Radio. Сам файл остаётся небольшим; обычной source-сборке нужны ABI-совместимые
runtime-библиотеки. В Windows portable ZIP автоматически включается только его
фактическое транзитивное множество DLL.

Windows-сборка выполняется в MSYS2 UCRT64. Точный проверяемый рецепт лежит
в [`.github/workflows/windows.yml`](../.github/workflows/windows.yml), а аудит и
упаковка DLL — в [`scripts/package_windows.sh`](../scripts/package_windows.sh).

## Проверка

```sh
ctest --test-dir build --output-on-failure
python3 tests/golden_test.py build/dvbt2mod
```

Golden-тест скачивает два fixture из GNU Radio `v3.10.12.0`, проверяет их
SHA-256 и сравнивает каждый float. Эталонный результат: 52 736 complex samples,
421 888 байт, максимальная абсолютная ошибка `5.96046448e-7` при допуске `1e-5`.

## Лицензия

Код распространяется под [GPL-3.0-or-later](../LICENSE), как и используемые
DVB-T2 блоки GNU Radio. Portable-пакет содержит точные версии зависимостей,
сторонние лицензии, контрольные суммы и ревизию сборки, а соответствующий
исходный код публикуется отдельным полным архивом рядом с ним.
