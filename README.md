# Ghost Master: Resurrection — DLSS SR + Frame Generation (ReShade add-on)

C++/DirectX 12 аддон ReShade для Ghost Master: Resurrection (UE 5.5.1, Steam app 3289090, build 25511106, exe SHA256 `0e5e8471808be706d119f37da90c84ecf291c9e91613e5a5f4e9bd4caa8ae4e9`). Подменяет временной проход TAAU игры результатом NVIDIA DLSS Super Resolution и добавляет DLSS Frame Generation 2x через NVIDIA Streamline. Исходники игры не нужны.

**Состояние (2026-10-26): релиз 1.0.0 собран и проверен в игре**, готов к загрузке на Nexus Mods (`release\GhostMasterDLSS-1.0.0.zip`). Подробная история с доказательствами — `PROGRESS.md`; краткая точка входа для новой сессии — `CLAUDE.md`.

## Что проверено в игре (RTX 4090, вывод 4K)

- DLSS SR вместо TAAU: тысячи кадров замены, нормальная картинка и UI (подтверждено пользователем).
- Смена разрешения и масштаба на лету: DLSS пересоздаётся под новый размер без перезапуска.
- Процент рендера из меню аддона: применяется сразу и с первого кадра запуска (запись `r.ScreenPercentage`).
- DLSS Frame Generation: 2 выведенных кадра на 1 отрисованный; на уровне ~175–210 FPS на экране против ~105–115 без генерации; артефактов UI не замечено (пользователь).
- Нагрузка аддона на CPU ~2–3 мс/кадр суммарно по потокам (было ~118 мс до переписывания отслеживания дескрипторов).
- Релизная сборка 1.0.0 из архива: аддон «Ghost Master DLSS» v1.0.0.0, DLSS и FG работают, чистое завершение.

## Установка в игру

Пользователям — архив релиза (см. `release-src\README.txt`, он же лежит в архиве). Из этой рабочей папки, при закрытой игре:

```powershell
.\tools\install-addon.ps1      # аддон, ghostmaster_fg.dll, ini (если ещё нет) и подписанный рантайм NVIDIA в Win64\streamline
.\tools\uninstall-addon.ps1    # удаляет аддон, ghostmaster_fg.dll и папку streamline (-RemoveSettings — ещё ini и логи)
```

Установщик проверяет хеш exe, наличие ReShade (`dxgi.dll`), подписи NVIDIA и сохраняет конфигурацию папки игры и список файлов с SHA256 в `backups\before-install-*`. Нужен ReShade 6.8+ с полной поддержкой аддонов (у пользователя установлен 6.8.0, API 20).

