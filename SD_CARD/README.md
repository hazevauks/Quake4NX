# SD_CARD

The test build, ready to copy to the root of the SD card. `tools/make_sd.py` fills this folder. Nothing but this file is
in git: the game's files are yours and stay on your machine.

```
switch/quake4/
  quake4.nro     the game
  q4base/        your copy of Quake 4's .pk4 files
  baseoq4/       openQ4's own pak0.pk4 and mod.json: its menus, fonts and strings. Your config and saves land here too
  SD_LOG/        made on the first start: logs and crash reports
```
