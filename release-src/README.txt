Ghost Master DLSS 1.0.0
=======================
NVIDIA DLSS Super Resolution + DLSS Frame Generation for Ghost Master: Resurrection (DirectX 12).
A ReShade add-on. Русская версия ниже.


WHAT IT DOES
------------
- Replaces the game's own temporal upscaler (TAAU) with NVIDIA DLSS Super Resolution.
- Adds DLSS Frame Generation (2x) through NVIDIA Streamline.
- In-game settings tab: DLSS on/off, render resolution (Quality / Balanced / Performance /
  Ultra Performance or any value from 33 to 99 %), DLSS model preset, Frame Generation on/off.
- Render resolution can be changed while playing, no restart needed.

Tested: RTX 4090, 4K output, Windows 10, Steam build 25511106 (executable from 2026-10-01).
Example on a level at 4K, 58 % render scale: about 105-115 FPS without and about 175-210 FPS
shown with Frame Generation.


REQUIREMENTS
------------
- Ghost Master: Resurrection, Steam version, build 25511106. On other builds the add-on finds no
  supported upscaling pass and DLSS stays inactive; the game keeps its own TAAU.
- NVIDIA RTX GPU. DLSS Super Resolution: RTX 20 series or newer.
  Frame Generation: RTX 40 / RTX 50 series only.
- Windows 10/11 with "Hardware-accelerated GPU scheduling" enabled (needed for Frame Generation):
  Settings > System > Display > Graphics > Change default graphics settings.