Аддоны DLSS 5 (`dlss5-feed`, `renodx-dlss5`) убраны из игры по просьбе пользователя в `disabled-addons\dlss5\`.

## Меню и управление

Home → вкладка **DLSS SR** (та же панель есть под записью аддона во вкладке Add-ons):

- DLSS вкл/выкл (F6), строка статуса (F7, сохраняется);
- разрешение рендера: «Game slider» (ползунок Resolution Quality игры) или «Set here» — Quality 66.7 / Balanced 58 / Performance 50 / Ultra Performance 33.3 % и ползунок 33.3–99 %;
- генерация кадров вкл/выкл и статус;
- пресет модели DLSS (default, J, K, L, M), применяется сразу;
- текущие размеры входа/выхода и FPS.

Настройки `ghostmaster_dlss.ini` (рядом с аддоном): `Mode` (replace/off/shadow), `ShowStatus`, `Preset`, `ScaleSource` (addon/game), `ScreenPercentage`, `FrameGeneration` (0 — Streamline вообще не грузится), `Diagnostics` (дампы шейдеров и захват команд, по умолчанию 0), `RuntimeDir`, `StreamlineDir`, `LogDir` (пусто — по умолчанию; относительные пути считаются от папки аддона). DLSS SR и FG используют один подписанный `nvngx_dlss.dll` 310.9.16 из `Win64\streamline` — в чистой игре своего `nvngx_dlss.dll` нет.

Логи: `Win64\ghostmaster-dlss\capture.jsonl` (события, раз в 2 с `perf` с FPS и полями `fg_*`), `ghostmaster-dlss\streamline\frame_generation.jsonl`, `ghostmaster-dlss\streamline\sl.log`.

## Как это устроено (кратко)

- `src/probe.cpp` — аддон ReShade: отслеживание дескрипторов без блокировок, перехват двух точных хешей шейдера TAAU (`fc434178cfb8e489`, `0f1634b465b59707`), чтение констант, меню, `r.ScreenPercentage`, связь с FG.
- `src/live_bridge.cpp` — подготовка входов (цвет, глубина, векторы движения), вызов DLSS, копирование результата в выход TAAU, восстановление состояния command list, смена разрешения.
- `src/dlss_backend.cpp` — NGX DLSS SR.
- `src/frame_generation.cpp` → `ghostmaster_fg.dll` — Streamline в режиме manual hooking: перехват слота vtable настоящей `IDXGIFactory2::CreateSwapChainForHwnd`, цепочка игра → ReShade → прокси Streamline → DXGI; DLL закрепляется в памяти, потому что ReShade выгружает аддоны между пробными устройствами.
- Ключевые адреса и раскладки (`r.ScreenPercentage`, буфер `View` UE) — в `PROGRESS.md` и комментариях `probe.cpp`.

## Тестовый лаунчер (без установки в игру)

`Start-DLSS.cmd` / `tools\launch-live.ps1` запускают игру с изолированным профилем ReShade в `run\` (только наш аддон, Streamline из `external\Streamline\bin\x64`), процент рендера задаётся через `run\addons\ghostmaster_dlss.ini`. `Start-Baseline.cmd` — тот же запуск, но аддон только измеряет FPS (для сравнения). Флаги лаунчера: `-Mode`, `-ScreenPercentage`, `-Width/-Height`, `-Fullscreen`, `-Diagnostics`, `-Baseline`. Капчуры — `captures\live-*`.

## Сборка, тесты, релиз

Требуются VS 2022 Build Tools (C++), Windows SDK, CMake (из VS) и зависимости в `external\` (в репозиторий не входят):

| Папка | Откуда |
|---|---|
| `external\reshade` | `git clone https://github.com/crosire/reshade` на commit `7bf9de8b33bcc76c3177007e65d73c72dd0f34c0` (нужны `include` и `deps\imgui`) |
| `external\DLSS` | [NVIDIA/DLSS](https://github.com/NVIDIA/DLSS) v310.9.16 (`include`, `lib\Windows_x86_64`, `LICENSE.txt`) |
| `external\Streamline` | `streamline-sdk-v2.14.16.zip` из [релизов NVIDIA-RTX/Streamline](https://github.com/NVIDIA-RTX/Streamline/releases/tag/v2.14.16), распакованный (`include`, `bin\x64`, `license.txt`) |

```powershell
.\tools\build.ps1
.\build\RelWithDebInfo\bridge_smoke.exe .\external\DLSS\lib\Windows_x86_64\rel .\reports\bridge-smoke   # DLSS-мост со сменой разрешения
.\build\RelWithDebInfo\sl_smoke.exe .\external\Streamline\bin\x64 .\reports\sl-smoke                     # DLSS-G через модуль FG (откроет окно)
.\tools\make-release.ps1                                                                                  # архив и тексты в release\
```

NGX/Streamline-тесты нужно запускать без песочницы (в песочнице NVIDIA Telemetry зависает при завершении). `sl_smoke` нужно окно в фокусе — DLSS-G не работает в окне без фокуса. Версия релиза — `src\version.rc`; тексты и ini релиза — `release-src\`; `make-release.ps1` проверяет совпадение версии и подписи NVIDIA и пишет `release\SHA256SUMS.txt`.

## Ограничения

- Привязан к одной сборке игры; после обновления игры аддон не найдёт проход TAAU и DLSS станет неактивным.
- Только два исследованных шейдера TAAU; другой метод сглаживания в настройках игры может выбрать иной вариант.
- 100 % (DLAA) не предлагается — игра тогда использует другой проход сглаживания.
- Проверено только на RTX 4090; HDR, мультимонитор, другие поколения GPU не проверены.
- Генерация кадров работает только при активном DLSS и окне в фокусе; при выключении DLSS через F6 в `sl.log` появляются ошибки «Failed to find global tag» (безвредно).
- В панели NVIDIA у пользователя глобально принудительно включён VSync — выводимый FPS ограничен частотой монитора; игровой лимит `MaxFPS` (Settings.sav) ограничивает только отрисованные кадры.

## Зависимости и лицензии

- [ReShade](https://github.com/crosire/reshade), commit `7bf9de8b33bcc76c3177007e65d73c72dd0f34c0` (заголовки API и ImGui); в релиз не входит.
- [NVIDIA DLSS SDK](https://github.com/NVIDIA/DLSS) 310.9.16, commit `9981c6350dc58c0ce21f024fd65e31e851c31689`; лицензия NVIDIA RTX SDK (`external\DLSS\LICENSE.txt`), разрешает распространение рантайма в составе приложения с уведомлением.
- [NVIDIA Streamline](https://github.com/NVIDIA-RTX/Streamline) 2.14.16 (zip SHA256 `32041e51db0cab709a271b72a352281b690c2abbd669a827809401a5dd23956b`); MIT.
- [Dear ImGui](https://github.com/ocornut/imgui), commit `3912b3d9a9c1b3f17431aebafd86d2f40ee6e59c`; MIT.

This software contains source code provided by NVIDIA Corporation.