- A recent NVIDIA driver.
- ReShade 6.8.0 or newer WITH FULL ADD-ON SUPPORT (https://reshade.me, the "with full add-on
  support" download). Tested with ReShade 6.8.0.
- Microsoft Visual C++ 2015-2022 Redistributable (x64). Usually already installed with the game.


INSTALLATION
------------
1. Install ReShade with full add-on support for
   ...\Ghost Master Resurrection\GhostMasterRemake\Binaries\Win64\GhostMasterRemake-Win64-Shipping.exe
   Choose DirectX 10/11/12. You do not need to install any effects.
2. Extract this archive into the game folder (the folder that contains "GhostMasterRemake").
   The files must end up in GhostMasterRemake\Binaries\Win64:
     ghostmaster_dlss.addon64
     ghostmaster_fg.dll
     ghostmaster_dlss.ini
     streamline\  (NVIDIA Streamline and DLSS runtime files)
3. Start the game. Press Home to open the ReShade overlay and select the "DLSS SR" tab.

Remove other upscaling / frame generation mods for this game first (OptiScaler, DLSS 5 / RenoDX
DLSS add-ons, other Streamline injectors). They hook the same parts of the renderer.


USAGE
-----
- Home: ReShade overlay, "DLSS SR" tab with all settings. Changes are saved to ghostmaster_dlss.ini.
- F6: DLSS on/off (the game's own TAAU when off).
- F7: show/hide the small status line (off by default).
- Render resolution: "Game slider" keeps the game's Resolution Quality setting; "Set here" uses the
  value from the tab and applies it from the first frame of every launch.
- Frame Generation works only while DLSS is active and the game window has focus.
  NVIDIA Control Panel "Vertical sync: On" limits shown FPS to the monitor refresh rate.


SETTINGS FILE (ghostmaster_dlss.ini)
------------------------------------
Mode=replace            replace = DLSS on, off = game's TAAU
ShowStatus=0            1 = status line in the top-left corner
Preset=default          DLSS model: default, J, K, L, M
ScaleSource=addon       addon = ScreenPercentage below, game = game's slider
ScreenPercentage=66.7   render resolution in percent (33.3 - 99)
FrameGeneration=1       0 = do not load Streamline / Frame Generation at all
Diagnostics=0           1 = large research logs, slower; leave at 0
RuntimeDir, StreamlineDir, LogDir   empty = defaults (relative paths are relative to the add-on folder)


TROUBLESHOOTING
---------------
- Nothing in the overlay: ReShade is not the "full add-on support" version, or the files are in
  the wrong folder.
- "DLSS SR: disabled - ..." : the reason is shown; usually a missing or modified file in .\streamline.
- "Render scale control unavailable": different game build; use the game's own slider instead.
- Frame Generation stays inactive: check GPU (RTX 40/50), hardware-accelerated GPU scheduling,
  window focus, and that DLSS shows as active.
- Logs: GhostMasterRemake\Binaries\Win64\ghostmaster-dlss\ (capture.jsonl, streamline\sl.log,
  streamline\frame_generation.jsonl). Please attach them to bug reports.
- To turn Frame Generation off completely: FrameGeneration=0 in ghostmaster_dlss.ini.


UNINSTALL
---------
Delete from GhostMasterRemake\Binaries\Win64: ghostmaster_dlss.addon64, ghostmaster_fg.dll,
ghostmaster_dlss.ini and the folders "streamline" and "ghostmaster-dlss".


KNOWN LIMITATIONS
-----------------
- Bound to one game build; a game update can disable the mod until it is updated.
- Only tested on an RTX 4090. Not tested: HDR output, multi-monitor, other GPU generations.
- 100 % render scale (DLAA) is not offered: the game then switches to a different anti-aliasing
  pass that the add-on does not replace.
- No separate UI buffer is passed to Frame Generation; no UI artifacts were seen in testing.


CREDITS AND LICENSES
--------------------
- NVIDIA DLSS (nvngx_dlss.dll, nvngx_dlssg.dll) - NVIDIA RTX SDK license, see streamline\NVIDIA_RTX_SDK_LICENSE.txt
- NVIDIA Streamline (sl.*.dll) - MIT license, see streamline\Streamline_LICENSE.txt
- ReShade by crosire - not included, installed separately.
- Dear ImGui (via ReShade's add-on API) - MIT.
This software contains source code provided by NVIDIA Corporation.
This mod is not made, endorsed or supported by NVIDIA or by the developers/publishers of the game.
NVIDIA, RTX and DLSS are trademarks of NVIDIA Corporation.


=====================================================================================
РУССКИЙ
=====================================================================================

Что делает
----------
- Заменяет штатный временной апскейлер игры (TAAU) на NVIDIA DLSS Super Resolution.
- Добавляет генерацию кадров DLSS Frame Generation (2x) через NVIDIA Streamline.
- Вкладка настроек в игре: DLSS вкл/выкл, разрешение рендера (Quality / Balanced / Performance /
  Ultra Performance или любое значение 33-99 %), пресет модели DLSS, генерация кадров вкл/выкл.
- Разрешение рендера меняется прямо во время игры, без перезапуска.

Проверено: RTX 4090, вывод 4K, Windows 10, Steam-сборка 25511106 (exe от 2026-10-01).
Пример на уровне в 4K при 58 %: около 105-115 FPS без генерации и около 175-210 FPS на экране с ней.

Требования
----------
- Ghost Master: Resurrection, Steam, сборка 25511106. На других сборках аддон не находит
  поддерживаемый проход апскейла, DLSS остаётся неактивным, игра работает со своим TAAU.
- Видеокарта NVIDIA RTX. DLSS Super Resolution: RTX 20 и новее. Генерация кадров: только RTX 40 / 50.
- Windows 10/11, включено «Планирование графического процессора с аппаратным ускорением»
  (нужно для генерации кадров).
- Свежий драйвер NVIDIA.
- ReShade 6.8.0 или новее С ПОЛНОЙ ПОДДЕРЖКОЙ АДДОНОВ (версия «with full add-on support» с reshade.me).
- Microsoft Visual C++ 2015-2022 Redistributable (x64), обычно уже стоит вместе с игрой.

Установка
---------
1. Установите ReShade с полной поддержкой аддонов на
   ...\Ghost Master Resurrection\GhostMasterRemake\Binaries\Win64\GhostMasterRemake-Win64-Shipping.exe
   (DirectX 10/11/12, эффекты не нужны).
2. Распакуйте архив в папку игры (ту, где лежит папка «GhostMasterRemake»). Файлы должны оказаться в
   GhostMasterRemake\Binaries\Win64: ghostmaster_dlss.addon64, ghostmaster_fg.dll,
   ghostmaster_dlss.ini и папка streamline.
3. Запустите игру, нажмите Home и откройте вкладку «DLSS SR».

Перед установкой удалите другие моды апскейла и генерации кадров для этой игры (OptiScaler,
аддоны DLSS 5 / RenoDX, другие инжекторы Streamline).

Управление
----------
- Home: оверлей ReShade, вкладка «DLSS SR» со всеми настройками (сохраняются в ghostmaster_dlss.ini).
- F6: DLSS вкл/выкл. F7: строка статуса вкл/выкл (по умолчанию выключена).
- «Game slider» — разрешение задаёт ползунок игры; «Set here» — значение из вкладки, применяется с
  первого кадра при каждом запуске.
- Генерация кадров работает, только пока активен DLSS и окно игры в фокусе. Принудительный VSync в
  панели NVIDIA ограничивает FPS на экране частотой монитора.

Если не работает
----------------
- В оверлее ничего нет: ReShade без полной поддержки аддонов или файлы не в той папке.
- «DLSS SR: disabled - ...» — указана причина, чаще всего нет файла в папке streamline.
- «Render scale control unavailable» — другая сборка игры, пользуйтесь ползунком игры.
- Генерация кадров не включается: проверьте видеокарту (RTX 40/50), аппаратное планирование GPU,
  фокус окна и что DLSS активен.
- Логи: GhostMasterRemake\Binaries\Win64\ghostmaster-dlss\. Прикладывайте их к сообщениям об ошибках.
- Полностью отключить генерацию кадров: FrameGeneration=0 в ghostmaster_dlss.ini.

Удаление
--------
Удалите из GhostMasterRemake\Binaries\Win64: ghostmaster_dlss.addon64, ghostmaster_fg.dll,
ghostmaster_dlss.ini и папки streamline и ghostmaster-dlss.
